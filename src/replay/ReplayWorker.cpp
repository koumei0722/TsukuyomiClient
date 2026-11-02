#include "replay/ReplayWorker.h"
#include "replay/JeTables.h"
#include <map>
#include <tuple>
#include <stdexcept>
#include <memory>

namespace tsukuyomi::replay {
ReplayWorker::~ReplayWorker() { post(Stop{true}); wait(); }
bool ReplayWorker::launch(const std::filesystem::path& dir, const std::filesystem::path& zip)
{
    if (!finished()) return false;
    wait();
    std::lock_guard lock(mutex_);
    events_.clear(); hasResult_ = false; accepting_ = true; finished_.store(false, std::memory_order_release);
    try { thread_ = std::thread(&ReplayWorker::run, this, dir, zip); }
    catch (...) { accepting_ = false; finished_.store(true, std::memory_order_release); return false; }
    return true;
}
bool ReplayWorker::post(Event event)
{
    std::lock_guard lock(mutex_);
    if (!accepting_) return false;
    if (std::holds_alternative<Stop>(event)) accepting_ = false;
    events_.push_back(std::move(event)); wake_.notify_one(); return true;
}
void ReplayWorker::wait() { if (thread_.joinable()) thread_.join(); }
bool ReplayWorker::accepting() { std::lock_guard lock(mutex_); return accepting_; }
bool ReplayWorker::takeResult(Result& out)
{
    std::lock_guard lock(mutex_);
    if (!hasResult_) return false;
    out = result_; hasResult_ = false; return true;
}
void ReplayWorker::run(std::filesystem::path dir, std::filesystem::path zip)
{
    Result result; result.path = zip;
    std::unique_ptr<FlashbackFile> file;
    try {
        struct Cached { Column column; int index = -1; bool dirty = true; };
        std::map<std::pair<int, int>, Cached> columns;
        std::map<std::tuple<int, int, int>, BlockChange> pending;
        Start start{};
        std::unique_ptr<JavaWorld> world;
        PlayerPosition player, lastPlayer;
        std::uint64_t day = 6000, ticks = 0, segmentTicks = 0;
        bool initialized = false, open = false, force = false, started = false;
        auto check = [&](bool ok) { if (!ok) throw std::runtime_error(file->error().empty() ? "could not write recording" : file->error()); };
        auto action = [&](int id, const Bytes& bytes) { check(file->action(id, bytes)); };
        auto writeActions = [&](const std::vector<Action>& actions) { for (const auto& a : actions) action(a.id, a.bytes); };
        auto chunk = [&](Cached& c) {
            if (c.dirty || c.index < 0) {
                c.index = file->cacheChunk(java::levelChunkWithLight(c.column)); check(c.index >= 0); c.dirty = false;
            }
            ByteWriter w; w.varint(c.index); action(5, w.data);
        };
        auto snapshot = [&] {
            check(file->beginSegment()); open = true; segmentTicks = 0;
            const auto config = jetables::configPackets();
            for (std::size_t at = 0; at < config.size();) {
                std::uint32_t len = 0; unsigned shift = 0;
                for (;;) {
                    if (at == config.size() || shift >= 35) throw std::runtime_error("invalid configuration table");
                    const auto b = config[at++]; len |= (b & 127u) << shift;
                    if (!(b & 128)) break;
                    shift += 7;
                }
                if (len > config.size() - at) throw std::runtime_error("invalid configuration length");
                check(file->action(2, config.subspan(at, len))); at += len;
            }
            start.gamemode = world->localGameMode();
            java::Login login; login.dim = start.dim; login.gamemode = start.gamemode;
            action(1, java::login(login)); action(1, java::playerInfoAdd(start.uuid, start.localName, start.gamemode));
            action(1, java::setTime(ticks, day)); action(1, java::gameEventWaitChunks());
            action(3, java::createLocalPlayer(start.uuid, start.localName, start.gamemode, player)); lastPlayer = player;
            for (auto& [key, c] : columns) { (void)key; chunk(c); }
            std::vector<Action> actions; world->snapshot(actions); writeActions(actions);
            check(file->endSnapshot()); check(file->action(0));
            {
                std::vector<Action> after;
                after.push_back({4, java::moveEntities(dimensionInfo(start.dim).key, 1, player.x, player.y, player.z, player.yaw, player.pitch, player.yaw, player.onGround)});
                world->localInventory(after); writeActions(after);
            }
            ++ticks; ++segmentTicks;
        };
        auto flush = [&] {
            std::map<std::tuple<int, int, int>, std::vector<BlockChange>> groups;
            for (const auto& [key, b] : pending) { (void)key; groups[{b.x >> 4, b.y >> 4, b.z >> 4}].push_back(b); }
            for (const auto& [key, changes] : groups) {
                const auto [sx, sy, sz] = key;
                if (changes.size() == 1) { const auto& b = changes[0]; action(1, java::blockUpdate(b.x, b.y, b.z, b.state)); }
                else action(1, java::sectionBlocksUpdate(sx, sy, sz, changes));
            }
            pending.clear();
        };
        bool done = false;
        while (!done) {
            Event event;
            { std::unique_lock lock(mutex_); wake_.wait(lock, [&] { return !events_.empty(); }); event = std::move(events_.front()); events_.pop_front(); }
            if (auto* e = std::get_if<Start>(&event)) {
                start = *e; started = true; player = e->playerPos; day = e->time;
                file = std::make_unique<FlashbackFile>(dir, e->uuid, "Bedrock " + zip.stem().string());
                if (start.localName.empty()) start.localName = "Player";
                start.localName = java::profileName(start.localName);
                std::uint64_t seed = 0; for (auto b : start.uuid) seed = seed * 31 + b;
                world = std::make_unique<JavaWorld>(JavaWorld::Options{start.dim, start.uuid, start.localName, start.lang, seed});
                world->setLocalIds(start.localRuntime, start.localUnique);
                world->setBlockLookup([&](int x, int y, int z) -> int {
                    const auto it = columns.find({x >> 4, z >> 4}); if (it == columns.end()) return -1;
                    const int s = (y >> 4) - dimensionInfo(start.dim).minSection;
                    if (s < 0 || s >= static_cast<int>(it->second.column.sections.size())) return -1;
                    const auto& blocks = it->second.column.sections[s].blocks;
                    return blocks.empty() ? 0 : blocks.size() == 4096 ? blocks[((y & 15) << 8) | ((z & 15) << 4) | (x & 15)] : -1;
                });
                std::vector<Action> ignored; world->apply({false, bedrock::SetPlayerGameType{start.gamemode == 3 ? 6 : start.gamemode}}, ignored);
                for (const auto& packet : start.world) { ignored.clear(); world->apply(packet, ignored); }
            } else if (!started) {
                if (std::holds_alternative<Stop>(event)) done = true;
            } else if (auto* packets = std::get_if<BedrockPackets>(&event)) {
                std::vector<Action> actions;
                for (const auto& packet : packets->packets) { actions.clear(); world->apply(packet, actions); if (initialized) writeActions(actions); }
            } else if (auto* slots = std::get_if<LocalSlots>(&event)) {
                std::vector<Action> actions; world->applyLocalSlots(slots->slots, slots->selected, actions); if (initialized) writeActions(actions);
            } else if (auto* columnEvent = std::get_if<ColumnEvent>(&event)) {
                auto& c = columnEvent->column;
                if (c.dim == start.dim) {
                    const auto key = std::pair{c.cx, c.cz};
                    auto& cached = columns[key]; cached = {std::move(c), -1, true};
                    std::erase_if(pending, [&](const auto& p) { return (p.second.x >> 4) == key.first && (p.second.z >> 4) == key.second; });
                    if (initialized) chunk(cached);
                }
            } else if (auto* forget = std::get_if<Forget>(&event)) {
                columns.erase({forget->cx, forget->cz});
                std::erase_if(pending, [&](const auto& p) { return (p.second.x >> 4) == forget->cx && (p.second.z >> 4) == forget->cz; });
            } else if (auto* blocks = std::get_if<Blocks>(&event)) {
                for (const auto& b : blocks->changes) {
                    const auto it = columns.find({b.x >> 4, b.z >> 4});
                    if (it == columns.end()) continue;
                    const int s = (b.y >> 4) - dimensionInfo(start.dim).minSection;
                    auto& c = it->second;
                    if (s < 0 || s >= static_cast<int>(c.column.sections.size())) continue;
                    auto& values = c.column.sections[s].blocks;
                    if (values.empty()) values.resize(4096);
                    values[((b.y & 15) << 8) | ((b.z & 15) << 4) | (b.x & 15)] = b.state; c.dirty = true;
                    if (initialized) pending[{b.x, b.y, b.z}] = b;
                }
            } else if (auto* p = std::get_if<Player>(&event)) player = p->pos;
            else if (auto* time = std::get_if<Time>(&event)) day = time->dayTicks;
            else if (std::holds_alternative<InitialDone>(event)) {
                if (!initialized) { initialized = true; snapshot(); }
            } else if (auto* dimension = std::get_if<DimensionEvent>(&event)) {
                if (open) { flush(); check(file->endSegment(ticks, force)); open = false; }
                start.dim = dimension->dim; columns.clear(); pending.clear(); initialized = false; force = true;
                world->setDimension(start.dim);
            } else if (std::holds_alternative<Tick>(event) && initialized) {
                flush();
                std::vector<Action> actions; world->endTick(player, !(player == lastPlayer), actions); writeActions(actions); lastPlayer = player;
                if ((ticks + 1) % 20 == 0) action(1, java::setTime(ticks + 1, day));
                check(file->action(0)); ++ticks; ++segmentTicks;
                if (segmentTicks >= 6000) { check(file->endSegment(ticks, force)); force = false; snapshot(); }
            }
            const auto* stop = std::get_if<Stop>(&event);
            result.sizeLimit = file && file->bytesWritten() >= 7ull * 512 * 1024 * 1024;
            if (stop || result.sizeLimit) {
                { std::lock_guard lock(mutex_); accepting_ = false; events_.clear(); }
                if ((!stop || stop->save) && file) {
                    if (!open) { initialized = true; snapshot(); }
                    flush(); check(file->endSegment(ticks, force)); open = false;
                    check(file->finish(zip)); result.saved = true;
                    std::error_code ec; result.bytes = std::filesystem::file_size(zip, ec);
                    if (ec) result.bytes = file->bytesWritten();
                } else if (file) file->abandon();
                result.ticks = ticks; done = true;
            }
        }
    } catch (const std::exception& e) { result.error = e.what(); }
      catch (...) { result.error = "unexpected recording failure"; }
    { std::lock_guard lock(mutex_); accepting_ = false; events_.clear(); result_ = std::move(result); hasResult_ = true; }
    finished_.store(true, std::memory_order_release);
}
}
