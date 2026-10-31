#include "NetherNetLink.h"
#include "Base64.h"
#include "BotNet.h"
#include "Crypto.h"
#include "GameIdentity.h"
#include "GameString.h"
#include "Jwt.h"
#include "core/Logger.h"
#include "memory/Memory.h"
#include <Windows.h>
#include <cstring>
#include <new>
#include <nlohmann/json.hpp>
#include <string_view>
#include <vector>

namespace tsukuyomi::bot::nethernet {
namespace {
using u64 = std::uint64_t;
using Fn8 = u64(__fastcall*)(u64, u64, u64, u64, u64, u64, u64, u64);

struct Offsets {
    std::uint32_t factoryLan = 0, factoryOptFlag = 0, factoryA0 = 0;
    std::uint32_t peerConnector = 0, peerRemote = 0, connectorTransport = 0, trickleFlag = 0;
    std::uint32_t slotSend = 0, slotAvailable = 0, slotRead = 0, slotSignaling = 0, slotOpen = 0, slotTrickleOn = 0, slotTrickleOff = 0;
    std::uint32_t slotLanOff = 0;
    std::uint32_t readyS = 0, readyX = 0, readyY = 0;
    bool readyKnown = false;
    std::uint32_t signalingIface = 0, signalingPeerId = 0;
    std::uint32_t signalingField = 0;
} g;
std::byte* g_createTransport = nullptr;
const void* g_peerSendFn = nullptr;
const void* g_signalingVtable = nullptr;
bool g_ready = false;
std::int32_t g_protocol = 0;
constexpr std::size_t kFactorySize = 0x100;
constexpr int kCallbackSlots = 9;

int memoryException(unsigned code)
{
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}
std::atomic<unsigned> g_lastCode{0};
int callException(unsigned code)
{
    g_lastCode.store(code, std::memory_order_relaxed);
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR || code == 0xE06D7363u
        ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}
template<class T> bool readAt(const void* at, T& out) { return memory::copyGuarded(at, &out, sizeof(T)); }
const void* functionStart(const void* inside)
{
    DWORD64 imageBase = 0;
    const auto* entry = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(inside), &imageBase, nullptr);
    return entry ? reinterpret_cast<const void*>(imageBase + entry->BeginAddress) : nullptr;
}
std::ptrdiff_t findMasked(const std::uint8_t* data, std::size_t size, std::string_view pattern)
{
    std::vector<int> bytes;
    for (std::size_t i = 0; i < pattern.size();) {
        if (pattern[i] == ' ') { ++i; continue; }
        if (pattern[i] == '?') { bytes.push_back(-1); ++i; continue; }
        bytes.push_back(std::stoi(std::string(pattern.substr(i, 2)), nullptr, 16)); i += 2;
    }
    for (std::size_t at = 0; at + bytes.size() <= size; ++at) {
        std::size_t k = 0;
        while (k < bytes.size() && (bytes[k] < 0 || data[at + k] == bytes[k])) ++k;
        if (k == bytes.size()) return static_cast<std::ptrdiff_t>(at);
    }
    return -1;
}

bool callGuarded(const void* fn, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64* result)
{
    __try {
        const u64 value = reinterpret_cast<Fn8>(const_cast<void*>(fn))(a1, a2, a3, a4, a5, 0, 0, 0);
        if (result) *result = value;
        return true;
    } __except (callException(GetExceptionCode())) { return false; }
}
bool call6Guarded(const void* fn, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64* result)
{
    __try {
        const u64 value = reinterpret_cast<Fn8>(const_cast<void*>(fn))(a1, a2, a3, a4, a5, a6, 0, 0);
        if (result) *result = value;
        return true;
    } __except (callException(GetExceptionCode())) { return false; }
}
const void* slotOf(void* object, std::uint32_t slot)
{
    __try {
        const auto* table = *reinterpret_cast<const std::uint8_t* const*>(object);
        if (!memory::inGameModule(table)) return nullptr;
        const void* fn = *reinterpret_cast<const void* const*>(table + slot);
        return memory::inGameModule(fn) ? fn : nullptr;
    } __except (memoryException(GetExceptionCode())) { return nullptr; }
}
std::uint64_t readQword(const void* at)
{
    std::uint64_t value = 0;
    return memory::isReadable(at, sizeof value) && memory::copyGuarded(at, &value, sizeof value) ? value : 0;
}
void* findPointerInto(const void* object, std::size_t length, const void* begin, std::size_t span)
{
    const auto* p = static_cast<const std::uint8_t*>(object);
    const auto lo = reinterpret_cast<std::uintptr_t>(begin);
    for (std::size_t off = 0; off + 8 <= length; off += 8) {
        const auto v = readQword(p + off);
        if (v > lo && v < lo + span && memory::inGameModule(reinterpret_cast<const void*>(readQword(reinterpret_cast<const void*>(v)))))
            return reinterpret_cast<void*>(v);
    }
    return nullptr;
}
void* findNetherNetPeer(void* peer)
{
    for (int depth = 0; depth < 6 && peer && memory::plausiblePointer(peer); ++depth) {
        const auto table = readQword(peer);
        if (table && memory::inGameModule(reinterpret_cast<const void*>(table))
            && readQword(reinterpret_cast<const void*>(table + 8)) == reinterpret_cast<std::uintptr_t>(g_peerSendFn)) return peer;
        peer = reinterpret_cast<void*>(readQword(static_cast<const std::uint8_t*>(peer) + 8));
    }
    return nullptr;
}
void addRef(void* control) { if (control) InterlockedIncrement(reinterpret_cast<volatile long*>(static_cast<std::uint8_t*>(control) + 8)); }
void release(void* control) { if (control) InterlockedDecrement(reinterpret_cast<volatile long*>(static_cast<std::uint8_t*>(control) + 8)); }

Link* ownerOf(u64 self) { return reinterpret_cast<Link* const*>(self)[1]; }
u64 __fastcall cbDestroy(u64 self, u64, u64, u64, u64, u64, u64, u64) { return self; }
u64 __fastcall cbFlags(u64, u64, u64 flags, u64, u64, u64, u64, u64)
{
    if (flags) *reinterpret_cast<std::uint8_t*>(flags) |= 2;
    return 0;
}
template<int Slot>
u64 __fastcall cbForward(u64 self, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7, u64 a8)
{
    void* const game = ownerOf(self)->gameCallbacks();
    const auto fn = reinterpret_cast<Fn8>((*reinterpret_cast<void* const* const*>(game))[Slot]);
    return fn(reinterpret_cast<u64>(game), a2, a3, a4, a5, a6, a7, a8);
}
u64 __fastcall cbOpened(u64 self, u64, u64 connectionId, u64, u64, u64, u64, u64)
{
    ownerOf(self)->onOpened(connectionId);
    return 0;
}
template<int Slot>
u64 __fastcall cbEvent(u64 self, u64 a2, u64 a3, u64 a4, u64, u64, u64, u64)
{
    ownerOf(self)->onEvent(Slot, a2, a3, a4);
    return 0;
}
u64 __fastcall cbNothing(u64, u64, u64, u64, u64, u64, u64, u64) { return 0; }
u64 __fastcall cbEmptyValue(u64 self, u64 out, u64 a3, u64 a4, u64, u64, u64, u64)
{
    std::memset(reinterpret_cast<void*>(out), 0, 16);
    (void)self; (void)a3; (void)a4;
    return out;
}
void* const kCallbackTable[kCallbackSlots] = {
    reinterpret_cast<void*>(&cbDestroy), reinterpret_cast<void*>(&cbFlags), reinterpret_cast<void*>(&cbForward<2>),
    reinterpret_cast<void*>(&cbForward<3>), reinterpret_cast<void*>(&cbOpened), reinterpret_cast<void*>(&cbEvent<5>),
    reinterpret_cast<void*>(&cbNothing), reinterpret_cast<void*>(&cbEvent<7>), reinterpret_cast<void*>(&cbEmptyValue),
};
}

