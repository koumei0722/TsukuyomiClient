#include "GameConnection.h"
#include "GameString.h"
#include "BotNet.h"
#include "BotSignatures.h"
#include "NetworkLayout.h"
#include "core/Logger.h"
#include "memory/Memory.h"
#include <Windows.h>
#include <cstring>
#include <malloc.h>
#include <vector>

namespace tsukuyomi::bot::gameconn {
namespace {
using u64 = std::uint64_t;
using Fn9 = u64(__fastcall*)(u64, u64, u64, u64, u64, u64, u64, u64, u64);

struct Offsets {
    const void* statsFn = nullptr;
    const void* ctor = nullptr;
    const void* dtor = nullptr;
    const void* enableEncryption = nullptr;
    std::uint32_t size = 0, idSize = 0;
    std::uint32_t nsArg5Ptr = 0, nsArg5Ctrl = 0, nsArg5Extra = 0, nsArg6 = 0, nsArg7 = 0;
    std::uint32_t options = 0, optionsText = 0, optionsTag = 0;
    std::uint32_t updateSlot = 0;
    std::uint32_t compressedPtr = 0;
    std::uint8_t compEnabled = 0, compThreshold = 0, compAlgorithm = 0, compZero16a = 0, compZero32a = 0, compZero16b = 0, compZero32b = 0;
    std::uint32_t peerOffset = 0, sendSlot = 0;
} g;
bool g_ready = false;
std::int32_t g_protocol = 0;
std::atomic<unsigned> g_lastCode{0};
constexpr std::size_t kNetworkStatusSize = 0x48;

int memoryException(unsigned code)
{
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}
int callException(unsigned code)
{
    g_lastCode.store(code, std::memory_order_relaxed);
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR || code == 0xE06D7363u
        ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}
bool call9(const void* fn, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7, u64 a8, u64 a9, u64* result)
{
    __try {
        const u64 value = reinterpret_cast<Fn9>(const_cast<void*>(fn))(a1, a2, a3, a4, a5, a6, a7, a8, a9);
        if (result) *result = value;
        return true;
    } __except (callException(GetExceptionCode())) { return false; }
}
bool call4(const void* fn, u64 a1, u64 a2, u64 a3, u64 a4, u64* result = nullptr) { return call9(fn, a1, a2, a3, a4, 0, 0, 0, 0, 0, result); }
const void* slotOf(const void* object, std::uint32_t slot)
{
    __try {
        const auto* table = *static_cast<const std::uint8_t* const*>(object);
        if (!memory::inGameModule(table)) return nullptr;
        const void* fn = *reinterpret_cast<const void* const*>(table + slot);
        return memory::inGameModule(fn) ? fn : nullptr;
    } __except (memoryException(GetExceptionCode())) { return nullptr; }
}
template<class T> bool readAt(const void* at, T& out) { return memory::copyGuarded(at, &out, sizeof(T)); }
template<class T> bool imm(Site site, std::size_t offset, T& out)
{
    const auto* at = net::site(site);
    return at && readAt(at + offset, out);
}
const void* relTarget(Site site, std::size_t relOffset)
{
    std::int32_t rel = 0;
    if (!imm(site, relOffset, rel)) return nullptr;
    const auto* target = net::site(site) + relOffset + 4 + rel;
    return memory::inGameModule(target) ? target : nullptr;
}
void addRef(void* control) { if (control) InterlockedIncrement(reinterpret_cast<volatile long*>(static_cast<std::uint8_t*>(control) + 8)); }
bool copyGameString(const void* source, std::string& out)
{
    std::uint64_t size = 0, capacity = 0;
    const auto* p = static_cast<const std::uint8_t*>(source);
    if (!readAt(p + 0x10, size) || !readAt(p + 0x18, capacity) || size > capacity || size > (64u << 20)) return false;
    const void* data = p;
    if (capacity >= 16 && !readAt(p, data)) return false;
    out.resize(static_cast<std::size_t>(size));
    return size == 0 || memory::copyGuarded(data, out.data(), static_cast<std::size_t>(size));
}
}

struct Connection::Peer {
    void* const* vtable;
    void* inner = nullptr;
    void* innerControl = nullptr;
    Transport* transport = nullptr;
    struct Control { void* const* vtable; long uses; long weaks; } control;
};
namespace {
using Peer = Connection::Peer;
Peer* peerOf(u64 self) { return reinterpret_cast<Peer*>(self); }
u64 __fastcall peerDestroy(u64 self, u64, u64, u64) { return self; }
u64 __fastcall peerSend(u64 self, u64 data, u64 reliability, u64)
{
    try { peerOf(self)->transport->sendBatch(*reinterpret_cast<const std::string*>(data), static_cast<int>(reliability & 0xff)); }
    catch (...) {}
    return 0;
}
u64 __fastcall peerStatus(u64, u64 out, u64, u64)
{
    std::memset(reinterpret_cast<void*>(out), 0, kNetworkStatusSize);
    return out;
}
u64 __fastcall peerNothing(u64, u64, u64, u64) { return 0; }
u64 __fastcall peerEncrypted(u64 self, u64, u64, u64) { return peerOf(self)->transport->encryptsItself() ? 1 : 0; }
u64 __fastcall peerReceive(u64 self, u64 out, u64, u64)
{
    try {
        const auto batch = peerOf(self)->transport->receiveBatch();
        if (!batch) return 1;
        if (net::assignToGameString(reinterpret_cast<void*>(out), *batch)) return 0;
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 4) log().warn(L"Bot: cannot hand a {}-byte batch to the game's connection", batch->size());
    } catch (...) {}
    return 1;
}
void* const kPeerTable[] = {
    reinterpret_cast<void*>(&peerDestroy), reinterpret_cast<void*>(&peerSend), reinterpret_cast<void*>(&peerStatus),
    reinterpret_cast<void*>(&peerNothing), reinterpret_cast<void*>(&peerNothing), reinterpret_cast<void*>(&peerNothing),
    reinterpret_cast<void*>(&peerEncrypted), reinterpret_cast<void*>(&peerNothing), reinterpret_cast<void*>(&peerReceive),
};
u64 __fastcall controlNothing(u64, u64, u64, u64) { return 0; }
void* const kControlTable[] = {reinterpret_cast<void*>(&controlNothing), reinterpret_cast<void*>(&controlNothing), reinterpret_cast<void*>(&controlNothing)};
}

