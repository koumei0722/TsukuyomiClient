#include "RakNetLink.h"
#include "BotNet.h"
#include "BotSignatures.h"
#include "core/Logger.h"
#include "memory/Memory.h"
#include <Windows.h>
#include <cstring>
#include <format>

namespace tsukuyomi::bot::raknet {
namespace {
using u64 = std::uint64_t;
using Fn10 = u64(__fastcall*)(u64, u64, u64, u64, u64, u64, u64, u64, u64, u64);

struct Offsets {
    const void* global = nullptr;
    const void* create = nullptr;
    const void* unassigned = nullptr;
    const void* peerSendFn = nullptr;
    std::uint32_t unassignedSize = 0, timeoutSlot = 0, timeoutMs = 0, pingSlot = 0;
    std::uint32_t startupSlot = 0, connectSlot = 0, connectInterval = 0, connectAttempts = 0;
    std::uint32_t maxIncomingSlot = 0, shutdownSlot = 0, shutdownMs = 0, shutdownPriority = 0;
    std::uint32_t receiveSlot = 0, deallocSlot = 0, sendSlot = 0, sendPriority = 0;
    std::int32_t threadPriority = 0;
    std::uint32_t packetLength = 0, packetGuid = 0, packetIndex = 0, packetData = 0;
    std::uint32_t reliability[4]{};
    std::uint32_t peerRakPeer = 0, peerGuid = 0, addressSlot = 0;
} g;
bool g_ready = false;
std::atomic<unsigned> g_lastCode{0};
constexpr std::size_t kSocketDescriptorSize = 0x34, kSocketFamily = 0x22, kBlockingSocket = 0x2c;
constexpr std::uint16_t kAfInet = 2, kAfInet6 = 23;
constexpr std::uint8_t kConnectionRequestAccepted = 0x10, kConnectionAttemptFailed = 0x11, kAlreadyConnected = 0x12,
    kNoFreeIncomingConnections = 0x14, kDisconnectionNotification = 0x15, kConnectionLost = 0x16, kConnectionBanned = 0x17,
    kInvalidPassword = 0x18, kIncompatibleProtocolVersion = 0x19, kIpRecentlyConnected = 0x1a;
constexpr char kGamePacket = static_cast<char>(0xFE);

int callException(unsigned code)
{
    g_lastCode.store(code, std::memory_order_relaxed);
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR || code == 0xE06D7363u
        ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}
bool call(const void* fn, u64 a1, u64 a2 = 0, u64 a3 = 0, u64 a4 = 0, u64 a5 = 0, u64 a6 = 0, u64 a7 = 0, u64 a8 = 0, u64 a9 = 0, u64 a10 = 0,
          u64* result = nullptr)
{
    __try {
        const u64 value = reinterpret_cast<Fn10>(const_cast<void*>(fn))(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10);
        if (result) *result = value;
        return true;
    } __except (callException(GetExceptionCode())) { return false; }
}
template<class T> bool readAt(const void* at, T& out) { return memory::copyGuarded(at, &out, sizeof(T)); }
template<class T> bool imm(Site site, std::size_t offset, T& out)
{
    const auto* at = net::site(site);
    return at && readAt(at + offset, out);
}
const void* ripTarget(Site site, std::size_t relOffset, std::size_t instructionEnd)
{
    std::int32_t rel = 0;
    if (!imm(site, relOffset, rel)) return nullptr;
    const auto* target = net::site(site) + instructionEnd + rel;
    return memory::inGameModule(target) ? target : nullptr;
}
const void* slotOf(const void* object, std::uint32_t slot)
{
    const std::uint8_t* table = nullptr;
    const void* fn = nullptr;
    if (!readAt(object, table) || !memory::inGameModule(table) || !readAt(table + slot, fn) || !memory::inGameModule(fn)) return nullptr;
    return fn;
}
const void* functionStart(const void* inside)
{
    DWORD64 imageBase = 0;
    const auto* entry = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(inside), &imageBase, nullptr);
    return entry ? reinterpret_cast<const void*>(imageBase + entry->BeginAddress) : nullptr;
}
void* findRakNetPeer(void* peer)
{
    for (int depth = 0; depth < 8 && peer && memory::plausiblePointer(peer); ++depth) {
        if (slotOf(peer, net::peerSendSlot()) == g.peerSendFn) return peer;
        void* inner = nullptr;
        if (!readAt(static_cast<const std::uint8_t*>(peer) + 8, inner)) return nullptr;
        peer = inner;
    }
    return nullptr;
}
}