struct Link::Identity {
    void* const* vtable;
    Link* owner;
    std::string domain;
    struct Control { void* const* vtable; long uses; long weaks; } control;
};
namespace {
using Identity = Link::Identity;
u64 __fastcall idDestroy(u64 self, u64, u64, u64, u64, u64, u64, u64) { return self; }
u64 __fastcall idAssert(u64 self, u64 out, u64 contents, u64, u64, u64, u64, u64)
{
    auto* const result = reinterpret_cast<std::uint8_t*>(out);
    if (!memory::isWritable(result, 0x21)) return out;
    result[0x20] = 0;
    const auto* identity = reinterpret_cast<const Identity*>(self);
    try {
        if (auto assertion = identity->owner->makeAssertion(*reinterpret_cast<const std::string*>(contents))) {
            gamestring::initialize(result);
            if (gamestring::assign(result, *assertion)) result[0x20] = 1;
        }
    } catch (...) {}
    return out;
}
u64 __fastcall idDomain(u64 self, u64 out, u64, u64, u64, u64, u64, u64)
{
    const auto& domain = reinterpret_cast<const Identity*>(self)->domain;
    reinterpret_cast<const char**>(out)[0] = domain.data();
    reinterpret_cast<std::size_t*>(out)[1] = domain.size();
    return out;
}
constexpr std::string_view kProtocol = "default";
u64 __fastcall idProtocol(u64, u64 out, u64, u64, u64, u64, u64, u64)
{
    reinterpret_cast<const char**>(out)[0] = kProtocol.data();
    reinterpret_cast<std::size_t*>(out)[1] = kProtocol.size();
    return out;
}
void* const kIdentityTable[] = {reinterpret_cast<void*>(&idDestroy), reinterpret_cast<void*>(&idAssert),
                                reinterpret_cast<void*>(&idDomain), reinterpret_cast<void*>(&idProtocol)};
u64 __fastcall ctrlNothing(u64, u64, u64, u64, u64, u64, u64, u64) { return 0; }
void* const kControlTable[] = {reinterpret_cast<void*>(&ctrlNothing), reinterpret_cast<void*>(&ctrlNothing), reinterpret_cast<void*>(&ctrlNothing)};
constexpr char kDefaultDomain[] = "https://authorization.franchise.minecraft-services.net/";
}

