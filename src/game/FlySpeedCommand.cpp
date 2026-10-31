#include "game/FlySpeedCommand.h"

#include "config/WriteSwitches.h"
#include "core/Logger.h"
#include "game/Abilities.h"
#include "game/BlockWrite.h"
#include "game/ChatCommand.h"
#include "game/FlySpeedLogic.h"
#include "game/GameData.h"
#include "game/PlayerListMemory.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"
#include "modules/DebugScreen.h"

#include <Windows.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

namespace tsukuyomi::flyspeed {
namespace {
constexpr std::uint32_t kAbilities = 0x68B75D48, kUniqueId = 0x18F957AF;
constexpr std::size_t kAbilitiesSize = 0x5B8;
constexpr std::size_t kPlayersSlot = 0x9F8, kFetchSlot = 0x1F0, kSenderSlot = 0xA40, kBroadcastSlot = 0x48;
constexpr const char* kUnavailable = "Fly speed is unavailable (see Tsukuyomi.log)";
std::atomic_bool g_ready{false}, g_failureLogged{false};
void* g_packetVtable = nullptr;
void* g_lambdaVtable = nullptr;
void* g_serialize = nullptr;
struct Pending {
    FlySpeedRequest request;
    std::int64_t ownId;
    std::string ownName;
    std::uint64_t world;
};
std::mutex g_queueMutex;
std::deque<Pending> g_queue;
std::atomic_size_t g_pending{0};
struct LocalWrite {
    enum class Mode { Host, Persistent, Reset } mode;
    float horizontal, vertical;
    std::uint64_t world;
};
std::mutex g_localMutex;
LocalWrite g_local{};
std::atomic_bool g_localActive{false};

LONG accessFilter(DWORD code)
{
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR
        ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}
void warnFailure()
{
    if (!g_failureLogged.exchange(true))
        log().warn(L"FlySpeed: game memory or an ability packet call was unavailable; request failed");
}
bool pointer(const void* base, std::size_t offset, const void*& out)
{
    return memory::plausiblePointer(base)
        && memory::copyGuarded(static_cast<const char*>(base) + offset, &out, sizeof(out))
        && memory::plausiblePointer(out);
}
const void* sessionLevel(const void* session)
{
    __try {
        const auto* bytes = static_cast<const char*>(session);
        const auto* control = *reinterpret_cast<const unsigned char* const*>(bytes + 0x30);
        if (!memory::plausiblePointer(control) || *control != 1) return nullptr;
        const void* level = *reinterpret_cast<const void* const*>(bytes + 0x40);
        return memory::plausiblePointer(level) ? level : nullptr;
    } __except (accessFilter(GetExceptionCode())) { return nullptr; }
}
struct LevelView { const void* level; const void* vtable; const void* fetch; const void* map; };
bool inspectLevel(const void* level, LevelView& out, bool server)
{
    out = {};
    const void* vtable = nullptr;
    const void* getter = nullptr;
    unsigned char code[8]{};
    std::int32_t disp = 0;
    if (!pointer(level, 0, vtable) || !pointer(vtable, kPlayersSlot, getter)
        || !memory::inGameModule(getter) || !memory::copyGuarded(getter, code, sizeof(code))
        || code[0] != 0x48 || code[1] != 0x8B || code[2] != 0x81 || code[7] != 0xC3) return false;
    std::memcpy(&disp, code + 3, sizeof(disp));
    if (disp <= 0 || disp >= 0x10000 || !pointer(level, static_cast<std::size_t>(disp), out.map)) return false;
    if (server && (!pointer(vtable, kFetchSlot, out.fetch)
        || !playerlist::mem::levelFetchLooksRight(out.fetch))) return false;
    out.level = level;
    out.vtable = vtable;
    return true;
}
const void* fetchActor(const LevelView& view, std::int64_t id)
{
    __try {
        const auto* vt = *static_cast<const void* const* const*>(view.level);
        if (vt != view.vtable || vt[kFetchSlot / 8] != view.fetch) return nullptr;
        using Fetch = const void* (__fastcall*)(const void*, std::int64_t, bool);
        return reinterpret_cast<Fetch>(const_cast<void*>(view.fetch))(view.level, id, false);
    } __except (accessFilter(GetExceptionCode())) { return nullptr; }
}
const void* packetSender(const LevelView& view)
{
    __try {
        const auto* vt = *static_cast<const void* const* const*>(view.level);
        if (vt != view.vtable || !memory::inGameModule(vt[kSenderSlot / 8])) return nullptr;
        using Get = const void* (__fastcall*)(const void*);
        return reinterpret_cast<Get>(const_cast<void*>(vt[kSenderSlot / 8]))(view.level);
    } __except (accessFilter(GetExceptionCode())) { return nullptr; }
}
bool readSpeeds(const void* actor, float& horizontal, float& vertical, bool verticalOnly = false)
{
    __try {
        const auto* layered = static_cast<const std::byte*>(GameData::componentAddress(actor, kAbilities, kAbilitiesSize));
        if (layered == nullptr) return false;
        const auto* h = layered + abilities::kArrayOffset + abilities::kPlayerLayer * abilities::kLayerStride
            + abilities::kFlySpeed * abilities::kStride;
        const auto* v = layered + abilities::kArrayOffset + abilities::kPlayerLayer * abilities::kLayerStride
            + abilities::kVerticalFlySpeed * abilities::kStride;
        if ((!verticalOnly && *h != std::byte{3}) || *v != std::byte{3}) return false;
        std::memcpy(&horizontal, h + abilities::kValueOffset, sizeof(float));
        std::memcpy(&vertical, v + abilities::kValueOffset, sizeof(float));
        return (verticalOnly || std::isfinite(horizontal)) && std::isfinite(vertical);
    } __except (accessFilter(GetExceptionCode())) { return false; }
}
int writeSpeeds(void* layered, float horizontal, float vertical)
{
    __try {
        if (layered == nullptr) return -1;
        auto* h = static_cast<std::byte*>(layered) + abilities::kArrayOffset
            + abilities::kPlayerLayer * abilities::kLayerStride + abilities::kFlySpeed * abilities::kStride;
        auto* v = static_cast<std::byte*>(layered) + abilities::kArrayOffset
            + abilities::kPlayerLayer * abilities::kLayerStride + abilities::kVerticalFlySpeed * abilities::kStride;
        float oldH = 0, oldV = 0;
        std::memcpy(&oldH, h + abilities::kValueOffset, sizeof(float));
        std::memcpy(&oldV, v + abilities::kValueOffset, sizeof(float));
        if (*h == std::byte{3} && *v == std::byte{3}
            && oldH == horizontal && oldV == vertical) return 1;
        *h = *v = std::byte{3};
        std::memcpy(h + abilities::kValueOffset, &horizontal, sizeof(float));
        std::memcpy(v + abilities::kValueOffset, &vertical, sizeof(float));
        return 0;
    } __except (accessFilter(GetExceptionCode())) { return -1; }
}
bool disposeFunction(void** fn)
{
    __try {
        void* impl = fn[7];
        if (impl != nullptr) {
            using Destroy = void (__fastcall*)(void*, bool);
            auto* vt = *static_cast<void***>(impl);
            reinterpret_cast<Destroy>(vt[4])(impl, impl != fn);
            fn[7] = nullptr;
        }
        return true;
    } __except (accessFilter(GetExceptionCode())) { return false; }
}
bool disposePacket(void* packet)
{
    __try {
        using Destroy = void (__fastcall*)(void*, unsigned);
        reinterpret_cast<Destroy>((*static_cast<void***>(packet))[0])(packet, 0);
        return true;
    } __except (accessFilter(GetExceptionCode())) { return false; }
}
bool sendAbilities(const void* sender, void* layered, std::int64_t id)
{
    alignas(16) std::byte packet[0x60]{};
    alignas(16) void* fn[8]{};
    bool sent = false, functionDisposed = true, packetDisposed = true;
    *reinterpret_cast<void**>(packet) = g_packetVtable;
    *reinterpret_cast<std::uint64_t*>(packet + 8) = 0x100000002ULL;
    *reinterpret_cast<std::int64_t*>(packet + 0x30) = id;
    *reinterpret_cast<std::uint32_t*>(packet + 0x58) = 1;
    fn[0] = g_lambdaVtable;
    fn[1] = packet + 0x30;
    fn[7] = fn;
    __try {
        __try {
            packet[0x38] = static_cast<const std::byte*>(layered)[0];
            packet[0x39] = static_cast<const std::byte*>(layered)[1];
            __try {
                using Serialize = void (__fastcall*)(void*, void*);
                reinterpret_cast<Serialize>(g_serialize)(layered, fn);
            } __finally { functionDisposed = disposeFunction(fn); }
            const auto* vt = *static_cast<const void* const* const*>(sender);
            if (functionDisposed && memory::inGameModule(vt[kBroadcastSlot / 8])) {
                using Broadcast = void (__fastcall*)(const void*, const void*);
                reinterpret_cast<Broadcast>(const_cast<void*>(vt[kBroadcastSlot / 8]))(sender, packet);
                sent = true;
            }
        } __finally { packetDisposed = disposePacket(packet); }
    } __except (accessFilter(GetExceptionCode())) { sent = false; }
    return sent && functionDisposed && packetDisposed;
}
bool readPlayers(const void* level, LevelView& view, std::vector<playerlist::mem::RawPlayer>& players, bool server)
{
    if (!inspectLevel(level, view, server)) return false;
    players.resize(playerlist::mem::kMaxPlayerEntries);
    const int count = playerlist::mem::readPlayerMap(view.map, players.data(), static_cast<int>(players.size()));
    if (count < 0) return false;
    players.resize(static_cast<std::size_t>(count));
    return true;
}
std::string ownName(std::int64_t id)
{
    const void* level = nullptr;
    LevelView view{};
    std::vector<playerlist::mem::RawPlayer> players;
    if (pointer(blockwrite::renderRegion(), 0x20, level) && readPlayers(level, view, players, false))
        for (const auto& player : players) if (player.actorId == id) return std::string(player.name, player.nameLength);
    return {};
}
void scheduleLocal(float horizontal, float vertical, std::uint64_t world, LocalWrite::Mode mode)
{
    std::lock_guard lock(g_localMutex);
    g_local = {mode, horizontal, vertical, world};
    g_localActive.store(true, std::memory_order_release);
}
chatcommand::Reply handle(const std::vector<std::string>& rawArgs)
{
    FlySpeedRequest request;
    std::string error;
    if (!parseFlySpeedArgs(rawArgs, request, error)) return {{error}, true};
    if (!g_ready.load(std::memory_order_acquire) || writes::blocked("FlySpeed")) return {{kUnavailable}, true};
    const bool integrated = GameData::instance().integratedServer();
    if (request.target.starts_with('@') && request.target != "@s" && request.target != "@a")
        return {{"Use a player name, @s or @a"}, true};
    std::int64_t id = 0;
    if (!GameData::copyComponent(GameData::instance().player(), kUniqueId, sizeof(id), &id))
        return {{"Could not read your player ID"}, true};
    const std::string name = ownName(id);
    const auto world = DebugScreen::worldEntry();
    if (integrated) {
        std::lock_guard lock(g_queueMutex);
        g_queue.push_back({std::move(request), id, name, world});
        g_pending.store(g_queue.size(), std::memory_order_release);
        return {};
    }
    if (request.target != "@s" && (name.empty() || matchPlayerName({name}, request.target) < 0))
        return {{kOwnPlayerOnly}, true};
    float horizontal = 0, vertical = 0;
    const bool show = request.kind == FlySpeedRequest::Kind::Show;
    const bool keepVertical = request.kind == FlySpeedRequest::Kind::Set && !request.hasVertical;
    if ((show || keepVertical) && !readSpeeds(GameData::instance().player(), horizontal, vertical, keepVertical)) {
        warnFailure();
        return {{"Could not read your fly speed"}, true};
    }
    if (!show) {
        horizontal = request.horizontal;
        if (request.hasVertical || request.kind == FlySpeedRequest::Kind::Reset) vertical = request.vertical;
        scheduleLocal(horizontal, vertical, world, request.kind == FlySpeedRequest::Kind::Reset
            ? LocalWrite::Mode::Reset : LocalWrite::Mode::Persistent);
    }
    return {{speedReply(name.empty() ? "@s" : name, horizontal, vertical, show, false)}, false};
}
void process(const Pending& pending, const LevelView& view, const std::vector<playerlist::mem::RawPlayer>& players)
{
    if (pending.world != DebugScreen::worldEntry()) {
        chatcommand::postNotice("Fly speed request dropped (world changed)");
        return;
    }
    std::vector<std::size_t> targets;
    if (pending.request.target == "@a" || pending.request.target == "@s") {
        for (std::size_t i = 0; i < players.size(); ++i)
            if (pending.request.target == "@a" || players[i].actorId == pending.ownId) targets.push_back(i);
    } else {
        std::vector<std::string> names;
        for (const auto& player : players) names.emplace_back(player.name, player.nameLength);
        const int match = matchPlayerName(names, pending.request.target);
        if (match >= 0) targets.push_back(static_cast<std::size_t>(match));
    }
    if (targets.empty()) { chatcommand::postNotice(missingPlayerReply(pending.request.target)); return; }
    const bool show = pending.request.kind == FlySpeedRequest::Kind::Show;
    const void* sender = show ? nullptr : packetSender(view);
    if (!show) {
        const void* getter = nullptr;
        unsigned char code[8]{};
        static bool logged = false;
        if (pointer(view.vtable, kSenderSlot, getter) && memory::inGameModule(getter)
            && memory::copyGuarded(getter, code, sizeof(code)) && !logged) {
            logged = true;
            log().info(L"FlySpeed: Level getPacketSender first bytes {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x}",
                       code[0], code[1], code[2], code[3], code[4], code[5], code[6], code[7]);
        }
    }
    for (const auto index : targets) {
        if (pending.world != DebugScreen::worldEntry()) {
            chatcommand::postNotice("Fly speed request dropped (world changed)");
            return;
        }
        const auto& player = players[index];
        const std::string name(player.name, player.nameLength);
        const void* actor = fetchActor(view, player.actorId);
        float horizontal = 0, vertical = 0;
        const bool keepVertical = pending.request.kind == FlySpeedRequest::Kind::Set && !pending.request.hasVertical;
        if (!GameData::instance().isServerPlayer(actor)
            || ((show || keepVertical) && !readSpeeds(actor, horizontal, vertical, keepVertical))) {
            warnFailure();
            chatcommand::postNotice("Could not read fly speed of " + name);
            continue;
        }
        if (!show) {
            horizontal = pending.request.horizontal;
            if (pending.request.hasVertical || pending.request.kind == FlySpeedRequest::Kind::Reset)
                vertical = pending.request.vertical;
            void* layered = GameData::componentAddress(actor, kAbilities, kAbilitiesSize);
            if (sender == nullptr || writeSpeeds(layered, horizontal, vertical) < 0) {
                warnFailure();
                chatcommand::postNotice("Could not set fly speed of " + name);
                continue;
            }
            if (player.actorId == pending.ownId)
                scheduleLocal(horizontal, vertical, pending.world, LocalWrite::Mode::Host);
            if (!sendAbilities(sender, layered, player.actorId)) {
                warnFailure();
                chatcommand::postNotice("Fly speed of " + name + " was saved, but the ability packet could not be sent");
                continue;
            }
        }
        chatcommand::postNotice(speedReply(name, horizontal, vertical, show, true));
    }
}
}

void registerCommand()
{
    chatcommand::registerRawCommand({"flyspeed"}, "/tk flyspeed <player> [<horizontal> [vertical] | reset]",
        [](const auto&, const auto& rawArgs) { return handle(rawArgs); });
}
void onScansReady()
{
    const auto* at = Scanner::instance().address(Target::UpdateAbilitiesPacketSite);
    if (at != nullptr) {
        g_lambdaVtable = memory::ripTarget(at + 0x36, 3);
        g_serialize = memory::ripTarget(at + 0x54, 1);
        g_packetVtable = memory::ripTarget(at + 0x9C, 3);
    }
    const bool ready = memory::inGameModule(g_lambdaVtable) && memory::inGameModule(g_serialize)
        && memory::inGameModule(g_packetVtable) && GameData::instance().knowsServerPlayer();
    g_ready.store(ready, std::memory_order_release);
    if (!ready) log().warn(L"FlySpeed: ability packet signatures are unavailable; /tk flyspeed is disabled");
}
void onServerTick(void* session)
{
    if (g_pending.load(std::memory_order_acquire) == 0) return;
    std::deque<Pending> work;
    {
        std::lock_guard lock(g_queueMutex);
        work.swap(g_queue);
        g_pending.store(0, std::memory_order_release);
    }
    LevelView view{};
    std::vector<playerlist::mem::RawPlayer> players;
    bool sampled = false, valid = false;
    for (const auto& pending : work) {
        if (pending.world != DebugScreen::worldEntry()) {
            chatcommand::postNotice("Fly speed request dropped (world changed)");
            continue;
        }
        if (!sampled) { sampled = true; valid = readPlayers(sessionLevel(session), view, players, true); }
        if (!valid) {
            warnFailure();
            chatcommand::postNotice("Could not read the world's players for fly speed");
            continue;
        }
        process(pending, view, players);
    }
}
void onClientTick()
{
    if (!g_localActive.load(std::memory_order_acquire)) return;
    std::lock_guard lock(g_localMutex);
    if (!g_localActive.load(std::memory_order_relaxed)) return;
    if (g_local.world != DebugScreen::worldEntry()) {
        g_localActive.store(false, std::memory_order_release);
        return;
    }
    void* layered = GameData::componentAddress(GameData::instance().player(), kAbilities, kAbilitiesSize);
    const int result = writeSpeeds(layered, g_local.horizontal, g_local.vertical);
    if (g_local.mode == LocalWrite::Mode::Persistent) return;
    if (result == 1 || (g_local.mode == LocalWrite::Mode::Reset && result == 0)) {
        g_localActive.store(false, std::memory_order_release);
    }
}
}