bool initialize()
{
    g_ready = false;
    std::int32_t probe = 0;
    g.statsFn = relTarget(Site::NetworkConnectionCreateSite, 0x1);
    g.ctor = relTarget(Site::NetworkConnectionCreateSite, 0xfe);
    g.dtor = relTarget(Site::NetworkConnectionDestroySite, 13);
    g.enableEncryption = net::site(Site::EnableEncryption);
    std::byte create[networklayout::kCreateBytes]{}, options[networklayout::kOptionsBytes]{}, destroy[networklayout::kDestroyBytes]{};
    const auto* const createSite = net::site(Site::NetworkConnectionCreateSite);
    const auto* const optionsSite = net::site(Site::NetworkConnectionOptionsSite);
    const auto* const destroySite = net::site(Site::NetworkConnectionDestroySite);
    const auto fields = createSite && optionsSite && destroySite && memory::copyGuarded(createSite, create, sizeof create)
        && memory::copyGuarded(optionsSite, options, sizeof options) && memory::copyGuarded(destroySite, destroy, sizeof destroy)
        ? networklayout::readConnection(create, options, destroy) : std::nullopt;
    if (fields) {
        g.size = fields->size; g.nsArg5Ptr = fields->arg5Pointer; g.nsArg5Ctrl = fields->arg5Control;
        g.nsArg5Extra = fields->arg5Extra; g.nsArg6 = fields->arg6; g.nsArg7 = fields->arg7;
        g.options = fields->options; g.optionsText = fields->optionsText; g.optionsTag = fields->optionsTag;
    }
    DWORD64 imageBase = 0;
    const auto* const entry = optionsSite ? RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(optionsSite), &imageBase, nullptr) : nullptr;
    std::uint8_t flushSlot = 0, functionSlot = 0, updateSlot = 0;
    bool ok = fields && entry && reinterpret_cast<const void*>(imageBase + entry->BeginAddress) == g.ctor
        && g.statsFn && g.ctor && g.dtor && g.enableEncryption
        && imm(Site::PeerUpdateSite, 0x6, flushSlot) && imm(Site::PeerUpdateSite, 0x2b, functionSlot) && imm(Site::PeerUpdateSite, 0x41, updateSlot)
        && imm(Site::CompressionPeerSite, 0x13, g.compressedPtr)
        && imm(Site::CompressionFieldsSite, 0x9, g.compEnabled) && imm(Site::CompressionFieldsSite, 0xd, g.compThreshold)
        && imm(Site::CompressionFieldsSite, 0x12, g.compZero16a)
        && imm(Site::CompressionFieldsSite, 0x18, g.compZero32a) && imm(Site::CompressionFieldsSite, 0x21, g.compZero16b)
        && imm(Site::CompressionFieldsSite, 0x27, g.compZero32b)
        && imm(Site::RequestNetworkSettingsSite, 14, probe);
    ok = ok && g.compThreshold <= 0x100 - 2 * sizeof(std::uint16_t);
    g.compAlgorithm = static_cast<std::uint8_t>(g.compThreshold + sizeof(std::uint16_t));
    g.updateSlot = updateSlot;
    g.peerOffset = net::connectionPeerOffset(); g.sendSlot = net::peerSendSlot();
    if (ok) {
        std::uint8_t head[0x40]{};
        ok = memory::copyGuarded(g.ctor, head, sizeof head);
        for (std::size_t i = 0; ok && i + 7 <= sizeof head; ++i)
            if (head[i] == 0x41 && head[i + 1] == 0xB8 && head[i + 6] == 0xE8) { std::memcpy(&g.idSize, head + i + 2, 4); break; }
    }
    if (probe > 0 && probe < 0x10000) g_protocol = probe;
    ok = ok && g.size >= 0x100 && g.size < 0x1000
        && g.idSize >= 0x40 && g.idSize < g.size && g.peerOffset && g.peerOffset + 8 <= g.size && g.compressedPtr + 8 <= g.size
        && g.sendSlot && g.updateSlot && g.updateSlot % 8 == 0 && flushSlot % 8 == 0 && flushSlot != g.updateSlot
        && g.nsArg6 < 0x1000 && g.nsArg7 < 0x1000 && g.nsArg5Ptr < 0x1000 && g.nsArg5Extra < 0x1000
        && g.compEnabled < 0x100 && g.compThreshold < 0x100 && g.compAlgorithm < 0x100 && g_protocol > 0;
    if (!ok) { log().warn(L"Bot: cannot read the game's connection functions - bots on extra connections are unavailable"); return false; }
    log().info(L"Bot: NetworkConnection size {:#x} id {:#x}; ns args +{:#x}/+{:#x}/+{:#x} +{:#x} +{:#x}; options +{:#x}; peer +{:#x} send {:#x} update {:#x}; compression +{:#x} ({:#x} {:#x} {:#x}); protocol {}",
        g.size, g.idSize, g.nsArg5Ptr, g.nsArg5Ctrl, g.nsArg5Extra, g.nsArg6, g.nsArg7, g.options, g.peerOffset, g.sendSlot, g.updateSlot,
        g.compressedPtr, g.compEnabled, g.compThreshold, g.compAlgorithm, g_protocol);
    g_ready = true;
    return true;
}
bool available() { return g_ready; }
std::int32_t protocolVersion() { return g_protocol; }