bool initialize()
{
    g_ready = false;
    auto* const create = net::site(Site::NetherNetCreateTransport);
    auto* const peerSend = net::site(Site::NetherNetPeerSendSite);
    auto* const flag = net::site(Site::NetherNetConnectFlagSite);
    auto* const signaling = net::site(Site::NetherNetSetSignalingSite);
    auto* const openSession = net::site(Site::NetherNetOpenSessionSite);
    auto* const availableSite = net::site(Site::NetherNetAvailableSite);
    auto* const readSite = net::site(Site::NetherNetReadSite);
    auto* const vtableSite = net::site(Site::HttpSignalingVtableSite);
    auto* const peerIdSite = net::site(Site::HttpSignalingPeerIdSite);
    if (!create || !peerSend || !flag || !signaling || !openSession || !availableSite || !readSite || !vtableSite || !peerIdSite) {
        log().warn(L"Bot: a NetherNet signature is missing - bots on extra connections are unavailable");
        return false;
    }
    std::uint8_t b = 0;
    std::uint32_t d = 0, transportAgain = 0;
    std::int32_t rel = 0;
    bool ok = readAt(peerSend + 12, b) && (g.peerConnector = b, true)
        && readAt(peerSend + 29, b) && (g.peerRemote = b, true)
        && readAt(peerSend + 38, d) && (g.connectorTransport = d, true)
        && readAt(peerSend + 48, b) && (g.slotSend = b, true)
        && readAt(flag + 3, transportAgain)
        && readAt(flag + 12, d) && (g.trickleFlag = d, true)
        && readAt(flag + 22, d) && (g.slotTrickleOn = d, true)
        && readAt(flag + 38, d) && (g.slotTrickleOff = d, true)
        && readAt(signaling + 13, b) && (g.slotSignaling = b, true)
        && readAt(openSession + 6, b) && (g.slotOpen = b, true)
        && readAt(availableSite + 37, b) && (g.slotAvailable = b, true)
        && readAt(readSite + 13, b) && (g.slotRead = b, true)
        && readAt(vtableSite + 3, rel)
        && readAt(peerIdSite + 3, d) && (g.signalingPeerId = d, true);
    g.signalingIface = 0;
    std::uint8_t body[0xa00]{};
    ok = ok && memory::copyGuarded(create, body, sizeof body);
    const auto a0 = findMasked(body, sizeof body, "8B 87 ? ? ? ? 89 85");
    const auto opt = findMasked(body, sizeof body, "80 7F ? 01 75 ? 8B 47 ? 89 85");
    const auto lan = findMasked(body, sizeof body, "80 7F ? 01 75 ? 48 8B 80 ? ? ? ? 48 89 D9 FF 15 ? ? ? ? 90 EB ? 48 8B 40 ?");
    if (ok && a0 >= 0 && opt >= 0 && lan >= 0) {
        std::memcpy(&g.factoryA0, body + a0 + 2, 4);
        g.factoryOptFlag = body[opt + 2];
        g.factoryLan = body[lan + 2];
        g.slotLanOff = body[lan + 28];
    } else ok = false;
    if (auto* const protocolSite = net::site(Site::RequestNetworkSettingsSite)) readAt(protocolSite + 14, g_protocol);
    if (g_protocol <= 0 || g_protocol > 0x10000) { g_protocol = 0; ok = false; }
    g_createTransport = create;
    g_peerSendFn = functionStart(peerSend);
    g_signalingVtable = vtableSite + 7 + rel;
    ok = ok && g_peerSendFn && transportAgain == g.connectorTransport && g.peerRemote < 0x100 && g.peerRemote % 8 == 0
        && g.slotTrickleOn < 0x400 && g.slotTrickleOff < 0x400 && g.slotTrickleOn % 8 == 0 && g.slotTrickleOff % 8 == 0 && g.factoryA0 + 4 <= kFactorySize
        && g.factoryOptFlag < kFactorySize && g.factoryLan < kFactorySize && g.signalingPeerId > g.signalingIface && g.signalingPeerId < 0x400;
    if (!ok) { log().warn(L"Bot: cannot read the NetherNet field offsets - bots on extra connections are unavailable"); return false; }
    log().info(L"Bot: network protocol {}", g_protocol);
    log().info(L"Bot: NetherNet peer +{:#x}/+{:#x} transport +{:#x} slots send {:#x} avail {:#x} read {:#x} signaling {:#x} open {:#x} trickle +{:#x} ({:#x}/{:#x}) lan +{:#x}/{:#x} signaling iface +{:#x} peer id +{:#x}",
        g.peerConnector, g.peerRemote, g.connectorTransport, g.slotSend, g.slotAvailable, g.slotRead, g.slotSignaling, g.slotOpen,
        g.trickleFlag, g.slotTrickleOn, g.slotTrickleOff, g.factoryLan, g.slotLanOff, g.signalingIface, g.signalingPeerId);
    g_ready = true;
    return true;
}
bool available() { return g_ready; }
bool isNetherNet(void* mainPeer) { return g_ready && findNetherNetPeer(mainPeer) != nullptr; }
std::optional<ServerId> serverId(void* mainPeer)
{
    void* const peer = g_ready ? findNetherNetPeer(mainPeer) : nullptr;
    Link::NetworkId remote{};
    if (!peer || !readAt(static_cast<const std::uint8_t*>(peer) + g.peerRemote, remote)) return std::nullopt;
    if ((remote.type != 1 && remote.type != 2) || (remote.id == 0 && (remote.type == 1 || remote.extra == 0))) return std::nullopt;
    return ServerId{remote.id, remote.type == 2 ? remote.extra : 0, remote.type};
}
std::int32_t protocolVersion() { return g_protocol; }

