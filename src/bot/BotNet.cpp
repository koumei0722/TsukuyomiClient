#include "BotNet.h"
#include "GameString.h"
#include "BotCodec.h"
#include "BotProtocol.h"
#include "BotSignatures.h"
#include "SkinSteal.h"
#include "core/Logger.h"
#include "core/Notice.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"
#include "replay/BedrockPackets.h"
#include "replay/PacketTap.h"
#include <Windows.h>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <deque>
#include <limits>
#include <format>
#include <mutex>
#include <map>
#include <utility>

namespace tsukuyomi::bot::net {
namespace {
struct Offsets {
    std::uint32_t disconnect = 0, peer = 0, begin = 0, end = 0;
    std::uint8_t sendSlot = 0, receiveSlot = 0, kind = 0, cns = 0, sub = 0;
} g_offsets;
std::array<std::byte*, static_cast<std::size_t>(Site::Count)> g_sites{};
std::atomic<bool> g_offsetsReady{false}, g_hooksReady{false};
std::atomic<void*> g_cns{nullptr};
std::mutex g_mutex;
std::deque<Received> g_queue;
std::array<std::uint64_t, kSubCount> g_perSub{};
std::uint64_t g_dropped = 0;
bool g_dropLogged = false;
std::uint8_t g_view = 0, g_headlessMask = 0;
constexpr std::uint8_t subBit(std::uint8_t sub) { return static_cast<std::uint8_t>(1u << sub); }
bool headless(std::uint8_t sub) { return sub < kSubCount && (g_headlessMask & subBit(sub)) != 0; }
possess::Ids g_swap;
std::int64_t g_tickOffset = 0;
std::optional<possess::AuthInputHead> g_lastInput;
std::optional<possess::StartGameHead> g_mainStart;
std::int32_t g_gameRadius = 0;
std::int32_t g_serverRadius = 0;
possess::StateKeeper g_keepers[kSubCount];
skin::SkinCatalog g_skins;
unsigned g_skinReadFailures = 0;
struct Timed { std::uint64_t due; std::string bytes; };
std::deque<Timed> g_timed;
constexpr std::ptrdiff_t kSystemFromCns = 0x20;
std::byte* g_sendInternal = nullptr;
std::atomic<SendFilter> g_sendFilter{nullptr};
int g_rewriteFailures = 0, g_assignFailures = 0;
std::optional<relog::ServerAddress> g_serverAddress;
bool g_loginUnreadableLogged = false;
bool g_relogArmed = false, g_relogRouted = false;
std::optional<std::pair<possess::Uuid, possess::Uuid>> g_relogUuids;
std::uint8_t g_relogView = 1;
std::string g_gameKey, g_userRequest, g_sentRequest;
std::uint64_t g_loginCount = 0;
int g_disconnectLogs = 0;
struct MainSwapArm { bool armed = false; std::string token, key; possess::Ids ids; std::string nonce; } g_mainSwapArm;
possess::Ids g_mainSwap;
bool g_mainSwapped = false;
std::string g_mainSwapError;
int g_terrainSub = -1;
bool g_terrainMisses = false;
std::deque<std::string> g_terrain;
constexpr std::size_t kTerrainQueue = 4096;
constexpr std::size_t kGameBlobs = 256;
std::deque<std::uint64_t> g_gameBlobs;
void rememberGameHits(protocol::Bytes body)
{
    protocol::Reader r(body);
    const auto misses = r.varUInt32();
    if (!misses || *misses > protocol::kMaxBlobStatusHashes) return;
    for (std::uint32_t i = 0; i < *misses; ++i) if (!r.fixed64()) return;
    const auto hits = r.varUInt32();
    if (!hits || *hits > protocol::kMaxBlobStatusHashes) return;
    for (std::uint32_t i = 0; i < *hits; ++i) {
        const auto hash = r.fixed64();
        if (!hash) return;
        if (std::find(g_gameBlobs.begin(), g_gameBlobs.end(), *hash) != g_gameBlobs.end()) continue;
        g_gameBlobs.push_front(*hash);
        if (g_gameBlobs.size() > kGameBlobs) g_gameBlobs.pop_back();
    }
}
bool primaryOnly(std::uint16_t id) { return id == 0xa2 || id == 0x88 || id == 0x140 || id == 0x152; }
bool g_viewStarted = false;
std::deque<std::string> g_heldForView;
void* g_lastConnection = nullptr;
std::atomic<void (*)()> g_pump{nullptr};
std::atomic<bool> g_pumpWanted{false};
std::uint64_t g_lastPump = 0;
std::uint64_t nowMs() { return GetTickCount64(); }
void resetRouting()
{
    g_headlessMask = 0; g_queue.clear(); g_perSub = {}; g_dropped = 0;
    g_view = 0; g_swap = g_mainSwap; g_tickOffset = 0; g_timed.clear();
    g_relogRouted = false; g_viewStarted = false; g_heldForView.clear();
    for (std::uint8_t sub = 1; sub < kSubCount; ++sub) g_keepers[sub].reset();
}
void dropQueued(std::uint8_t sub)
{
    std::erase_if(g_queue, [sub](const Received& packet) { return packet.recipient == sub; });
}
int memoryException(unsigned code)
{
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR
        ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}
void* senderCns(void* sender)
{
    __try {
        if (!memory::plausiblePointer(sender)) return nullptr;
        const auto* p = static_cast<const std::uint8_t*>(sender);
        if (p[g_offsets.kind] != 0 || p[g_offsets.sub] != 0) return nullptr;
        void* const cns = *reinterpret_cast<void* const*>(p + g_offsets.cns);
        return memory::plausiblePointer(cns) ? cns : nullptr;
    } __except (memoryException(GetExceptionCode())) { return nullptr; }
}
void* firstConnection(void* cns)
{
    __try {
        if (!memory::plausiblePointer(cns)) return nullptr;
        const auto* p = static_cast<const std::uint8_t*>(cns);
        const auto begin = *reinterpret_cast<const std::uintptr_t*>(p + g_offsets.begin);
        const auto end = *reinterpret_cast<const std::uintptr_t*>(p + g_offsets.end);
        if (!memory::plausiblePointer(begin) || end <= begin || (end - begin) % sizeof(void*) != 0) return nullptr;
        void* const conn = *reinterpret_cast<void* const*>(begin);
        return memory::plausiblePointer(conn) ? conn : nullptr;
    } __except (memoryException(GetExceptionCode())) { return nullptr; }
}
bool sendGuarded(void* cns, const std::string* bytes)
{
    __try {
        auto* const conn = static_cast<const std::uint8_t*>(firstConnection(cns));
        if (!conn || conn[g_offsets.disconnect] != 0) return false;
        void* const peer = *reinterpret_cast<void* const*>(conn + g_offsets.peer);
        if (!memory::plausiblePointer(peer)) return false;
        const auto* table = *reinterpret_cast<const std::uint8_t* const*>(peer);
        if (!memory::plausiblePointer(table)) return false;
        using SendFn = void(__fastcall*)(void*, const std::string*, int, int);
        const auto fn = *reinterpret_cast<const SendFn*>(table + g_offsets.sendSlot);
        if (!memory::inGameModule(reinterpret_cast<const void*>(fn))
            || !memory::isExecutable(reinterpret_cast<const void*>(fn), 1)) return false;
        fn(peer, bytes, 1, 0);
        return true;
    } __except (memoryException(GetExceptionCode())) { return false; }
}
bool peekGuarded(const void* out, char* destination, std::size_t length, std::size_t* size)
{
    __try {
        const auto* p = static_cast<const std::uint8_t*>(out);
        const auto have = *reinterpret_cast<const std::size_t*>(p + 0x10);
        const auto capacity = *reinterpret_cast<const std::size_t*>(p + 0x18);
        if (have == 0 || have > capacity) return false;
        if (length > have) length = have;
        const auto* source = capacity >= 16 ? *reinterpret_cast<const char* const*>(p) : reinterpret_cast<const char*>(p);
        std::memcpy(destination, source, length);
        *size = have;
        return true;
    } __except (memoryException(GetExceptionCode())) { return false; }
}
bool copyGuarded(const void* out, char* destination, std::size_t length)
{
    __try {
        const auto* p = static_cast<const std::uint8_t*>(out);
        const auto size = *reinterpret_cast<const std::size_t*>(p + 0x10);
        const auto capacity = *reinterpret_cast<const std::size_t*>(p + 0x18);
        if (size != length || size > capacity) return false;
        const auto* source = capacity >= 16 ? *reinterpret_cast<const char* const*>(p) : reinterpret_cast<const char*>(p);
        std::memcpy(destination, source, length);
        return true;
    } __except (memoryException(GetExceptionCode())) { return false; }
}
bool assignGameString(void* out, const std::string& bytes)
{
    return gamestring::assign(out, bytes);
}
void scanSites()
{
    const auto* const base = reinterpret_cast<const std::byte*>(GetModuleHandleW(nullptr));
    for (const SiteInfo& info : kSites) {
        const ScanHit hit = scanMainModule(info.signature);
        const auto index = static_cast<std::size_t>(info.site);
        g_sites[index] = hit.count == 1 ? hit.address : nullptr;
        if (hit.count == 1)
            log().success(L"{} found (RVA {:#x})", info.name, static_cast<std::uintptr_t>(hit.address - base));
        else if (hit.count == 0)
            log().warn(L"{} not found - {} is unavailable", info.name, info.purpose);
        else
            log().warn(L"{} matched more than once - ignoring to avoid a false positive", info.name);
    }
}
std::byte* siteAddress(Site site) { return g_sites[static_cast<std::size_t>(site)]; }
template<class T> bool immediate(Site target, std::size_t offset, T& value)
{
    const auto* site = siteAddress(target);
    if (!site || !memory::isReadable(site + offset, sizeof(T))) return false;
    return memory::copyGuarded(site + offset, &value, sizeof(T));
}
}
bool initialize()
{
    scanSites();
    const bool ok = siteAddress(Site::NetworkReceivePacket) != nullptr
        && immediate(Site::NetworkReceivePacket, 0x16, g_offsets.receiveSlot)
        && immediate(Site::PeerSendSite, 3, g_offsets.disconnect)
        && immediate(Site::PeerSendSite, 13, g_offsets.peer)
        && immediate(Site::PeerSendSite, 0x24, g_offsets.sendSlot)
        && immediate(Site::SenderToServerSite, 2, g_offsets.kind)
        && immediate(Site::SenderToServerSite, 16, g_offsets.cns)
        && immediate(Site::SenderToServerSite, 20, g_offsets.begin)
        && immediate(Site::SenderToServerSite, 27, g_offsets.end)
        && immediate(Site::SenderSubIdSite, 3, g_offsets.sub);
    if (!ok || !g_offsets.disconnect || !g_offsets.peer || !g_offsets.cns
        || g_offsets.end != g_offsets.begin + sizeof(void*) || g_offsets.sendSlot != sizeof(void*)
        || g_offsets.receiveSlot != 8 * sizeof(void*)) {
        notice::failOnce("BotOffsets", L"Bot: cannot read the network signature immediates", "Bot is unavailable");
        return false;
    }
    if (auto* site = siteAddress(Site::PeerSendSite)) {
        DWORD64 imageBase = 0;
        if (const auto* entry = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(site), &imageBase, nullptr))
            g_sendInternal = reinterpret_cast<std::byte*>(imageBase + entry->BeginAddress);
    }
    log().info(L"Bot: conn disconnect={:#x}, peer={:#x}, sendSlot={:#x}; sender kind={:#x}, cns={:#x}, sub={:#x}; connections={:#x}..{:#x}; sendInternal RVA {:#x}",
        g_offsets.disconnect, g_offsets.peer, g_offsets.sendSlot, g_offsets.kind, g_offsets.cns,
        g_offsets.sub, g_offsets.begin, g_offsets.end,
        g_sendInternal ? static_cast<std::uintptr_t>(g_sendInternal - reinterpret_cast<std::byte*>(GetModuleHandleW(nullptr))) : 0);
    g_offsetsReady.store(true, std::memory_order_release);
    return true;
}
void* receiveTarget() { return siteAddress(Site::NetworkReceivePacket); }
std::byte* site(Site which) { return siteAddress(which); }
namespace {
void* peerGuarded(void* cns)
{
    __try {
        auto* const conn = static_cast<const std::uint8_t*>(firstConnection(cns));
        if (!conn || conn[g_offsets.disconnect] != 0) return nullptr;
        void* const peer = *reinterpret_cast<void* const*>(conn + g_offsets.peer);
        return memory::plausiblePointer(peer) ? peer : nullptr;
    } __except (memoryException(GetExceptionCode())) { return nullptr; }
}
}
void* mainPeer() { return available() ? peerGuarded(g_cns.load(std::memory_order_acquire)) : nullptr; }
void setSendFilter(SendFilter filter) { g_sendFilter.store(filter, std::memory_order_release); }
void* sendInternalTarget() { return g_sendInternal; }
void hooksReady(bool receiveReady, bool sendReady)
{
    g_hooksReady.store(receiveReady && sendReady, std::memory_order_release);
    if (!receiveReady || !sendReady)
        notice::failOnce("BotHooks", L"Bot: NetworkReceivePacket or PacketSend hook is unavailable", "Bot is unavailable");
}
bool available() { return g_offsetsReady.load(std::memory_order_acquire) && g_hooksReady.load(std::memory_order_acquire); }
void observeSender(void* sender)
{
    if (!g_offsetsReady.load(std::memory_order_acquire)) return;
    if (void* cns = senderCns(sender)) g_cns.store(cns, std::memory_order_release);
}
void activate(std::uint8_t sub)
{
    if (sub >= kSubCount) return;
    std::lock_guard lock(g_mutex);
    dropQueued(sub); g_perSub[sub] = 0; g_dropLogged = false;
    g_headlessMask |= subBit(sub);
    g_keepers[sub].reset();
}
void deactivate(std::uint8_t sub)
{
    if (sub >= kSubCount) return;
    std::lock_guard lock(g_mutex);
    g_headlessMask = static_cast<std::uint8_t>(g_headlessMask & ~subBit(sub));
    dropQueued(sub); g_perSub[sub] = 0;
    if (sub != 0) g_keepers[sub].reset();
}
void deactivateAll()
{
    std::lock_guard lock(g_mutex);
    resetRouting();
}
int receive(ReceiveFn original, void* peer, void* out, void* context)
{
    void* const cns = g_cns.load(std::memory_order_acquire);
    void* const connection = firstConnection(cns);
    const bool client = peer && peer == peerGuarded(cns);
    if (client) {
        if (g_pumpWanted.load(std::memory_order_acquire))
            if (auto* const pump = g_pump.load(std::memory_order_acquire); pump && nowMs() - g_lastPump >= 20) { g_lastPump = nowMs(); pump(); }
        std::lock_guard lock(g_mutex);
        if (connection != g_lastConnection) {
            if (g_lastConnection) { resetRouting(); log().info(L"Bot: a new client connection; routing reset"); }
            g_lastConnection = connection;
        }
        while (!g_timed.empty() && g_timed.front().due <= nowMs()) {
            auto timed = std::move(g_timed.front());
            g_timed.pop_front();
            if (assignGameString(out, timed.bytes)) return 0;
            if (g_assignFailures++ < 8) log().warn(L"Bot: cannot hand a {}-byte packet to the game", timed.bytes.size());
        }

    }
    for (;;) {
        const int result = original(peer, out, context);
        if (result != 0 || !client) return result;
        char headBytes[96]{};
        std::size_t length = 0;
        if (!peekGuarded(out, headBytes, sizeof headBytes, &length)) return result;
        const std::size_t peeked = length < sizeof headBytes ? length : sizeof headBytes;
        const auto head = protocol::readHeader(protocol::bytes({headBytes, peeked}));
        if (!head) return result;
        if (head->id == 0x05 && head->recipient == 0 && g_disconnectLogs < 16) {
            if (const auto reason = protocol::Reader(protocol::bytes({headBytes + head->bodyOffset, peeked - head->bodyOffset})).varInt32()) {
                ++g_disconnectLogs;
                log().info(L"Bot: the server disconnected the game (reason {})", *reason);
            }
        }
        std::lock_guard lock(g_mutex);
        if ((head->id == 0xae && g_terrainSub > 0 && head->recipient == g_terrainSub) || (head->id == 0x88 && g_terrainMisses && head->recipient == 0)) {
            std::string whole(length, '\0');
            if (copyGuarded(out, whole.data(), length)) {
                if (g_terrain.size() >= kTerrainQueue) g_terrain.pop_front();
                g_terrain.push_back(std::move(whole));
                if (head->id == 0xae) continue;
            }
        }
        if (head->id == 0x0b && head->recipient < kSubCount && peeked > head->bodyOffset) {
            if (const auto start = possess::readStartGameHead(protocol::bytes({headBytes + head->bodyOffset, peeked - head->bodyOffset}))) {
                auto& keeper = g_keepers[head->recipient];
                keeper.reset(); keeper.setSelf(start->runtimeId);
                if (head->recipient == 0) g_mainStart = start;
                log().info(L"Bot: StartGame for sub {} unique={} runtime={} mode={}", head->recipient, start->uniqueId, start->runtimeId, start->gameMode);
            }
            if (head->recipient == 0 && g_relogArmed) {
                g_relogArmed = false; g_relogRouted = true; g_viewStarted = false; g_heldForView.clear();
                g_queue.clear(); g_perSub = {}; g_dropped = 0;
                g_view = g_relogView; g_headlessMask = subBit(0); g_swap = {}; g_tickOffset = 0;
                if (g_relogUuids) {
                    g_swap.active = true;
                    g_swap.runtimeA = g_swap.runtimeB = ~std::uint64_t{0};
                    g_swap.uniqueA = g_swap.uniqueB = (std::numeric_limits<std::int64_t>::min)();
                    g_swap.uuidA = g_relogUuids->first; g_swap.uuidB = g_relogUuids->second; g_swap.uuids = true;
                }
                log().info(L"Bot: rejoined; the main account's StartGame goes to the headless session, the bot's packets to the game");
            }
        }
        if (head->recipient == g_view && (head->id == 0x46 || head->id == 0x79) && peeked > head->bodyOffset) {
            protocol::Reader r(protocol::bytes({headBytes + head->bodyOffset, peeked - head->bodyOffset}));
            if (head->id == 0x46) { if (const auto radius = r.varInt32(); radius && *radius > 0) g_serverRadius = *radius; }
            else if (r.varInt32() && r.varInt32() && r.varInt32()) if (const auto blocks = r.varUInt32(); blocks && *blocks >= 16) g_serverRadius = static_cast<std::int32_t>(*blocks / 16);
        }
        const bool keep = head->recipient < kSubCount && possess::StateKeeper::wants(head->id)
            && (head->recipient == 0 || headless(head->recipient) || g_view == head->recipient);
        const bool toView = head->recipient == g_view;
        if (toView && replay::bedrock::wanted(head->id, false)) {
            std::string tapped(length, '\0');
            if (copyGuarded(out, tapped.data(), length) && length > head->bodyOffset)
                replay::PacketTap::instance().push(false, head->id, reinterpret_cast<const std::uint8_t*>(tapped.data()) + head->bodyOffset,
                                                   length - head->bodyOffset);
        }
        const bool toHeadless = headless(head->recipient) && head->recipient != g_view;
        const bool rewriteView = toView && (g_view != 0 || g_swap.active || ((head->id == 0xa1 || head->id == 0x97) && g_tickOffset != 0));
        const bool readForHeadless = toHeadless && protocol::isReadPacket(head->id);
        if (toView && g_relogRouted && head->id == 2 && peeked >= head->bodyOffset + 4
            && std::memcmp(headBytes + head->bodyOffset, "\0\0\0\0", 4) == 0) continue;
        if (g_relogRouted && toHeadless && head->recipient == 0 && primaryOnly(head->id)) {
            if (g_viewStarted) return result;
            if (head->id == 0x88) continue;
            std::string held(length, '\0');
            if (!copyGuarded(out, held.data(), length)) return result;
            g_heldForView.push_back(std::move(held));
            continue;
        }
        const bool skinPacket = (head->id == 0x3f || head->id == 0x5d) && head->recipient < kSubCount;
        std::string whole;
        if (keep || rewriteView || readForHeadless || skinPacket) {
            whole.resize(length);
            if (!copyGuarded(out, whole.data(), length)) return result;
        }
        if (skinPacket && !g_skins.observe(head->id, protocol::bytes(whole).subspan(head->bodyOffset)) && g_skinReadFailures++ < 4) {
            std::wstring headHex;
            for (std::size_t i = head->bodyOffset; i < whole.size() && i < head->bodyOffset + 24; ++i)
                headHex += std::format(L"{:02x}", static_cast<unsigned char>(whole[i]));
            log().warn(L"Bot: cannot read the skins in received id {:#x} ({} bytes, body {}...)", head->id, length, headHex);
        }
        if (keep) {
            const auto body = protocol::bytes(whole).subspan(head->bodyOffset);
            g_keepers[head->recipient].observe(head->id, body);
        }
        if (toView) {
            if (!rewriteView) return result;
            const auto rewritten = possess::rewrite(whole, g_swap, 0, head->sender, -g_tickOffset);
            if (!rewritten) {
                if (g_rewriteFailures++ < 16) log().warn(L"Bot: cannot rewrite received id {:#x} ({} bytes); dropped", head->id, length);
                continue;
            }
            if (assignGameString(out, *rewritten)) {
                if (head->id == 0x0b && g_relogRouted && !g_viewStarted) {
                    g_viewStarted = true;
                    std::map<std::uint16_t, int> heldIds;
                    for (const auto& held : g_heldForView) if (const auto h = protocol::readHeader(protocol::bytes(held))) ++heldIds[h->id];
                    std::wstring kinds;
                    for (const auto& [id, count] : heldIds) kinds += std::format(L" {:#x}x{}", id, count);
                    log().info(L"Bot: the game got the bot's StartGame; handing {} primary-only packet(s):{}", g_heldForView.size(), kinds);
                    for (auto& held : g_heldForView) g_timed.push_back({nowMs(), std::move(held)});
                    g_heldForView.clear();
                }
                return 0;
            }
            if (g_assignFailures++ < 8) log().warn(L"Bot: cannot hand received id {:#x} to the game; dropped", head->id);
            continue;
        }
        if (!toHeadless) return result;
        ++g_perSub[head->recipient];
        if (!readForHeadless) continue;
        whole.erase(0, head->bodyOffset);
        if (g_queue.size() == 16384) {
            g_queue.pop_front(); ++g_dropped;
            if (!g_dropLogged) { g_dropLogged = true; log().warn(L"Bot: receive queue overflow; dropping oldest packets"); }
        }
        g_queue.push_back({head->id, head->recipient, std::move(whole)});
    }
}
namespace {
std::optional<std::string> onGameLogin(const std::string& data, std::size_t bodyOffset)
{
    ++g_loginCount;
    const auto body = protocol::bytes(data).subspan(bodyOffset);
    const auto parts = relog::splitLogin(body);
    const auto key = parts ? relog::clientDataKey(*parts) : std::string();
    g_gameKey = key;
    if (parts && !(g_mainSwapArm.armed && !g_mainSwapArm.nonce.empty())) g_userRequest = parts->request;
    g_sentRequest = parts ? parts->request : std::string();
    g_mainSwap = {}; g_mainSwapped = false; g_swap = {};
    if (!std::exchange(g_mainSwapArm.armed, false)) return std::nullopt;
    if (key.empty() || key != g_mainSwapArm.key) {
        g_mainSwapError = "the game's login key changed";
        log().warn(L"Bot: the game's login key is not the one the bot's token was issued for; logging in as your account");
        return std::nullopt;
    }
    auto replaced = relog::replaceLoginToken(body, g_mainSwapArm.token);
    if (!replaced) {
        g_mainSwapError = "cannot rewrite the game's login";
        log().warn(L"Bot: cannot rewrite the game's Login; logging in as your account");
        return std::nullopt;
    }
    if (const auto sent = relog::splitLogin(protocol::bytes(*replaced))) g_sentRequest = sent->request;
    g_mainSwap = g_mainSwapArm.ids; g_swap = g_mainSwap; g_mainSwapped = true;
    g_mainSwapArm = {};
    log().info(L"Bot: the game logs in with the bot's token ({} -> {} bytes)", data.size(), bodyOffset + replaced->size());
    return data.substr(0, bodyOffset) + *replaced;
}
}
void sendInternal(SendInternalFn original, void* system, void* id, void* packet, const std::string* data)
{
    void* const cns = g_cns.load(std::memory_order_acquire);
    if (!data || data->empty() || !cns || static_cast<std::byte*>(system) != static_cast<std::byte*>(cns) + kSystemFromCns)
        return original(system, id, packet, data);
    const auto head = protocol::readHeader(protocol::bytes(*data));
    if (!head || head->sender != 0) return original(system, id, packet, data);
    if (const auto filter = g_sendFilter.load(std::memory_order_acquire);
        filter && filter(head->id, protocol::bytes(*data).subspan(head->bodyOffset))) return;
    std::optional<std::string> rewritten;
    {
        std::lock_guard lock(g_mutex);
        const auto body = protocol::bytes(*data).subspan(head->bodyOffset);
        if (g_view == 0 && replay::bedrock::wanted(head->id, true) && !body.empty())
            replay::PacketTap::instance().push(true, head->id, reinterpret_cast<const std::uint8_t*>(body.data()), body.size());
        if (head->id == 0xc1 || head->id == 0x01) {
            if (head->id == 0xc1) { g_mainSwap = {}; g_mainSwapped = false; g_swap = {}; }
            if (g_view != 0 || g_headlessMask) { resetRouting(); log().info(L"Bot: the game started a new connection; routing reset"); }
            if (head->id == 0x01) {
                if (auto address = relog::readLoginServerAddress(body)) {
                    log().info(L"Bot: server address {}:{}", std::wstring(address->host.begin(), address->host.end()), address->port);
                    g_serverAddress = std::move(address);
                } else if (!std::exchange(g_loginUnreadableLogged, true)) log().warn(L"Bot: cannot read ServerAddress from the game's Login ({} bytes)", data->size());
                rewritten = onGameLogin(*data, head->bodyOffset);
            }
        }
        if (head->id == 0x90) if (const auto input = possess::readAuthInputHead(body)) g_lastInput = input;
        if (head->id == 0x87) rememberGameHits(body);
        if (head->id == 0x45 && g_view == 0) if (const auto radius = protocol::Reader(body).varInt32()) g_gameRadius = *radius;
        const bool plain = g_view == 0 && !g_swap.active && (g_tickOffset == 0 || head->id != 0x90);
        if (!plain && head->id != 0x01) {
            rewritten = possess::rewrite(*data, g_swap, head->recipient, g_view, g_tickOffset);
            if (!rewritten) {
                if (g_rewriteFailures++ < 16) log().warn(L"Bot: cannot rewrite sent id {:#x} ({} bytes); dropped", head->id, data->size());
                return;
            }
        }
    }
    original(system, id, packet, rewritten ? &*rewritten : data);
}
Incoming takeIncoming()
{
    std::lock_guard lock(g_mutex);
    Incoming result;
    result.perSub = g_perSub; result.dropped = g_dropped;
    g_perSub = {}; g_dropped = 0;
    result.packets.reserve(g_queue.size());
    while (!g_queue.empty()) { result.packets.push_back(std::move(g_queue.front())); g_queue.pop_front(); }
    return result;
}
bool connected() { return available() && firstConnection(g_cns.load(std::memory_order_acquire)) != nullptr; }
bool sendRaw(const std::string& bytes) { return available() && !bytes.empty() && sendGuarded(g_cns.load(std::memory_order_acquire), &bytes); }
std::optional<possess::StartGameHead> mainStartGame() { std::lock_guard lock(g_mutex); return g_mainStart; }
std::optional<possess::AuthInputHead> lastGameInput() { std::lock_guard lock(g_mutex); return g_lastInput; }
std::int32_t gameRadius() { std::lock_guard lock(g_mutex); return g_gameRadius > 0 ? g_gameRadius : g_serverRadius; }
void schedule(std::string bytes, std::uint64_t delayMs)
{
    if (bytes.empty()) return;
    std::lock_guard lock(g_mutex);
    g_timed.push_back({nowMs() + delayMs, std::move(bytes)});
}
void setKeeperSelf(std::uint8_t sub, std::uint64_t runtimeId)
{
    std::lock_guard lock(g_mutex);
    if (sub < kSubCount) g_keepers[sub].setSelf(runtimeId);
}
std::optional<possess::Uuid> playerUuid(std::uint8_t sub, std::uint64_t runtimeId)
{
    std::lock_guard lock(g_mutex);
    return sub >= kSubCount ? std::nullopt : g_keepers[sub].playerUuid(runtimeId);
}
std::optional<std::string> playerName(std::uint8_t sub, std::uint64_t runtimeId)
{
    std::lock_guard lock(g_mutex);
    return sub >= kSubCount ? std::nullopt : g_keepers[sub].playerName(runtimeId);
}
std::optional<possess::Uuid> listedUuid(std::int64_t uniqueId) { std::lock_guard lock(g_mutex); return g_keepers[0].listedUuid(uniqueId); }
std::optional<skin::ListedPlayer> listedSkin(const std::string& name) { std::lock_guard lock(g_mutex); return g_skins.find(name); }
std::vector<std::string> listedSkinNames() { std::lock_guard lock(g_mutex); return g_skins.names(); }
std::vector<std::string> keeperChunksOf(std::uint8_t sub, std::int32_t chunkX, std::int32_t chunkZ, std::int32_t dimension, std::int32_t radius)
{
    std::lock_guard lock(g_mutex);
    return sub < kSubCount ? g_keepers[sub].chunksNear(chunkX, chunkZ, dimension, radius) : std::vector<std::string>{};
}
possess::Ids currentIds() { std::lock_guard lock(g_mutex); return g_swap; }
std::optional<relog::ServerAddress> serverAddress() { std::lock_guard lock(g_mutex); return g_serverAddress; }
std::string gameLoginKey() { std::lock_guard lock(g_mutex); return g_gameKey; }
std::string userRequest() { std::lock_guard lock(g_mutex); return g_userRequest; }
std::string sentRequest() { std::lock_guard lock(g_mutex); return g_sentRequest; }
std::uint64_t loginCount() { std::lock_guard lock(g_mutex); return g_loginCount; }
void armMainSwap(std::string token, std::string key, const possess::Uuid& user, const possess::Uuid& bot, std::string nonce)
{
    std::lock_guard lock(g_mutex);
    g_mainSwapArm = {true, std::move(token), std::move(key), {}, std::move(nonce)};
    auto& ids = g_mainSwapArm.ids;
    ids.active = true;
    ids.runtimeA = ids.runtimeB = ~std::uint64_t{0};
    ids.uniqueA = ids.uniqueB = (std::numeric_limits<std::int64_t>::min)();
    ids.uuidA = user; ids.uuidB = bot; ids.uuids = true;
    g_mainSwapError.clear();
}
void disarmMainSwap() { std::lock_guard lock(g_mutex); g_mainSwapArm = {}; }
std::optional<std::string> armedLoginNonce()
{
    std::lock_guard lock(g_mutex);
    if (!g_mainSwapArm.armed || g_mainSwapArm.nonce.empty()) return std::nullopt;
    return g_mainSwapArm.nonce;
}
bool mainSwapped() { std::lock_guard lock(g_mutex); return g_mainSwapped; }
std::string takeMainSwapError() { std::lock_guard lock(g_mutex); return std::exchange(g_mainSwapError, {}); }
void clearRouting()
{
    std::lock_guard lock(g_mutex);
    if (g_view == 0 && !g_headlessMask && !g_swap.active) return;
    log().info(L"Bot: routing cleared before the game leaves (view {}, headless {:#x})", g_view, g_headlessMask);
    resetRouting();
}
void armRelog(bool toBot, std::optional<std::pair<possess::Uuid, possess::Uuid>> uuids, std::uint8_t view)
{
    std::lock_guard lock(g_mutex);
    g_relogArmed = toBot; g_relogUuids = uuids;
    g_relogView = view >= 1 && view < kSubCount ? view : std::uint8_t{1};
}
bool relogRouted() { std::lock_guard lock(g_mutex); return g_relogRouted; }
void setPump(void (*pump)()) { g_pump.store(pump, std::memory_order_release); }
void setPumpWanted(bool wanted) { g_pumpWanted.store(wanted, std::memory_order_release); }
bool timedPending() { std::lock_guard lock(g_mutex); return !g_timed.empty(); }
namespace {
std::atomic<ReceiveFn> g_receiveOriginal{nullptr};
void* connectionGuarded(void* cns)
{
    __try {
        auto* const conn = static_cast<const std::uint8_t*>(firstConnection(cns));
        return conn && conn[g_offsets.disconnect] == 0 ? const_cast<std::uint8_t*>(conn) : nullptr;
    } __except (memoryException(GetExceptionCode())) { return nullptr; }
}
}
void* mainConnection() { return available() ? connectionGuarded(g_cns.load(std::memory_order_acquire)) : nullptr; }
void* networkSystem()
{
    void* const cns = g_cns.load(std::memory_order_acquire);
    return available() && cns ? static_cast<std::byte*>(cns) + kSystemFromCns : nullptr;
}
std::uint32_t connectionPeerOffset() { return g_offsets.peer; }
std::uint32_t peerSendSlot() { return g_offsets.sendSlot; }
std::uint32_t peerReceiveSlot() { return g_offsets.receiveSlot; }
void setReceiveOriginal(ReceiveFn original) { g_receiveOriginal.store(original, std::memory_order_release); }
ReceiveFn receiveOriginal() { return g_receiveOriginal.load(std::memory_order_acquire); }
bool assignToGameString(void* out, const std::string& bytes) { return assignGameString(out, bytes); }
void setTerrainRoute(int sub, bool copyMisses)
{
    std::lock_guard lock(g_mutex);
    g_terrainSub = sub; g_terrainMisses = copyMisses;
    if (sub < 0 && !copyMisses) g_terrain.clear();
}
std::vector<std::string> takeTerrainPackets()
{
    std::lock_guard lock(g_mutex);
    std::vector<std::string> out(std::make_move_iterator(g_terrain.begin()), std::make_move_iterator(g_terrain.end()));
    g_terrain.clear();
    return out;
}
std::vector<std::uint64_t> gameCachedBlobs() { std::lock_guard lock(g_mutex); return {g_gameBlobs.begin(), g_gameBlobs.end()}; }
}
