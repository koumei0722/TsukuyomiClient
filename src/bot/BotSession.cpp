#include "BotSession.h"
#include "Possess.h"
#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <numbers>
#include <utility>

namespace tsukuyomi::bot {
namespace {
constexpr float kWalkPerTick = 0.2158f;
constexpr float kEyeHeight = 1.62f;
constexpr std::int32_t kJumpFlags[] = {3, 6, 16, 31, 60, 61};
constexpr double kMaxPathDistance = 30000;
constexpr double kMaxPathRadius = 60000;
double distance(protocol::Vec3 a, protocol::Vec3 b)
{
    return std::hypot(double(a.x) - b.x, double(a.y) - b.y, double(a.z) - b.z);
}
double horizontalDistance(protocol::Vec3 a, protocol::Vec3 b) { return std::hypot(double(a.x) - b.x, double(a.z) - b.z); }
bool finite(protocol::Vec3 p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }
std::string coordinates(protocol::Vec3 p) { return std::format("({}, {}, {})", p.x, p.y, p.z); }
bool blockPosition(protocol::Vec3 p)
{
    constexpr double low = (std::numeric_limits<std::int32_t>::min)(), high = (std::numeric_limits<std::int32_t>::max)();
    return finite(p) && double(p.x) >= low && double(p.x) <= high && double(p.y) > low && double(p.y) <= high
        && double(p.z) >= low && double(p.z) <= high;
}
}
const char* stateName(State state)
{
    switch (state) {
    case State::Idle: return "Idle";
    case State::Preparing: return "Preparing";
    case State::LoggingIn: return "LoggingIn";
    case State::Spawning: return "Spawning";
    case State::Spawned: return "Spawned";
    case State::Disconnected: return "Disconnected";
    }
    return "Unknown";
}
void BotSession::reset()
{
    state_ = State::Idle;
    runtimeId_.reset(); startedAt_.reset(); uniqueId_ = 0;
    spawnReceived_ = false; loginReceived_ = false; chunkUnreadable_ = false;
    outgoing_.clear(); events_.clear();
    movable_ = false; onGround_ = false; teleported_ = false; dead_ = false;
    position_ = {}; dimension_.reset(); pitch_ = 0; yaw_ = 0;
    tick_ = 0; nextTickAt_ = 0; respawnAt_.reset();
    spawnSerial_ = 0;
    clearActions(); inventory_ = {}; armor_ = {}; offhand_ = {}; cursor_ = {};
    inventorySerial_ = 0; lastStackResponseAt_ = 0; lastPickupAt_ = 0; resyncWanted_ = false; stackResponses_.clear();
    selectedSlot_ = 0; cannotBuild_.clear();
    following_ = false; leader_.reset(); leaderDimension_.reset(); followRetryAt_ = 0; followWaitingNoted_ = false; notes_.clear();
    inLiquid_ = false; jumpHeld_ = false;
    inventoryStale_ = false; resyncSentAt_.reset();
}
bool BotSession::send(std::string packet, const char* name)
{
    if (packet.empty()) {
        if (cannotBuild_.insert(name).second) events_.push_back(std::string("cannot build ") + name);
        return false;
    }
    outgoing_.push_back(std::move(packet));
    return true;
}
void BotSession::clearActions()
{
    walkTarget_.reset(); actions_.clear(); usingItem_.reset();
    walkMode_ = WalkMode::Straight; path_.clear(); pathIndex_ = 0; pathComplete_ = false; pathWaiting_ = false; pathRequests_ = 0;
}
bool BotSession::canAct() const { return state_ == State::Spawned && movable_ && !classic_ && !dead_ && runtimeId_.has_value(); }
void BotSession::lookAt(protocol::Vec3 point)
{
    const double dx = double(point.x) - position_.x, dy = double(point.y) - position_.y, dz = double(point.z) - position_.z;
    constexpr double degrees = 180.0 / std::numbers::pi;
    yaw_ = static_cast<float>(-std::atan2(dx, dz) * degrees);
    pitch_ = static_cast<float>(-std::atan2(dy, std::hypot(dx, dz)) * degrees);
}
std::optional<std::string> BotSession::walkTo(protocol::Vec3 target, WalkMode mode)
{
    if (!canAct()) return "cannot act now";
    if (!finite(target) || (mode == WalkMode::Path && !blockPosition(target))) return "invalid coordinates";
    if (mode == WalkMode::Path && distance(target, feet()) > kMaxPathDistance)
        return std::format("the target is too far for a path (up to {} blocks)", kMaxPathDistance);
    following_ = false; pathAnnounced_ = false;
    if (mode == WalkMode::Path) { startPath(target); return std::nullopt; }
    walkTarget_ = target; walkStarted_ = tick_; walkChecked_ = tick_;
    walkDistance_ = horizontalDistance(target, position_); walkLimit_ = walkDistance_ / 0.2 * 2 + 100;
    walkMode_ = mode; path_.clear(); pathIndex_ = 0; pathComplete_ = false; pathWaiting_ = false; pathRequests_ = 0;
    return std::nullopt;
}
void BotSession::startPath(protocol::Vec3 target)
{
    walkTarget_ = target; walkStarted_ = tick_; walkChecked_ = tick_;
    walkDistance_ = horizontalDistance(target, position_); walkLimit_ = walkDistance_ / 0.2 * 2 + 100;
    walkMode_ = WalkMode::Path; path_.clear(); pathIndex_ = 0; pathComplete_ = false; pathWaiting_ = false; pathRequests_ = 0;
    pathTarget_ = {static_cast<std::int32_t>(std::floor(target.x)), static_cast<std::int32_t>(std::floor(target.y)),
                   static_cast<std::int32_t>(std::floor(target.z))};
    pathBest_ = targetDistance(feet());
    requestPath(true);
}
std::optional<std::string> BotSession::follow()
{
    if (!canAct()) return "cannot act now";
    walkTarget_.reset(); pathWaiting_ = false; path_.clear();
    following_ = true; followRetryAt_ = 0; followWaitingNoted_ = false;
    return std::nullopt;
}
void BotSession::setLeader(std::optional<protocol::Vec3> feet, std::optional<std::int32_t> dimension)
{
    leader_ = feet && blockPosition(*feet) ? feet : std::nullopt;
    leaderDimension_ = dimension;
}
void BotSession::updateFollow()
{
    const bool away = !leader_ || (dimension_ && leaderDimension_ && *dimension_ != *leaderDimension_);
    if (away) {
        if (walkTarget_) { walkTarget_.reset(); pathWaiting_ = false; path_.clear(); }
        if (!followWaitingNoted_) {
            followWaitingNoted_ = true;
            notes_.push_back(leader_ ? "follow: waiting for you to come back to this dimension" : "follow: waiting for your position");
        }
        return;
    }
    followWaitingNoted_ = false;
    const auto f = feet();
    const auto leader = *leader_;
    if (!walkTarget_) {
        if (distance(f, leader) > kFollowStart && tick_ >= followRetryAt_) startPath(leader);
        return;
    }
    if (horizontalDistance(f, leader) < kFollowStop && std::fabs(double(f.y) - leader.y) < 1.5) {
        walkTarget_.reset(); pathWaiting_ = false; path_.clear();
        return;
    }
    const PathNode now{static_cast<std::int32_t>(std::floor(leader.x)), static_cast<std::int32_t>(std::floor(leader.y)),
                       static_cast<std::int32_t>(std::floor(leader.z))};
    const double moved = std::hypot(double(now.x) - pathTarget_.x, double(now.y) - pathTarget_.y, double(now.z) - pathTarget_.z);
    if (!pathWaiting_ && moved > 1.5 && tick_ - pathRequestedAt_ >= 10) {
        walkTarget_ = leader; pathTarget_ = now; pathBest_ = targetDistance(f); pathRequests_ = 0;
        requestPath(false);
    }
}
void BotSession::finishWalk(const std::string& text, bool arrived)
{
    walkTarget_.reset(); pathWaiting_ = false;
    if (!following_) { events_.push_back(text); return; }
    notes_.push_back("follow: " + text);
    followRetryAt_ = arrived ? tick_ : tick_ + 40;
}
protocol::Vec3 BotSession::feet() const { return {position_.x, position_.y - kEyeHeight, position_.z}; }
double BotSession::targetDistance(protocol::Vec3 f) const
{
    return std::hypot(pathTarget_.x + 0.5 - f.x, pathTarget_.y - f.y, pathTarget_.z + 0.5 - f.z);
}
void BotSession::requestPath(bool counted)
{
    pathWaiting_ = true; ++pathSerial_; pathRequestedAt_ = tick_;
    if (counted) ++pathRequests_;
}
std::optional<BotSession::PathRequest> BotSession::pathRequest() const
{
    if (!walkTarget_ || walkMode_ != WalkMode::Path || !pathWaiting_ || !canAct()) return std::nullopt;
    const auto f = feet();
    double dx = pathTarget_.x + 0.5 - f.x, dy = pathTarget_.y - f.y, dz = pathTarget_.z + 0.5 - f.z;
    auto target = pathTarget_;
    const double across = std::hypot(dx, dz);
    requestShortened_ = across > kPathLeg;
    if (requestShortened_) {
        const double scale = kPathLeg / across;
        dx *= scale; dy *= scale; dz *= scale;
        target = {static_cast<std::int32_t>(std::floor(f.x + dx)), static_cast<std::int32_t>(std::floor(f.y + dy)),
                  static_cast<std::int32_t>(std::floor(f.z + dz))};
    }
    const auto radius = static_cast<float>(std::clamp(std::sqrt(dx * dx + dy * dy + dz * dz) * 1.5 + 16.0, 32.0, kMaxPathRadius));
    return PathRequest{pathSerial_, f, target, radius, inLiquid_};
}
void BotSession::pathFound(std::uint64_t serial, std::vector<PathNode> nodes, bool complete)
{
    if (!pathWaiting_ || serial != pathSerial_ || !walkTarget_) return;
    pathWaiting_ = false;
    if (requestShortened_) complete = false;
    if (nodes.empty() && pathAnnounced_ && pathRequests_ < kMaxPathRequests) {
        path_.clear(); pathIndex_ = 0; pathComplete_ = false; pathRetryAt_ = tick_ + 20;
        notes_.push_back("got no path nodes; asking again in a second");
        return;
    }
    if (nodes.empty() || (!complete && nodes.size() <= 1)) {
        finishWalk(std::format("no path to ({}, {}, {}) from ", pathTarget_.x, pathTarget_.y, pathTarget_.z) + coordinates(position_), false);
        return;
    }
    path_ = std::move(nodes); pathIndex_ = 0; pathComplete_ = complete;
    double length = 0;
    protocol::Vec3 from = feet();
    for (const auto& node : path_) {
        const protocol::Vec3 at{node.x + 0.5f, float(node.y), node.z + 0.5f};
        length += distance(from, at); from = at;
    }
    const auto f = feet();
    walkStarted_ = tick_; walkChecked_ = tick_; pathCheckedIndex_ = 0;
    navigationCheckedAt_ = tick_; navigationCheckedFeet_ = f;
    walkLimit_ = length / 0.2 * 2 + 100; walkDistance_ = std::hypot(path_[0].x + 0.5 - f.x, path_[0].z + 0.5 - f.z);
    auto text = std::format("found {} path ({} nodes)", complete ? "a" : "a partial", path_.size());
    if (pathAnnounced_ || following_) notes_.push_back(std::move(text));
    else { pathAnnounced_ = true; events_.push_back(std::move(text)); }
}
bool BotSession::requestSubChunks(std::int32_t dimension, std::int32_t radius, std::int32_t down, std::int32_t up)
{
    if (!canAct() || radius < 0 || down < 0 || up < 0 || radius > 8 || down > 8 || up > 8) return false;
    const auto f = feet();
    const auto cx = static_cast<std::int32_t>(std::floor(f.x / 16)), cy = static_cast<std::int32_t>(std::floor(f.y / 16)),
               cz = static_cast<std::int32_t>(std::floor(f.z / 16));
    std::vector<std::array<std::int8_t, 3>> offsets;
    for (std::int32_t dx = -radius; dx <= radius; ++dx)
        for (std::int32_t dz = -radius; dz <= radius; ++dz)
            for (std::int32_t dy = -down; dy <= up; ++dy)
                offsets.push_back({static_cast<std::int8_t>(dx), static_cast<std::int8_t>(dy), static_cast<std::int8_t>(dz)});
    return send(codec().subChunkRequest(sub_, dimension, cx, cy, cz, offsets), "subChunkRequest");
}
void BotSession::pathFailed(std::uint64_t serial, const std::string& reason)
{
    if (!pathWaiting_ || serial != pathSerial_ || !walkTarget_) return;
    finishWalk("cannot find a path (" + reason + ")", false);
}
const char* BotSession::followPath(protocol::AuthInput& input, bool aiming)
{
    if (pathWaiting_) {
        return tick_ - pathRequestedAt_ > 1400 ? "cannot find a path (the game did not compute it) at " : nullptr;
    }
    if (path_.empty() && tick_ < pathRetryAt_) return nullptr;
    if (path_.empty()) { requestPath(true); return nullptr; }
    const auto f = feet();
    const auto reached = [&](const PathNode& node) {
        return std::hypot(node.x + 0.5 - f.x, node.z + 0.5 - f.z) < 0.35 && f.y > node.y - (inLiquid_ ? 1.5 : 0.5);
    };
    while (pathIndex_ < path_.size() && reached(path_[pathIndex_])) ++pathIndex_;
    if (pathIndex_ >= path_.size()) {
        if (pathComplete_) return "arrived at ";
        const double left = targetDistance(f);
        const bool closer = left < pathBest_ - 1.0;
        if (closer) pathBest_ = left;
        else if (pathRequests_ >= kMaxPathRequests) return "gave up walking at ";
        requestPath(!closer);
        return nullptr;
    }
    if (double(tick_ - walkStarted_) > walkLimit_) return "gave up walking at ";
    const auto& node = path_[pathIndex_];
    const double dx = node.x + 0.5 - f.x, dz = node.z + 0.5 - f.z, d = std::hypot(dx, dz);
    if (tick_ - navigationCheckedAt_ > kNavigationCheckTicks) {
        const bool stuck = distance(f, navigationCheckedFeet_) < kNavigationCheckDistance;
        navigationCheckedAt_ = tick_; navigationCheckedFeet_ = f;
        if (stuck) {
            if (pathRequests_ >= kMaxPathRequests) return "blocked at ";
            requestPath(true);
            return nullptr;
        }
    }
    if (tick_ - walkChecked_ >= 40) {
        const bool stuck = pathIndex_ == pathCheckedIndex_ && walkDistance_ - d < 0.3;
        walkChecked_ = tick_; walkDistance_ = d; pathCheckedIndex_ = pathIndex_;
        if (stuck) {
            if (pathRequests_ >= kMaxPathRequests) return "blocked at ";
            requestPath(true);
            return nullptr;
        }
    }
    if (aiming) return nullptr;
    if (d > 1e-3) {
        yaw_ = static_cast<float>(-std::atan2(dx, dz) * 180.0 / std::numbers::pi); pitch_ = 0;
        const auto step = (std::min)(double(kWalkPerTick), d);
        input.delta.x = static_cast<float>(dx / d * step); input.delta.z = static_cast<float>(dz / d * step);
        position_.x += input.delta.x; position_.z += input.delta.z;
        input.moveZ = 1; input.extraFlags.push_back(10);
    }
    if (!inLiquid_ && node.y > std::floor(f.y + 0.05) && d < 1.3 && tick_ >= jumpReadyAt_) {
        input.extraFlags.insert(input.extraFlags.end(), std::begin(kJumpFlags), std::end(kJumpFlags));
        jumpReadyAt_ = tick_ + 10;
    }
    return nullptr;
}
void BotSession::stopActions()
{
    if (!canAct()) return;
    walkTarget_.reset(); pathWaiting_ = false; following_ = false;
    std::erase_if(actions_, [](const PendingAction& action) { return action.kind == Action::StartUse || action.kind == Action::Stop; });
    actions_.push_front(PendingAction{Action::Stop});
}
std::optional<std::string> BotSession::build(protocol::Vec3 block, std::uint8_t face)
{
    if (!canAct()) return "cannot act now";
    if (face > 5) return "invalid face";
    if (!blockPosition(block)) return "invalid block coordinates";
    const protocol::Vec3 p{std::floor(block.x), std::floor(block.y), std::floor(block.z)};
    protocol::Vec3 clicked{0.5f, 0.5f, 0.5f};
    const float side = face % 2 ? 1.0f : 0.0f;
    if (face <= 1) clicked.y = side;
    else if (face <= 3) clicked.z = side;
    else clicked.x = side;
    PendingAction action{Action::Build};
    action.point = {p.x + clicked.x, p.y + clicked.y, p.z + clicked.z};
    if (distance(position_, action.point) > 6) return "the block is out of reach";
    static constexpr const char* faces[] = {"down", "up", "north", "south", "west", "east"};
    action.label = coordinates(p) + " " + faces[face];
    action.use.action = 0; action.use.face = face; action.use.clicked = clicked;
    action.use.x = static_cast<std::int32_t>(p.x); action.use.y = static_cast<std::int32_t>(p.y); action.use.z = static_cast<std::int32_t>(p.z);
    action.use.blockRuntimeId = codec().blockRuntimeId(action.use.x, action.use.y, action.use.z);
    actions_.push_back(std::move(action));
    return std::nullopt;
}
std::optional<std::string> BotSession::useItem(bool eat)
{
    if (!canAct()) return "cannot act now";
    PendingAction action{Action::StartUse};
    action.eat = eat;
    action.use.face = 0xff;
    actions_.push_back(std::move(action));
    return std::nullopt;
}
void BotSession::releaseUse(bool finished)
{
    if (!usingItem_) return;
    const auto value = *usingItem_; usingItem_.reset();
    if (!send(codec().releaseItem(sub_, {0, value.slot, value.held, position_}), "releaseItem")) return;
    if (finished) events_.push_back(value.eat ? "finished eating" : "used");
}
void BotSession::sendActions()
{
    if (!canAct()) return;
    if (usingItem_ && tick_ >= usingItem_->releaseAt) releaseUse(true);
    if (actions_.empty()) return;
    if (resyncWanted_ && actions_.front().kind != Action::Stop) return;
    if (inventoryStale_ && actions_.front().kind != Action::Stop) {
        if (!resyncSentAt_) {
            send(codec().releaseItem(sub_, {0, selectedSlot_, inventory_[selectedSlot_], position_}), "releaseItem");
            resyncSentAt_ = tick_;
            return;
        }
        if (tick_ - *resyncSentAt_ < kInventoryResponseWaitTicks) return;
        inventoryStale_ = false; resyncSentAt_.reset();
    }
    auto action = std::move(actions_.front()); actions_.pop_front();
    if (action.kind == Action::Stop) { releaseUse(false); events_.push_back("stopped"); return; }
    if (usingItem_) releaseUse(false);
    const auto& held = inventory_[selectedSlot_];
    if (action.kind == Action::StartUse && held.empty()) { events_.push_back("the hand is empty"); return; }
    if (action.kind == Action::Build) send(codec().animateSwing(sub_, *runtimeId_), "animateSwing");
    action.use.hotbarSlot = selectedSlot_; action.use.held = held; action.use.position = position_;
    if (!send(codec().useItem(sub_, action.use), "useItem")) return;
    if (action.kind == Action::StartUse) usingItem_ = UsingItem{selectedSlot_, held, action.eat, tick_ + (action.eat ? 40 : 20)};
    else events_.push_back("clicked " + action.label);
}
bool BotSession::sendInput()
{
    protocol::AuthInput input;
    const bool aiming = !actions_.empty() && actions_.front().kind == Action::Build;
    if (following_ && canAct()) updateFollow();
    if (walkTarget_ && canAct() && walkMode_ == WalkMode::Path) {
        if (const char* result = followPath(input, aiming))
            finishWalk(std::string(result) + coordinates(position_), std::string_view(result).starts_with("arrived"));
    } else if (walkTarget_ && canAct()) {
        const auto d = horizontalDistance(*walkTarget_, position_);
        const char* result = nullptr;
        if (d < 0.35) result = "arrived at ";
        else if (double(tick_ - walkStarted_) > walkLimit_) result = "gave up walking at ";
        else if (tick_ - walkChecked_ >= 40) {
            if (walkDistance_ - d < 0.5) result = "blocked at ";
            walkChecked_ = tick_; walkDistance_ = d;
        }
        if (result) { walkTarget_.reset(); events_.push_back(std::string(result) + coordinates(position_)); }
        else if (!aiming) {
            const double dx = double(walkTarget_->x) - position_.x, dz = double(walkTarget_->z) - position_.z;
            yaw_ = static_cast<float>(-std::atan2(dx, dz) * 180.0 / std::numbers::pi); pitch_ = 0;
            const auto step = (std::min)(double(kWalkPerTick), d);
            input.delta.x = static_cast<float>(dx / d * step); input.delta.z = static_cast<float>(dz / d * step);
            position_.x += input.delta.x; position_.z += input.delta.z;
            input.moveZ = 1; input.extraFlags.push_back(10);
        }
    }
    if (canAct() && inLiquid_ && (walkTarget_ || following_)
        && std::uniform_real_distribution<double>(0.0, 1.0)(floatRandom_) < kFloatChance) {
        static constexpr std::int32_t kHold[] = {3, 6, 16, 61}, kPress[] = {60};
        input.extraFlags.insert(input.extraFlags.end(), std::begin(kHold), std::end(kHold));
        if (!jumpHeld_) input.extraFlags.insert(input.extraFlags.end(), std::begin(kPress), std::end(kPress));
        jumpHeld_ = true;
    } else jumpHeld_ = false;
    if (!actions_.empty() && canAct()) {
        const auto& action = actions_.front();
        if (action.kind == Action::Build) lookAt(action.point);
        if (action.kind == Action::StartUse) input.extraFlags.push_back(53);
    }
    input.pitch = pitch_; input.yaw = yaw_; input.position = position_;
    if (onGround_) input.delta.y = -0.0784f;
    input.tick = tick_;
    input.handledTeleport = teleported_;
    input.verticalCollision = onGround_;
    if (!send(codec().playerAuthInput(sub_, input), "playerAuthInput")) return false;
    teleported_ = false;
    return true;
}
void BotSession::prepare() { reset(); state_ = State::Preparing; }
void BotSession::attach(std::uint8_t sub)
{
    reset(); sub_ = sub; state_ = State::LoggingIn; loginReceived_ = true;
}
void BotSession::begin(std::uint8_t sub, const guest::Login& login)
{
    reset(); sub_ = sub; state_ = State::LoggingIn;
    send(codec().subClientLogin(sub_, login), "subClientLogin");
}
void BotSession::beginPrimary()
{
    reset(); sub_ = 0; state_ = State::LoggingIn;
}
void BotSession::fail(std::string reason)
{
    state_ = State::Disconnected;
    outgoing_.clear(); clearActions();
    events_.push_back("disconnected (" + std::move(reason) + ")");
}
void BotSession::spawnIfReady()
{
    if (!spawnReceived_ || !runtimeId_ || state_ == State::Spawned) return;
    send(codec().setLocalPlayerAsInitialized(sub_, *runtimeId_), "setLocalPlayerAsInitialized");
    if (!classic_) send(codec().loadingScreen(sub_, 2, std::nullopt), "loadingScreen");
    state_ = State::Spawned; spawnSerial_ = nextSpawnSerial(); events_.push_back("spawned");
    inventoryStale_ = true;
}
void BotSession::onPacket(std::uint16_t id, protocol::Bytes body)
{
    if (state_ == State::Idle || state_ == State::Preparing || state_ == State::Disconnected) return;
    switch (id) {
    case 2:
        if (const auto status = codec().readPlayStatus(body)) {
            if (*status == 0) {
                if (!loginReceived_) { loginReceived_ = true; events_.push_back("logged in"); }
            } else if (*status == 3) {
                spawnReceived_ = true; spawnIfReady();
            } else fail("PlayStatus " + std::to_string(*status));
        }
        break;
    case 0x0b:
        if (const auto start = codec().readStartGame(body); start && !runtimeId_) {
            runtimeId_ = start->runtimeId; uniqueId_ = start->uniqueId;
            if (const auto head = possess::readStartGameHead(body)) dimension_ = head->dimension;
            if (start->spawn) {
                movable_ = true;
                position_ = start->spawn->position; pitch_ = start->spawn->pitch; yaw_ = start->spawn->yaw;
                teleported_ = true;
            } else events_.push_back("cannot read the spawn position from StartGame; the bot will not fall or take damage like a player");
            send(codec().requestChunkRadius(sub_, 4, 4), "requestChunkRadius");
            state_ = State::Spawning; spawnIfReady();
        }
        break;
    case 0x3a:
        if (const auto hashes = codec().readLevelChunkBlobs(body)) {
            for (std::size_t at = 0; at < hashes->size(); at += protocol::kMaxBlobStatusHashes) {
                const auto count = (std::min)(protocol::kMaxBlobStatusHashes, hashes->size() - at);
                send(codec().clientCacheBlobHits(sub_, std::span(*hashes).subspan(at, count)), "clientCacheBlobHits");
            }
        } else if (!chunkUnreadable_) {
            chunkUnreadable_ = true;
            events_.push_back("cannot read LevelChunk (" + std::to_string(body.size()) + " bytes); the main player's chunks may stop loading");
        }
        break;
    case 0x13:
        if (const auto move = codec().readMovePlayer(body); move && runtimeId_ && move->runtimeId == *runtimeId_ && movable_) {
            pitch_ = move->pitch; yaw_ = move->yaw;
            if (move->mode != 3) {
                position_ = move->position; onGround_ = move->onGround;
                if (move->mode == 1 || move->mode == 2) { teleported_ = true; onGround_ = false; }
                if (move->mode == 2 && walkTarget_ && walkMode_ == WalkMode::Path && !pathWaiting_) requestPath(false);
            }
        }
        break;
    case 0xa1:
        if (const auto fix = codec().readCorrectPlayerMovePrediction(body); fix && fix->type == 0 && movable_) {
            position_ = fix->position; onGround_ = fix->onGround;
        }
        break;
    case 0x2d:
        if (const auto value = codec().readRespawn(body); value && runtimeId_ && movable_ && !classic_
            && (value->runtimeId == *runtimeId_ || value->runtimeId == 0)) {
            if (value->state == 0 && state_ == State::Spawned && !dead_) {
                dead_ = true; clearActions(); respawnAt_ = tick_ + 20; respawnPresses_ = 0; events_.push_back("died");
                break;
            }
            if (value->state != 1) break;
            clearActions();
            position_ = value->position; onGround_ = false; teleported_ = true;
            if (state_ != State::Spawned || !dead_) break;
            dead_ = false; respawnAt_.reset();
            send(codec().playerAction(sub_, *runtimeId_, 7), "playerAction");
            events_.push_back("respawned");
        }
        break;
    case 0x3d:
        if (const auto change = codec().readChangeDimension(body)) dimension_ = change->dimension;
        if (const auto change = codec().readChangeDimension(body); change && runtimeId_ && movable_) {
            position_ = change->position; onGround_ = false; teleported_ = true;
            if (classic_) break;
            if (change->screenId) {
                send(codec().loadingScreen(sub_, 1, change->screenId), "loadingScreen");
                send(codec().loadingScreen(sub_, 2, change->screenId), "loadingScreen");
            }
            send(codec().playerAction(sub_, *runtimeId_, 14), "playerAction");
        }
        break;
    case 0x46:
        (void)codec().readChunkRadiusUpdated(body);
        break;
    case 0x73:
        if (const auto needs = codec().readNetworkStackLatencyNeedsResponse(body); needs && *needs)
            send(codec().networkStackLatencyReply(sub_, body), "networkStackLatency");
        break;
    case 0x1f:
        if (const auto value = codec().readMobEquipment(body); value && value->runtimeId == runtimeId_.value_or(0)
            && value->window == 0 && value->hotbarSlot < 9) selectedSlot_ = value->hotbarSlot;
        break;
    case 0x30:
        if (const auto value = codec().readPlayerHotbar(body); value && value->window == 0 && value->slot < 9)
            selectedSlot_ = static_cast<std::uint8_t>(value->slot);
        break;
    case 0x11: {
        protocol::Reader reader(body);
        const auto item = reader.varUInt64(), taker = reader.varUInt64();
        if (!item || !taker) { inventoryStale_ = true; break; }
        if (*taker != runtimeId_.value_or(0)) break;
        inventoryStale_ = true; resyncWanted_ = true; lastPickupAt_ = tick_;
        break;
    }
    case 0x31:
        if (const auto value = codec().readInventoryContent(body)) {
            if (value->window == 0) {
                inventoryStale_ = false; resyncSentAt_.reset(); resyncWanted_ = false;
                inventory_ = {};
                std::copy_n(value->items.begin(), (std::min)(value->items.size(), inventory_.size()), inventory_.begin());
            } else if (value->window == 120) {
                armor_ = {};
                std::copy_n(value->items.begin(), (std::min)(value->items.size(), armor_.size()), armor_.begin());
            } else if (value->window == 119) offhand_ = value->items.empty() ? Item{} : value->items[0];
            else if (value->window == 124) cursor_ = value->items.empty() ? Item{} : value->items[0];
            else break;
            ++inventorySerial_;
        }
        break;
    case 0x32:
        if (const auto value = codec().readInventorySlot(body)) {
            if (value->window == 0 && value->slot < inventory_.size()) inventory_[value->slot] = value->item;
            else if (value->window == 120 && value->slot < armor_.size()) armor_[value->slot] = value->item;
            else if (value->window == 119 && value->slot == 0) offhand_ = value->item;
            else if (value->window == 124 && value->slot == 0) cursor_ = value->item;
            else break;
            ++inventorySerial_;
        }
        break;
    case 0x94: {
        lastStackResponseAt_ = tick_;
        if (stackResponses_.size() == 64) stackResponses_.erase(stackResponses_.begin());
        stackResponses_.emplace_back(reinterpret_cast<const char*>(body.data()), body.size());
        const auto responses = codec().readItemStackResponse(body);
        if (!responses) break;
        const auto before = inventory();
        const auto find = [&](std::int32_t netId) -> Item {
            for (const auto& item : before.slots) if (!item.empty() && item.netId == netId) return item;
            for (const auto& item : before.armor) if (!item.empty() && item.netId == netId) return item;
            for (const auto* item : {&before.offhand, &before.cursor}) if (!item->empty() && item->netId == netId) return *item;
            return {};
        };
        for (const auto& response : *responses) {
            if (response.status != 0) continue;
            for (const auto& container : response.containers) for (const auto& slot : container.slots) {
                Item* target = nullptr;
                const auto c = container.container;
                if ((c == 28 || c == 29 || c == 12) && slot.slot < inventory_.size()) target = &inventory_[slot.slot];
                else if (c == 6 && slot.slot < armor_.size()) target = &armor_[slot.slot];
                else if (c == 34 && slot.slot <= 1) target = &offhand_;
                else if (c == 59 && slot.slot == 0) target = &cursor_;
                if (!target) continue;
                Item item;
                if (slot.count && slot.netId) {
                    item = find(slot.netId);
                    if (item.empty()) {
                        item.id = 1; item.netId = slot.netId;
                        inventoryStale_ = true; resyncWanted_ = true;
                    }
                    item.count = slot.count;
                }
                *target = std::move(item); ++inventorySerial_;
            }
        }
        break;
    }
    case 5:
        if (const auto value = codec().readDisconnect(body))
            fail("reason " + std::to_string(value->reason) + (value->message.empty() ? "" : ": " + value->message));
        else fail("Disconnect");
        send(codec().disconnect(sub_), "disconnect");
        break;
    }
}
void BotSession::onTick(std::uint64_t nowMs)
{
    if (state_ == State::Spawned && movable_) {
        if (classic_) return;
        if (!nextTickAt_ || nowMs + kTickMs < nextTickAt_ || nowMs > nextTickAt_ + 1000) nextTickAt_ = nowMs;
        for (int sent = 0; sent < 4 && nowMs >= nextTickAt_; ++sent, nextTickAt_ += kTickMs) {
            ++tick_;
            if (respawnAt_ && tick_ >= *respawnAt_) {
                constexpr int kMaxRespawnPresses = 30;
                if (respawnPresses_++ < kMaxRespawnPresses) {
                    respawnAt_ = tick_ + 20;
                    send(codec().respawn(sub_, {}, protocol::kRespawnClientReady, *runtimeId_), "respawn");
                } else {
                    respawnAt_.reset(); events_.push_back("respawn gave up (no answer from the server)");
                }
            }
            if (canAct() && resyncWanted_ && tick_ - lastStackResponseAt_ >= 10 && tick_ - lastPickupAt_ >= 5 && !resyncSentAt_) {
                if (send(codec().releaseItem(sub_, {0, selectedSlot_, inventory_[selectedSlot_], position_}), "releaseItem")) {
                    resyncSentAt_ = tick_; resyncWanted_ = false;
                }
            }
            if (resyncSentAt_ && tick_ - *resyncSentAt_ >= kInventoryResponseWaitTicks) {
                inventoryStale_ = false; resyncSentAt_.reset();
            }
            if (sendInput()) sendActions();
        }
        return;
    }
    if (state_ != State::LoggingIn) return;
    if (!startedAt_) startedAt_ = nowMs;
    if (nowMs < *startedAt_ || nowMs - *startedAt_ < 30000) return;
    fail("no response");
    send(codec().disconnect(sub_), "disconnect");
}
BotSession::InventorySnapshot BotSession::inventory() const
{
    return {inventory_, armor_, offhand_, cursor_, selectedSlot_, inventorySerial_};
}
std::optional<std::string> BotSession::selectSlot(std::uint8_t slot)
{
    if (!canAct()) return "cannot act now";
    if (slot >= 9) return "hotbar slot must be 1-9";
    if (!send(codec().mobEquipment(sub_, *runtimeId_, inventory_[slot], slot), "mobEquipment")) return "cannot send hotbar selection";
    selectedSlot_ = slot; ++inventorySerial_;
    return std::nullopt;
}
bool BotSession::sendPacket(std::string packet, const char* name) { return send(std::move(packet), name); }
std::vector<std::string> BotSession::takeStackResponses() { return std::exchange(stackResponses_, {}); }
bool BotSession::openInventoryOnServer()
{
    return canAct() && send(codec().interact(sub_, 6, *runtimeId_), "interact");
}
bool BotSession::closeInventoryOnServer() { return send(codec().containerClose(sub_, 0, 0xf7, false), "containerClose"); }
std::vector<std::string> BotSession::takeOutgoing() { return std::exchange(outgoing_, {}); }
std::vector<std::string> BotSession::takeEvents() { return std::exchange(events_, {}); }
std::vector<std::string> BotSession::takeNotes() { return std::exchange(notes_, {}); }
}