Link::Link() = default;
Link::~Link() { close(); }
std::string Link::closeReason() const { std::lock_guard lock(mutex_); return closeReason_; }

bool Link::open(void* mainPeer, std::string token, std::shared_ptr<crypto::EcKey> key, std::string& error)
{
    if (!g_ready) { error = "NetherNet is unavailable"; return false; }
    if (state_.load() != State::Idle) { error = "already open"; return false; }
    void* const peer = findNetherNetPeer(mainPeer);
    if (!peer) { error = "the main connection is not NetherNet"; return false; }
    void* connector = nullptr; void* mainTransport = nullptr;
    if (!readAt(static_cast<std::uint8_t*>(peer) + g.peerConnector, connector) || !memory::plausiblePointer(connector)
        || !readAt(static_cast<std::uint8_t*>(connector) + g.connectorTransport, mainTransport) || !memory::plausiblePointer(mainTransport)) {
        error = "cannot read the NetherNet connector"; return false;
    }
    gameCallbacks_ = findPointerInto(mainTransport, 0x80, connector, 0x400);
    if (!g.signalingField) {
        if (const void* setFn = slotOf(mainTransport, g.slotSignaling)) {
            std::uint8_t code[0x100]{};
            if (memory::copyGuarded(setFn, code, sizeof code))
                if (const auto at = findMasked(code, sizeof code, "4C 89 B6 ? ? ? ? 4C 8B B6 ? ? ? ? 4C 89 BE"); at >= 0) {
                    std::uint32_t a = 0, b = 0, c = 0;
                    std::memcpy(&a, code + at + 3, 4); std::memcpy(&b, code + at + 10, 4); std::memcpy(&c, code + at + 17, 4);
                    if (a % 8 == 0 && a < 0x1000 && b == a + 8 && c == a + 8) g.signalingField = a;
                }
        }
        log().info(L"Bot: NetherNet transport signaling field +{:#x}", g.signalingField);
    }
    void* iface = nullptr; void* control = nullptr; std::uint8_t trickle = 0;
    const bool haveSignaling = gameCallbacks_ && g.signalingField
        && readAt(static_cast<std::uint8_t*>(mainTransport) + g.signalingField, iface) && memory::plausiblePointer(iface)
        && readAt(static_cast<std::uint8_t*>(mainTransport) + g.signalingField + 8, control) && memory::plausiblePointer(control);
    void* ifaceVtable = nullptr;
    const bool httpSignaling = haveSignaling && readAt(iface, ifaceVtable) && ifaceVtable == g_signalingVtable;
    if (!haveSignaling
        || !(httpSignaling ? readAt(static_cast<std::uint8_t*>(iface) - g.signalingIface + g.signalingPeerId, remoteId_)
                           : readAt(static_cast<std::uint8_t*>(peer) + g.peerRemote, remoteId_))
        || !readAt(static_cast<std::uint8_t*>(connector) + g.trickleFlag, trickle)) {
        error = "cannot read the main connection's signaling";
        return false;
    }
    if (!httpSignaling)
        log().info(L"Bot: extra connection: the main connection's signaling is not HttpSignalingClient (vtable RVA {:#x})",
            memory::inGameModule(ifaceVtable) ? reinterpret_cast<std::uintptr_t>(ifaceVtable) - reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)) : 0);
    std::uint8_t lan = 0;
    if (const void* lanOff = slotOf(mainTransport, g.slotLanOff)) {
        std::uint8_t code[0x20]{};
        if (memory::copyGuarded(lanOff, code, sizeof code))
            if (const auto at = findMasked(code, sizeof code, "80 B9 ? ? ? ? 01"); at >= 0) {
                std::uint32_t field = 0; std::memcpy(&field, code + at + 2, 4);
                if (field < 0x1000) readAt(static_cast<std::uint8_t*>(mainTransport) + field, lan);
            }
    }
    gameIdentity_ = token.empty() && !key;
    if (gameIdentity_ && !gameidentity::held()) { error = "the game's identity is not available"; return false; }
    token_ = std::move(token); key_ = std::move(key);
    identity_ = std::make_unique<Identity>();
    identity_->vtable = kIdentityTable; identity_->owner = this;
    identity_->domain = kDefaultDomain;
    const auto domainToken = gameIdentity_ ? gameidentity::token().value_or(std::string()) : token_;
    if (const auto payload = jwt::readPayload(domainToken); payload && payload->contains("iss") && (*payload)["iss"].is_string())
        identity_->domain = (*payload)["iss"].get<std::string>();
    identity_->control = {kControlTable, 1, 1};
    callbacks_ = {kCallbackTable, this};
    const auto random = crypto::randomBytes(8);
    if (random.size() != 8) { error = "no random"; return false; }
    std::memcpy(&localId_.id, random.data(), 8);
    localId_.id |= 1; localId_.extra = 0; localId_.type = 1;
    alignas(16) std::uint8_t factory[kFactorySize]{};
    std::uint32_t one = 1;
    std::memcpy(factory + g.factoryA0, &one, 4);
    factory[g.factoryOptFlag] = 0;
    factory[g.factoryLan] = lan;
    std::memset(transportHolder_, 0, sizeof transportHolder_);
    log().info(L"Bot: extra connection: creating a transport (lan {}, trickle {}, server id {:#x}, {} signaling)", lan, trickle, remoteId_.id,
        httpSignaling ? L"server" : L"other");
    if (!callGuarded(g_createTransport, reinterpret_cast<u64>(factory), reinterpret_cast<u64>(transportHolder_),
                     reinterpret_cast<u64>(&localId_), reinterpret_cast<u64>(&callbacks_), 0, nullptr)) {
        error = "createTransport faulted (" + std::to_string(g_lastCode.load()) + ")"; return false;
    }
    std::memcpy(&transport_, transportHolder_ + 0x40, sizeof transport_);
    if (!memory::plausiblePointer(transport_)) { error = "createTransport returned nothing"; return false; }
    holderValid_ = true;
    log().info(L"Bot: extra connection: transport created; setting the signaling");
    void* shared[2]{iface, control};
    addRef(control);
    const void* setSignaling = slotOf(transport_, g.slotSignaling);
    const bool signalingOk = setSignaling && callGuarded(setSignaling, reinterpret_cast<u64>(transport_), reinterpret_cast<u64>(shared), 0, 0, 0, nullptr);
    if (shared[1]) release(shared[1]);
    if (!signalingOk) { error = "setting the signaling faulted (" + std::to_string(g_lastCode.load()) + ")"; close(); return false; }
    log().info(L"Bot: extra connection: signaling set");
    if (const void* trickleSlot = slotOf(transport_, trickle == 1 ? g.slotTrickleOn : g.slotTrickleOff))
        callGuarded(trickleSlot, reinterpret_cast<u64>(transport_), 0, 0, 0, 0, nullptr);
    if (!g.readyKnown) {
        if (const void* openFn = slotOf(transport_, g.slotOpen)) {
            std::uint8_t code[0x1000]{};
            if (memory::copyGuarded(openFn, code, sizeof code)) {
                const auto s = findMasked(code, sizeof code, "48 8B 73 ? 0F B6 9B");
                const auto x = findMasked(code, sizeof code, "48 8B BE ? ? ? ? 48 8B 0D");
                const auto y = findMasked(code, sizeof code, "4C 8B 7F ? 4D 85 FF");
                if (s >= 0 && x >= 0 && y >= 0) {
                    g.readyS = code[s + 3]; std::memcpy(&g.readyX, code + x + 3, 4); g.readyY = code[y + 3];
                    g.readyKnown = g.readyX < 0x1000;
                }
            }
        }
        log().info(L"Bot: extra connection: transport readiness field {} (+{:#x} +{:#x} +{:#x})", g.readyKnown, g.readyS, g.readyX, g.readyY);
    }
    createdAt_ = GetTickCount64();
    state_.store(State::Creating, std::memory_order_release);
    return true;
}

