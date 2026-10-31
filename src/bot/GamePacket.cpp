#include "GamePacket.h"
#include "BotNet.h"
#include "GameString.h"
#include "core/Logger.h"
#include "memory/Memory.h"
#include <Windows.h>
#include <atomic>
#include <cstring>
#include <intrin.h>

namespace tsukuyomi::bot::gamepacket {
namespace {
using CreateFn = void*(__fastcall*)(void* out, int id);
using WriteFn = void(__fastcall*)(void* packet, void* stream, void* context, std::uint64_t mode);
using ReadFn = void*(__fastcall*)(void* packet, void* result, void* stream, void* context);
using ModeFn = void*(__fastcall*)(void* table, std::uint64_t* out, std::uint32_t id);
using SetModeFn = void(__fastcall*)(void* packet, std::uint32_t mode);
using ReleaseFn = void(__fastcall*)(void* control);
constexpr std::size_t kGetIdSlot = 1, kWriteSlot = 5, kReadSlot = 9, kSetModeSlot = 13;
std::uint32_t g_contextOffset = 0, g_modesOffset = 0;
std::uint32_t g_maxId = 0;
void* g_create = nullptr;
void* g_streamVtable = nullptr;
std::atomic<bool> g_ready{false};

int memoryException(unsigned code)
{
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}
void emptyString(std::byte* at)
{
    std::memset(at, 0, 0x20);
    const std::size_t capacity = 15;
    std::memcpy(at + 0x18, &capacity, sizeof capacity);
}
struct alignas(16) Stream { std::byte raw[0x50]; };
void initStream(Stream& stream, const void* data, std::size_t size, void* buffer)
{
    std::memset(stream.raw, 0, sizeof stream.raw);
    std::memcpy(stream.raw, &g_streamVtable, sizeof(void*));
    emptyString(stream.raw + 0x08);
    std::memcpy(stream.raw + 0x28, &data, sizeof(void*));
    std::memcpy(stream.raw + 0x30, &size, sizeof size);
    std::memcpy(stream.raw + 0x48, &buffer, sizeof(void*));
}
bool createGuarded(void* out, int id)
{
    __try { reinterpret_cast<CreateFn>(g_create)(out, id); return true; }
    __except (memoryException(GetExceptionCode())) { return false; }
}
bool getIdGuarded(void* packet, std::uint32_t* id)
{
    __try {
        auto* const vtable = *static_cast<void***>(packet);
        *id = reinterpret_cast<std::uint32_t(__fastcall*)(void*)>(vtable[kGetIdSlot])(packet);
        return true;
    } __except (memoryException(GetExceptionCode())) { return false; }
}
bool contextGuarded(std::uint32_t id, void** context, std::uint64_t* mode)
{
    __try {
        auto* const system = static_cast<std::byte*>(net::networkSystem());
        if (!system) return false;
        *context = *reinterpret_cast<void**>(system + g_contextOffset);
        void* const table = *reinterpret_cast<void**>(system + g_modesOffset);
        if (!*context || !table) return false;
        *mode = 0;
        auto* const vtable = *static_cast<void***>(table);
        reinterpret_cast<ModeFn>(vtable[1])(table, mode, id);
        *mode &= 0x1ffffffffull;
        return true;
    } __except (memoryException(GetExceptionCode())) { return false; }
}
bool writeGuarded(void* packet, void* stream, void* context, std::uint64_t mode)
{
    __try {
        auto* const vtable = *static_cast<void***>(packet);
        reinterpret_cast<WriteFn>(vtable[kWriteSlot])(packet, stream, context, mode);
        return true;
    } __except (memoryException(GetExceptionCode())) { return false; }
}
bool readGuarded(void* packet, void* result, void* stream, void* context, std::uint64_t mode)
{
    __try {
        auto* const vtable = *static_cast<void***>(packet);
        if (mode >> 32) reinterpret_cast<SetModeFn>(vtable[kSetModeSlot])(packet, static_cast<std::uint32_t>(mode));
        reinterpret_cast<ReadFn>(vtable[kReadSlot])(packet, result, stream, context);
        return true;
    } __except (memoryException(GetExceptionCode())) { return false; }
}
void releaseGuarded(void* control)
{
    __try {
        auto* const base = static_cast<std::byte*>(control);
        if (_InterlockedDecrement(reinterpret_cast<volatile long*>(base + 8)) != 0) return;
        auto* const vtable = *static_cast<void***>(control);
        reinterpret_cast<ReleaseFn>(vtable[0])(control);
        if (_InterlockedDecrement(reinterpret_cast<volatile long*>(base + 0xc)) == 0) reinterpret_cast<ReleaseFn>(vtable[1])(control);
    } __except (memoryException(GetExceptionCode())) {}
}
struct GameBuffer {
    std::string* string = static_cast<std::string*>(::operator new(sizeof(std::string)));
    GameBuffer() { emptyString(reinterpret_cast<std::byte*>(string)); }
    ~GameBuffer() { gamestring::destroy(string); ::operator delete(string); }
};
}

Packet& Packet::operator=(Packet&& other) noexcept
{
    if (this != &other) {
        if (control_) releaseGuarded(control_);
        object_ = std::exchange(other.object_, nullptr);
        control_ = std::exchange(other.control_, nullptr);
        id_ = other.id_;
    }
    return *this;
}
Packet::~Packet() { if (control_) releaseGuarded(control_); }

bool initialize()
{
    auto* const create = net::site(Site::CreatePacket);
    auto* const stream = net::site(Site::BinaryStreamVtableSite);
    auto* const send = net::site(Site::NetworkSystemSendSite);
    std::uint32_t maxId = 0;
    std::int32_t rel = 0;
    if (!create || !stream || !send || !memory::copyGuarded(create + 10, &maxId, sizeof maxId) || !memory::copyGuarded(stream + 3, &rel, sizeof rel)
        || !memory::copyGuarded(send + 3, &g_modesOffset, sizeof g_modesOffset) || !memory::copyGuarded(send + 0x4f, &g_contextOffset, sizeof g_contextOffset)) {
        log().warn(L"Bot: the game's packet functions are unavailable");
        return false;
    }
    g_create = create; g_maxId = maxId;
    g_streamVtable = stream + 7 + rel;
    Packet probe = gamepacket::create(0x1f);
    std::uint32_t id = 0;
    if (!probe || !getIdGuarded(probe.object(), &id) || id != 0x1f) {
        log().error(L"Bot: the game's packet factory did not give the expected packet (id {:#x})", id);
        g_create = nullptr;
        return false;
    }
    g_ready.store(true, std::memory_order_release);
    log().info(L"Bot: the game's packet functions are ready (ids up to {:#x}, context +{:#x}, modes +{:#x})", g_maxId, g_contextOffset, g_modesOffset);
    return true;
}
bool available() { return g_ready.load(std::memory_order_acquire); }

Packet create(std::uint16_t id)
{
    Packet packet;
    if (!g_create || id > g_maxId) return packet;
    void* out[2] = {nullptr, nullptr};
    if (!createGuarded(out, id) || !out[0] || !out[1]) return packet;
    packet.object_ = out[0]; packet.control_ = out[1]; packet.id_ = id;
    return packet;
}

std::optional<std::string> write(const Packet& packet)
{
    void* context = nullptr;
    std::uint64_t mode = 0;
    if (!packet || !g_streamVtable || !contextGuarded(packet.id(), &context, &mode)) return std::nullopt;
    GameBuffer buffer;
    Stream stream;
    initStream(stream, buffer.string, 0, buffer.string);
    if (!writeGuarded(packet.object(), &stream, context, mode)) return std::nullopt;
    auto bytes = gamestring::read(buffer.string);
    return bytes;
}

bool read(Packet& packet, protocol::Bytes body, std::size_t* consumed)
{
    void* context = nullptr;
    std::uint64_t mode = 0;
    if (!packet || !g_streamVtable || !contextGuarded(packet.id(), &context, &mode)) return false;
    GameBuffer buffer;
    Stream stream;
    initStream(stream, body.data(), body.size(), buffer.string);
    alignas(16) std::byte result[0x100] = {};
    if (!readGuarded(packet.object(), result, &stream, context, mode)) return false;
    if (consumed) std::memcpy(consumed, stream.raw + 0x38, sizeof(std::size_t));
    return result[0x40] == std::byte{1};
}

}