bool initialize()
{
    g_ready = false;
    auto* const create = net::site(Site::RakPeerCreateSite);
    auto* const send = net::site(Site::RakNetPeerSendSite);
    if (!create || !send) { log().warn(L"Bot: a RakNet signature is missing - bots on extra connections are unavailable on RakNet servers"); return false; }
    g.global = ripTarget(Site::RakPeerCreateSite, 0x3, 0x8);
    g.create = ripTarget(Site::RakPeerCreateSite, 0x1d, 0x21);
    g.unassigned = ripTarget(Site::RakPeerCreateSite, 0x69, 0x6d);
    g.peerSendFn = functionStart(send);
    const auto* table = ripTarget(Site::RakNetPeerSendSite, 0xf, 0x13);
    std::uint8_t b = 0;
    bool ok = g.global && g.create && g.unassigned && g.peerSendFn && table
        && imm(Site::RakPeerCreateSite, 0x73, g.unassignedSize) && imm(Site::RakPeerCreateSite, 0x85, g.timeoutSlot)
        && imm(Site::RakPeerCreateSite, 0x8d, g.timeoutMs) && imm(Site::RakPeerCreateSite, 0xaf, g.pingSlot)
        && imm(Site::RakPeerStartupSite, 0x9, b) && (g.startupSlot = b, true) && imm(Site::RakPeerStartupSite, 0xe, g.threadPriority)
        && imm(Site::RakPeerConnectSite, 0x6, b) && (g.connectSlot = b, true)
        && imm(Site::RakPeerConnectSite, 0x1e, g.connectInterval) && imm(Site::RakPeerConnectSite, 0x26, g.connectAttempts)
        && imm(Site::RakPeerShutdownSite, 0xd, b) && (g.maxIncomingSlot = b, true)
        && imm(Site::RakPeerShutdownSite, 0x23, b) && (g.shutdownSlot = b, true)
        && imm(Site::RakPeerShutdownSite, 0x25, g.shutdownMs) && imm(Site::RakPeerShutdownSite, 0x2e, g.shutdownPriority)
        && imm(Site::RakPeerReceiveSite, 0xd, g.receiveSlot) && imm(Site::RakPeerReceiveSite, 0x25, g.packetLength)
        && imm(Site::RakPacketGuidSite, 0x3, g.packetGuid) && imm(Site::RakPacketGuidSite, 0x12, g.packetIndex)
        && imm(Site::RakPacketDataSite, 0x12, g.packetData)
        && imm(Site::RakPeerDeallocSite, 0xd, g.deallocSlot)
        && imm(Site::RakNetPeerSendSite, 0x34, g.sendSlot) && imm(Site::RakNetPeerSendSite, 0x5d, g.sendPriority)
        && memory::copyGuarded(table, g.reliability, sizeof g.reliability)
        && imm(Site::RakNetPeerAddressSite, 0x3, b) && (g.peerRakPeer = b, true)
        && imm(Site::RakNetPeerAddressSite, 0x7, b) && (g.peerGuid = b, true)
        && imm(Site::RakNetPeerAddressSite, 0x13, g.addressSlot);
    for (const auto slot : {g.timeoutSlot, g.pingSlot, g.startupSlot, g.connectSlot, g.maxIncomingSlot, g.shutdownSlot, g.receiveSlot,
                            g.deallocSlot, g.sendSlot, g.addressSlot})
        ok = ok && slot && slot % 8 == 0 && slot < 0x800;
    ok = ok && g.packetIndex == g.packetGuid + 8 && g.packetLength > g.packetIndex && g.packetData > g.packetLength && g.packetData < 0x200
        && g.unassignedSize >= 0x10 && g.unassignedSize <= 0x88 && g.connectAttempts && g.connectAttempts < 100 && g.connectInterval < 10000
        && g.shutdownMs < 5000 && g.peerRakPeer % 8 == 0 && g.peerGuid % 8 == 0;
    for (const auto value : g.reliability) ok = ok && value < 16;
    if (!ok) { log().warn(L"Bot: cannot read the RakNet slots - bots on extra connections are unavailable on RakNet servers"); return false; }
    log().info(L"Bot: RakPeer slots startup {:#x} connect {:#x} receive {:#x} dealloc {:#x} send {:#x} shutdown {:#x} address {:#x}; packet guid +{:#x} length +{:#x} data +{:#x}",
        g.startupSlot, g.connectSlot, g.receiveSlot, g.deallocSlot, g.sendSlot, g.shutdownSlot, g.addressSlot, g.packetGuid, g.packetLength, g.packetData);
    g_ready = true;
    return true;
}
bool available() { return g_ready; }
bool isRakNet(void* mainPeer) { return g_ready && findRakNetPeer(mainPeer) != nullptr; }