bool Link::ready() const
{
    if (!g.readyKnown || !transport_) return false;
    std::uint64_t s = 0, x = 0, y = 0;
    return readAt(static_cast<const std::uint8_t*>(transport_) + g.readyS, s) && s
        && readAt(reinterpret_cast<const std::uint8_t*>(s) + g.readyX, x) && x
        && readAt(reinterpret_cast<const std::uint8_t*>(x) + g.readyY, y) && y;
}

void Link::tick()
{
    if (state_.load(std::memory_order_acquire) != State::Creating) return;
    const auto waited = GetTickCount64() - createdAt_;
    if (g.readyKnown ? !ready() : waited < 2000) {
        if (waited > 10000) { { std::lock_guard lock(mutex_); closeReason_ = "the transport did not become ready"; } close(); }
        return;
    }
    log().info(L"Bot: extra connection: transport ready after {} ms; opening the session", waited);
    if (!openSession()) close();
}

bool Link::openSession()
{
    void* identityShared[2]{identity_.get(), &identity_->control};
    InterlockedIncrement(&identity_->control.uses);
    state_.store(State::Opening, std::memory_order_release);
    const void* openFn = slotOf(transport_, g.slotOpen);
    if (!openFn || !callGuarded(openFn, reinterpret_cast<u64>(transport_), reinterpret_cast<u64>(&remoteId_), reinterpret_cast<u64>(identityShared), 0, 0, nullptr)) {
        std::lock_guard lock(mutex_);
        closeReason_ = "opening the session faulted (" + std::to_string(g_lastCode.load()) + ")";
        return false;
    }
    log().info(L"Bot: extra connection opening (local id {:#x}, server id {:#x})", localId_.id, remoteId_.id);
    return true;
}