Connection::Connection() { received_[0x18] = 15; }
Connection::~Connection() { close(); }

bool Connection::open(Transport& transport, std::string& error)
{
    if (!g_ready) { error = "the game's connection functions were not found"; return false; }
    if (connection_) { error = "already open"; return false; }
    if (!net::receiveOriginal()) { error = "no receive hook"; return false; }
    auto* const ns = static_cast<std::uint8_t*>(net::networkSystem());
    auto* const main = static_cast<std::uint8_t*>(net::mainConnection());
    if (!ns || !main) { error = "join a world with the main player first"; return false; }
    std::vector<std::uint8_t> id(g.idSize);
    struct Arg5 { u64 pointer, control, extra; } arg5{};
    u64 arg6 = 0;
    struct Options { std::uint32_t kind = 0, padding = 0; alignas(std::string) std::byte text[sizeof(std::string)]{}; std::uint8_t present = 0; } options;
    std::string optionsText;
    static_assert(offsetof(Options, text) == 8 && offsetof(Options, present) == 0x28);
    if (!memory::copyGuarded(main, id.data(), id.size()) || !readAt(ns + g.nsArg5Ptr, arg5.pointer) || !readAt(ns + g.nsArg5Ctrl, arg5.control)
        || !readAt(ns + g.nsArg5Extra, arg5.extra) || !readAt(ns + g.nsArg6, arg6)
        || !readAt(main + g.optionsTag, options.present) || options.present > 1
        || (options.present && (!readAt(main + g.options, options.kind) || !copyGameString(main + g.optionsText, optionsText)))) {
        error = "cannot read the game's network system"; return false;
    }
    peer_ = std::make_unique<Peer>();
    peer_->vtable = kPeerTable; peer_->transport = &transport;
    peer_->control = {kControlTable, 1, 1};
    void* memory = _aligned_malloc(g.size, 16);
    if (!memory) { error = "out of memory"; peer_.reset(); return false; }
    std::memset(memory, 0, g.size);
    options.text[0x18] = std::byte{15};
    if (options.present && !gamestring::assign(options.text, optionsText)) {
        error = "copying the connection options failed"; peer_.reset(); _aligned_free(memory); return false;
    }
    struct Shared { u64 pointer = 0, control = 0; } stats, peerShared{reinterpret_cast<u64>(peer_.get()), reinterpret_cast<u64>(&peer_->control)};
    if (!call4(g.statsFn, reinterpret_cast<u64>(ns), reinterpret_cast<u64>(&stats), reinterpret_cast<u64>(id.data()), 0)) {
        gamestring::destroy(options.text);
        error = "creating the connection statistics faulted (" + std::to_string(g_lastCode.load()) + ")"; peer_.reset(); _aligned_free(memory); return false;
    }
    addRef(reinterpret_cast<void*>(arg5.control));
    InterlockedIncrement(&peer_->control.uses);
    if (!call9(g.ctor, reinterpret_cast<u64>(memory), reinterpret_cast<u64>(id.data()), reinterpret_cast<u64>(&peerShared), 0,
               reinterpret_cast<u64>(&arg5), arg6, reinterpret_cast<u64>(ns + g.nsArg7), reinterpret_cast<u64>(&stats), reinterpret_cast<u64>(&options), nullptr)) {
        error = "constructing the connection faulted (" + std::to_string(g_lastCode.load()) + ")";
        (void)peer_.release();
        return false;
    }
    connection_ = memory;
    auto* const conn = static_cast<std::uint8_t*>(memory);
    void* inner = nullptr;
    void* innerInner = nullptr;
    if (!readAt(conn + g.peerOffset, outer_) || !memory::plausiblePointer(outer_) || !readAt(conn + g.compressedPtr, compressed_)
        || !memory::plausiblePointer(compressed_) || !readAt(static_cast<std::uint8_t*>(compressed_) + 8, inner) || !memory::plausiblePointer(inner)) {
        error = "the game's connection has no layers"; close(); return false;
    }
    if (slotOf(outer_, net::peerReceiveSlot()) != net::receiveTarget()) {
        error = "unexpected receive layer in the game's connection"; close(); return false;
    }
    if (inner == peer_.get()) encrypted_ = nullptr;
    else if (readAt(static_cast<std::uint8_t*>(inner) + 8, innerInner) && innerInner == peer_.get()) encrypted_ = inner;
    else { error = "unexpected layers in the game's connection"; close(); return false; }
    log().info(L"Bot: extra connection built with the game's NetworkConnection (encryption layer {})", encrypted_ != nullptr);
    return true;
}