std::optional<Address> remoteAddress(void* mainPeer)
{
    void* const peer = g_ready ? findRakNetPeer(mainPeer) : nullptr;
    void* rakPeer = nullptr;
    alignas(8) unsigned char guid[16]{};
    if (!peer || !readAt(static_cast<const std::uint8_t*>(peer) + g.peerRakPeer, rakPeer) || !memory::plausiblePointer(rakPeer)
        || !memory::copyGuarded(static_cast<const std::uint8_t*>(peer) + g.peerGuid, guid, sizeof guid)) return std::nullopt;
    const void* fn = slotOf(rakPeer, g.addressSlot);
    alignas(16) unsigned char out[0x100]{};
    if (!fn || !call(fn, reinterpret_cast<u64>(rakPeer), reinterpret_cast<u64>(out), reinterpret_cast<u64>(guid))) return std::nullopt;
    std::uint16_t family = 0, portBe = 0;
    std::memcpy(&family, out, 2); std::memcpy(&portBe, out + 2, 2);
    Address address;
    address.port = static_cast<std::uint16_t>((portBe >> 8) | (portBe << 8));
    if (family == kAfInet) address.host = std::format("{}.{}.{}.{}", out[4], out[5], out[6], out[7]);
    else if (family == kAfInet6) {
        for (int i = 0; i < 8; ++i)
            address.host += std::format("{}{:x}", i ? ":" : "", (out[8 + 2 * i] << 8) | out[9 + 2 * i]);
    } else return std::nullopt;
    if (!address.port) return std::nullopt;
    return address;
}

Link::~Link() { close(); }

bool Link::open(const Address& server, std::string& error)
{
    if (!g_ready) { error = "RakNet is unavailable"; return false; }
    if (state_ != State::Idle) { error = "already open"; return false; }
    void* global = nullptr;
    if (!readAt(g.global, global) || !global) { error = "the game's RakNet is not ready"; return false; }
    struct { void* destroy; void* peer; } made{};
    alignas(8) unsigned char noExtra[16]{};
    if (!call(g.create, reinterpret_cast<u64>(&made), reinterpret_cast<u64>(noExtra)) || !memory::plausiblePointer(made.peer)
        || !memory::inGameModule(made.destroy)) {
        error = "creating a RakPeer failed (" + std::to_string(g_lastCode.load()) + ")"; return false;
    }
    peer_ = made.peer; destroy_ = made.destroy;
    const auto p = reinterpret_cast<u64>(peer_);
    const void* timeout = slotOf(peer_, g.timeoutSlot);
    const void* ping = slotOf(peer_, g.pingSlot);
    const void* startup = slotOf(peer_, g.startupSlot);
    const void* connect = slotOf(peer_, g.connectSlot);
    if (!timeout || !ping || !startup || !connect) { fail("the RakPeer has unexpected slots"); error = reason_; return false; }
    call(timeout, p, g.timeoutMs, reinterpret_cast<u64>(g.unassigned));
    call(ping, p, 1);
    alignas(8) unsigned char descriptor[kSocketDescriptorSize]{};
    const std::uint16_t family = server.host.find(':') != std::string::npos ? kAfInet6 : kAfInet;
    std::memcpy(descriptor + kSocketFamily, &family, 2);
    descriptor[kBlockingSocket] = 1;
    u64 started = ~u64{0};
    if (!call(startup, p, 1, reinterpret_cast<u64>(descriptor), 1, static_cast<u64>(static_cast<std::uint32_t>(g.threadPriority)), 0, 0, 0, 0, 0, &started)
        || static_cast<std::uint32_t>(started) != 0) {
        fail("RakPeer::Startup failed (" + std::to_string(static_cast<std::uint32_t>(started)) + ")"); error = reason_; return false;
    }
    u64 attempt = ~u64{0};
    if (!call(connect, p, reinterpret_cast<u64>(server.host.c_str()), server.port, 0, 0, 0, 0, g.connectAttempts, g.connectInterval, 0, &attempt)
        || static_cast<std::uint32_t>(attempt) != 0) {
        fail("RakPeer::Connect failed (" + std::to_string(static_cast<std::uint32_t>(attempt)) + ")"); error = reason_; return false;
    }
    state_ = State::Connecting;
    since_ = GetTickCount64();
    log().info(L"Bot: extra connection: RakNet connecting to {}:{}", std::wstring(server.host.begin(), server.host.end()), server.port);
    return true;
}