void Link::onOpened(std::uint64_t connectionId)
{
    connectionId_ = connectionId;
    state_.store(State::Open, std::memory_order_release);
    log().info(L"Bot: extra connection opened (connection {:#x})", connectionId);
}

void Link::onEvent(int slot, std::uint64_t a2, std::uint64_t a3, std::uint64_t a4)
{
    if (slot == 5 && a3 == connectionId_ && state_.load(std::memory_order_acquire) == State::Open) {
        { std::lock_guard lock(mutex_); closeReason_ = "the NetherNet session closed (" + std::to_string(a4) + ")"; }
        state_.store(State::Closed, std::memory_order_release);
    }
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 24) log().info(L"Bot: extra connection callback {} ({:#x}, {:#x}, {:#x})", slot, a2, a3, a4);
}

std::optional<std::string> Link::makeAssertion(const std::string& fingerprints) const
{
    if (gameIdentity_) return gameidentity::assertWith(fingerprints);
    if (!key_ || token_.empty()) return std::nullopt;
    static const std::string header = base64::encodeUrl(std::string_view(R"({"alg":"ES384"})"));
    const std::string input = header + '.' + base64::encodeUrl(std::string_view(fingerprints));
    const auto digest = crypto::sha384({reinterpret_cast<const std::uint8_t*>(input.data()), input.size()});
    const auto signature = key_->signDigest(digest);
    if (signature.size() != 96) return std::nullopt;
    const nlohmann::json assertion{{"fingerprints", header + ".." + base64::encodeUrl(signature)}, {"token", token_}};
    return assertion.dump();
}