bool Connection::send(const std::string& packet)
{
    if (!outer_) return false;
    const void* fn = slotOf(outer_, g.sendSlot);
    return fn && call4(fn, reinterpret_cast<u64>(outer_), reinterpret_cast<u64>(&packet), 1, 0);
}

void Connection::flush()
{
    if (!outer_) return;
    if (const void* fn = slotOf(outer_, g.updateSlot)) call4(fn, reinterpret_cast<u64>(outer_), 0, 0, 0);
}

std::optional<std::string> Connection::receive()
{
    const auto original = net::receiveOriginal();
    if (!connection_ || !original) return std::nullopt;
    struct { u64 pointer = 0, control = 0; } timepoint;
    u64 result = 1;
    if (!call4(reinterpret_cast<const void*>(original), reinterpret_cast<u64>(outer_), reinterpret_cast<u64>(received_),
               reinterpret_cast<u64>(&timepoint), 0, &result) || static_cast<int>(result) != 0)
        return std::nullopt;
    std::string packet;
    if (!copyGameString(received_, packet) || packet.empty()) return std::nullopt;
    return packet;
}

bool Connection::enableCompression(std::uint16_t threshold, std::uint16_t algorithm)
{
    if (!compressed_) return false;
    auto* const p = static_cast<std::uint8_t*>(compressed_);
    __try {
        p[g.compEnabled] = algorithm != 0xffff ? 1 : 0;
        *reinterpret_cast<std::uint16_t*>(p + g.compThreshold) = threshold;
        *reinterpret_cast<std::uint16_t*>(p + g.compAlgorithm) = algorithm;
        *reinterpret_cast<std::uint16_t*>(p + g.compZero16a) = 0;
        *reinterpret_cast<std::uint32_t*>(p + g.compZero32a) = 0;
        *reinterpret_cast<std::uint16_t*>(p + g.compZero16b) = 0;
        *reinterpret_cast<std::uint32_t*>(p + g.compZero32b) = 0;
        return true;
    } __except (memoryException(GetExceptionCode())) { return false; }
}

bool Connection::enableEncryption(const std::string& key)
{
    if (!encrypted_) return false;
    return call4(g.enableEncryption, reinterpret_cast<u64>(encrypted_), reinterpret_cast<u64>(&key), 0, 0);
}

void Connection::close()
{
    if (connection_) {
        if (!call4(g.dtor, reinterpret_cast<u64>(connection_), 0, 0, 0))
            log().warn(L"Bot: destroying the extra connection faulted ({})", g_lastCode.load());
        _aligned_free(connection_);
        connection_ = nullptr;
    }
    outer_ = compressed_ = encrypted_ = nullptr;
    if (!gamestring::destroy(received_)) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) log().warn(L"Bot: could not release the last received game buffer");
    }
    peer_.reset();
}

}