void Link::tick()
{
    if (!peer_ || (state_ != State::Connecting && state_ != State::Open)) return;
    const auto p = reinterpret_cast<u64>(peer_);
    const void* receive = slotOf(peer_, g.receiveSlot);
    const void* dealloc = slotOf(peer_, g.deallocSlot);
    if (!receive || !dealloc) { fail("the RakPeer has unexpected slots"); return; }
    for (int count = 0; count < 4096; ++count) {
        u64 packet = 0;
        if (!call(receive, p, 0, 0, 0, 0, 0, 0, 0, 0, 0, &packet) || !packet) break;
        const auto* at = reinterpret_cast<const std::uint8_t*>(packet);
        std::uint32_t length = 0;
        const std::uint8_t* data = nullptr;
        std::uint8_t id = 0;
        if (readAt(at + g.packetLength, length) && readAt(at + g.packetData, data) && length && length <= (64u << 20) && readAt(data, id)) {
            if (id == static_cast<std::uint8_t>(kGamePacket)) {
                if (state_ == State::Open && length > 1) {
                    std::string payload(length - 1, '\0');
                    if (memory::copyGuarded(data + 1, payload.data(), payload.size())) incoming_.push_back(std::move(payload));
                }
            } else if (id == kConnectionRequestAccepted) {
                if (state_ == State::Connecting && memory::copyGuarded(at + g.packetGuid, guid_, sizeof guid_)) {
                    state_ = State::Open;
                    log().info(L"Bot: extra connection: RakNet connection accepted after {} ms", GetTickCount64() - since_);
                }
            } else {
                const char* why = nullptr;
                switch (id) {
                case kConnectionAttemptFailed: why = "the RakNet connection attempt failed"; break;
                case kAlreadyConnected: why = "the server says this address is already connected"; break;
                case kNoFreeIncomingConnections: why = "the server is full"; break;
                case kDisconnectionNotification: why = "the server closed the RakNet connection"; break;
                case kConnectionLost: why = "the RakNet connection was lost"; break;
                case kConnectionBanned: why = "banned from the server"; break;
                case kInvalidPassword: why = "the server rejected the RakNet password"; break;
                case kIncompatibleProtocolVersion: why = "the server uses another RakNet protocol version"; break;
                case kIpRecentlyConnected: why = "this IP connected too recently"; break;
                default: break;
                }
                if (why) { call(dealloc, p, packet); fail(why); return; }
            }
        }
        call(dealloc, p, packet);
    }
    if (state_ == State::Connecting && GetTickCount64() - since_ > kConnectTimeoutMs) fail("the RakNet connection timed out");
}

bool Link::sendBatch(const std::string& bytes, int reliability)
{
    if (state_ != State::Open || !peer_) return false;
    const void* send = slotOf(peer_, g.sendSlot);
    if (!send) return false;
    std::string framed;
    framed.reserve(bytes.size() + 1);
    framed.push_back(kGamePacket);
    framed += bytes;
    alignas(16) unsigned char target[0xa0]{};
    std::memcpy(target, guid_, sizeof guid_);
    memory::copyGuarded(g.unassigned, target + 0x10, g.unassignedSize);
    const auto rakReliability = reliability >= 0 && reliability < 4 ? g.reliability[reliability] : g.reliability[3];
    u64 receipt = 0;
    if (!call(send, reinterpret_cast<u64>(peer_), reinterpret_cast<u64>(framed.data()), framed.size(), g.sendPriority, rakReliability, 0,
              reinterpret_cast<u64>(target), 0, 0, 0, &receipt))
        return false;
    return static_cast<std::uint32_t>(receipt) != 0;
}

std::optional<std::string> Link::receiveBatch()
{
    if (incoming_.empty()) tick();
    if (incoming_.empty()) return std::nullopt;
    auto batch = std::move(incoming_.front());
    incoming_.pop_front();
    return batch;
}

void Link::fail(std::string reason)
{
    if (state_ == State::Closed) return;
    reason_ = std::move(reason);
    log().warn(L"Bot: extra connection (RakNet) closed: {}", std::wstring(reason_.begin(), reason_.end()));
    close();
}

void Link::close()
{
    if (peer_) {
        const auto p = reinterpret_cast<u64>(peer_);
        if (const void* fn = slotOf(peer_, g.maxIncomingSlot)) call(fn, p, 0);
        if (const void* fn = slotOf(peer_, g.shutdownSlot)) call(fn, p, g.shutdownMs, 0, g.shutdownPriority);
        if (!call(destroy_, p)) log().warn(L"Bot: destroying the extra connection's RakPeer faulted ({})", g_lastCode.load());
        peer_ = nullptr; destroy_ = nullptr;
    }
    incoming_.clear();
    if (state_ != State::Idle) state_ = State::Closed;
    if (reason_.empty() && state_ == State::Closed) reason_ = "closed";
}

}