bool Link::send(const std::string& bytes)
{
    if (state_.load(std::memory_order_acquire) != State::Open || !transport_) return false;
    const void* fn = slotOf(transport_, g.slotSend);
    u64 result = 0;
    return fn && callGuarded(fn, reinterpret_cast<u64>(transport_), reinterpret_cast<u64>(&remoteId_), connectionId_,
                             reinterpret_cast<u64>(&bytes), 1, &result);
}

std::optional<std::string> Link::receive()
{
    if (state_.load(std::memory_order_acquire) != State::Open || !transport_) return std::nullopt;
    const void* available = slotOf(transport_, g.slotAvailable);
    const void* read = slotOf(transport_, g.slotRead);
    std::uint32_t size = 0;
    u64 has = 0;
    if (!available || !read || !callGuarded(available, reinterpret_cast<u64>(transport_), reinterpret_cast<u64>(&remoteId_), connectionId_,
                                            reinterpret_cast<u64>(&size), 0, &has) || !(has & 0xff) || !size || size > (64u << 20))
        return std::nullopt;
    std::string buffer(size, '\0');
    std::uint32_t got = 0;
    u64 ok = 0;
    if (!call6Guarded(read, reinterpret_cast<u64>(transport_), reinterpret_cast<u64>(&remoteId_), connectionId_,
                      reinterpret_cast<u64>(buffer.data()), size, reinterpret_cast<u64>(&got), &ok) || !(ok & 0xff))
        return std::nullopt;
    buffer.resize((std::min)(got, size));
    return buffer;
}

void Link::close()
{
    if (holderValid_) {
        holderValid_ = false;
        void* impl = nullptr;
        std::memcpy(&impl, transportHolder_ + 0x38, sizeof impl);
        if (const void* doCall = slotOf(impl, 0x10)) {
            void* pointer = transport_;
            if (!callGuarded(doCall, reinterpret_cast<u64>(impl), reinterpret_cast<u64>(&pointer), 0, 0, 0, nullptr))
                log().warn(L"Bot: destroying the extra connection's transport faulted");
        }
    }
    transport_ = nullptr;
    if (state_.load() != State::Idle) {
        state_.store(State::Closed, std::memory_order_release);
        std::lock_guard lock(mutex_);
        if (closeReason_.empty()) closeReason_ = "closed";
    }
    identity_.reset();
}

}
