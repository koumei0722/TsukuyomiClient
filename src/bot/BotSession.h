#pragma once
#include "BotCodec.h"
#include "GuestLogin.h"
#include <array>
#include <deque>
#include <random>
#include <set>
#include <vector>

namespace tsukuyomi::bot {
enum class State { Idle, Preparing, LoggingIn, Spawning, Spawned, Disconnected };
const char* stateName(State);
struct PathNode {
    std::int32_t x = 0, y = 0, z = 0;
    bool operator==(const PathNode&) const = default;
};
enum class WalkMode { Straight, Path };
class BotSession {
public:
    static constexpr std::uint64_t kTickMs = 50;
    void prepare();
    void begin(std::uint8_t sub, const guest::Login& login);
    void beginPrimary();
    void onPacket(std::uint16_t id, protocol::Bytes body);
    void onTick(std::uint64_t nowMs);
    std::vector<std::string> takeOutgoing();
    std::vector<std::string> takeEvents();
    std::vector<std::string> takeNotes();
    void reset();
    void fail(std::string reason);
    State state() const { return state_; }
    void attach(std::uint8_t sub);
    std::uint8_t sub() const { return sub_; }
    std::optional<std::uint64_t> runtimeId() const { return runtimeId_; }
    std::int64_t uniqueId() const { return uniqueId_; }
    protocol::Vec3 position() const { return position_; }
    protocol::Vec3 feet() const;
    std::optional<std::int32_t> dimension() const { return dimension_; }
    float pitch() const { return pitch_; }
    float yaw() const { return yaw_; }
    std::uint64_t tick() const { return tick_; }
    std::uint64_t spawnSerial() const { return spawnSerial_; }
    void setClassic(bool value) { classic_ = value; }
    bool classic() const { return classic_; }
    bool canAct() const;
    std::optional<std::string> walkTo(protocol::Vec3 target, WalkMode mode = WalkMode::Straight);
    std::optional<std::string> follow();
    bool following() const { return following_; }
    void setLeader(std::optional<protocol::Vec3> feet, std::optional<std::int32_t> dimension);
    static constexpr double kFollowStart = 3.0, kFollowStop = 2.0;
    struct PathRequest {
        std::uint64_t serial = 0;
        protocol::Vec3 feet;
        PathNode target;
        float radius = 0;
        bool inLiquid = false;
    };
    std::optional<PathRequest> pathRequest() const;
    void pathFound(std::uint64_t serial, std::vector<PathNode> nodes, bool complete);
    void pathFailed(std::uint64_t serial, const std::string& reason);
    bool wantsSurroundings() const { return canAct() && (walkTarget_.has_value() || following_); }
    void setInLiquid(std::optional<bool> value) { inLiquid_ = value.value_or(false); }
    static constexpr double kFloatChance = 0.8;
    bool inLiquid() const { return inLiquid_; }
    bool requestSubChunks(std::int32_t dimension, std::int32_t radius, std::int32_t down, std::int32_t up);
    void stopActions();
    std::optional<std::string> build(protocol::Vec3 block, std::uint8_t face);
    std::optional<std::string> useItem(bool eat);
    static constexpr std::uint64_t kInventoryResponseWaitTicks = 20;
    struct InventorySnapshot {
        std::array<Item, 36> slots;
        std::array<Item, 4> armor;
        Item offhand, cursor;
        std::uint8_t selected;
        std::uint64_t serial;
    };
    InventorySnapshot inventory() const;
    std::optional<std::string> selectSlot(std::uint8_t slot);
    bool sendPacket(std::string packet, const char* name);
    std::vector<std::string> takeStackResponses();
    bool openInventoryOnServer();
    bool closeInventoryOnServer();
private:
    void spawnIfReady();
    bool sendInput();
    bool send(std::string packet, const char* name);
    void clearActions();
    void sendActions();
    void lookAt(protocol::Vec3 point);
    void releaseUse(bool finished);
    std::array<Item, 36> inventory_{};
    std::array<Item, 4> armor_{};
    Item offhand_, cursor_;
    std::uint64_t inventorySerial_ = 0, lastStackResponseAt_ = 0, lastPickupAt_ = 0;
    bool resyncWanted_ = false;
    std::vector<std::string> stackResponses_;
    std::uint8_t selectedSlot_ = 0;
    bool inventoryStale_ = false;
    std::optional<std::uint64_t> resyncSentAt_;
    std::optional<protocol::Vec3> walkTarget_;
    std::uint64_t walkStarted_ = 0, walkChecked_ = 0;
    double walkLimit_ = 0, walkDistance_ = 0;
    static constexpr int kMaxPathRequests = 8;
    WalkMode walkMode_ = WalkMode::Straight;
    PathNode pathTarget_;
    std::vector<PathNode> path_;
    std::size_t pathIndex_ = 0;
    bool pathComplete_ = false, pathWaiting_ = false;
    std::uint64_t pathSerial_ = 0, pathRequestedAt_ = 0, jumpReadyAt_ = 0;
    std::size_t pathCheckedIndex_ = 0;
    int pathRequests_ = 0;
    double pathBest_ = 0;
    bool pathAnnounced_ = false;
    static constexpr double kPathLeg = 48;
    mutable bool requestShortened_ = false;
    std::uint64_t pathRetryAt_ = 0;
    static constexpr std::uint64_t kNavigationCheckTicks = 100;
    static constexpr double kNavigationCheckDistance = 1.5;
    std::uint64_t navigationCheckedAt_ = 0;
    protocol::Vec3 navigationCheckedFeet_;
    bool inLiquid_ = false, jumpHeld_ = false;
    std::minstd_rand floatRandom_{20261013};
    bool following_ = false;
    std::optional<protocol::Vec3> leader_;
    std::optional<std::int32_t> leaderDimension_;
    std::uint64_t followRetryAt_ = 0;
    bool followWaitingNoted_ = false;
    std::vector<std::string> notes_;
    double targetDistance(protocol::Vec3 feet) const;
    void requestPath(bool counted);
    void startPath(protocol::Vec3 target);
    void updateFollow();
    void finishWalk(const std::string& text, bool arrived);
    const char* followPath(protocol::AuthInput& input, bool aiming);
    enum class Action { Build, StartUse, Stop };
    struct PendingAction {
        Action kind;
        protocol::Vec3 point;
        UseItem use;
        std::string label;
        bool eat = false;
    };
    std::deque<PendingAction> actions_;
    struct UsingItem { std::uint8_t slot; Item held; bool eat; std::uint64_t releaseAt; };
    std::optional<UsingItem> usingItem_;
    std::set<std::string> cannotBuild_;
    State state_ = State::Idle;
    std::uint8_t sub_ = 1;
    std::optional<std::uint64_t> runtimeId_, startedAt_;
    std::int64_t uniqueId_ = 0;
    bool spawnReceived_ = false, loginReceived_ = false, chunkUnreadable_ = false;
    std::vector<std::string> outgoing_, events_;
    bool movable_ = false, onGround_ = false, teleported_ = false, dead_ = false;
    protocol::Vec3 position_;
    std::optional<std::int32_t> dimension_;
    float pitch_ = 0, yaw_ = 0;
    std::uint64_t tick_ = 0, nextTickAt_ = 0;
    std::optional<std::uint64_t> respawnAt_;
    int respawnPresses_ = 0;
    std::uint64_t spawnSerial_ = 0;
    bool classic_ = false;
    static std::uint64_t nextSpawnSerial() { static std::uint64_t serial = 0; return ++serial; }
};
}
