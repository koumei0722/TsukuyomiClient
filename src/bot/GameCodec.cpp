#include "GameCodec.h"
#include "BotNet.h"
#include "GamePacket.h"
#include "GamePath.h"
#include "GameString.h"
#include "core/Logger.h"
#include "core/Notice.h"
#include "game/BlockWrite.h"
#include "memory/Memory.h"
#include <Windows.h>
#include <cstddef>
#include <cmath>
#include <cstring>
#include <limits>
#include <format>
#include <unordered_map>
#include <vector>

namespace tsukuyomi::bot::gamecodec {
namespace {
using gamepacket::Packet;

constexpr std::size_t kDescriptorSize = 0x60, kDescImpl = 0x08, kDescCount = 0x10, kDescHasNetId = 0x18, kDescNetId = 0x20, kDescNetIdIndex = 0x30;

namespace tx {
constexpr std::size_t kAction = 0x68;
constexpr std::size_t kUseTrigger = 0x6c, kUseX = 0x70, kUseY = 0x74, kUseZ = 0x78, kUseBlock = 0x7c, kUseFace = 0x80, kUseSlot = 0x84, kUseHand = 0x88,
                      kUseItem = 0x90, kUsePosition = 0xf0, kUseClicked = 0xfc, kUsePrediction = 0x108, kUseCooldown = 0x109;
constexpr std::size_t kReleaseSlot = 0x6c, kReleaseItem = 0x70, kReleaseHead = 0xd0;
}

using DestroyFn = void*(__fastcall*)(void* self, unsigned flags);
using CopyFn = void*(__fastcall*)(void* self, const void* source);
using StackDescriptorFn = void*(__fastcall*)(void* self, const void* stack);
using FromTypeFn = void*(__fastcall*)(void** out, int type, const void* transaction);
using PayloadCtorFn = void*(__fastcall*)(void* self, void** transaction, bool clientSide);
using PayloadDtorFn = void(__fastcall*)(void* self);
void* g_copyDescriptor = nullptr;
void* g_stackDescriptor = nullptr;
void* g_fromType = nullptr;
void* g_payloadCtor = nullptr;
void* g_payloadDtor = nullptr;
std::uint32_t g_variantOffset = 0;
bool g_ready = false, g_transactions = false, g_inventoryView = false;
std::uint32_t g_blockNetworkIdOffset = 0;
bool g_levelChunkBlobs = false, g_levelChunkLimit = false;
constexpr std::size_t kChunkLimit = 0x40, kChunkHasLimit = 0x44, kChunkBlobs = 0x50;
int memoryException(unsigned code);
constexpr std::size_t kGetBlockSlot = 2;
using GetBlockFn = const void*(__fastcall*)(void* region, const std::int32_t* position);
bool blockIdGuarded(void* region, const std::int32_t* position, std::uint32_t* id)
{
    __try {
        auto* const vtable = *static_cast<void***>(region);
        const auto* block = static_cast<const std::byte*>(reinterpret_cast<GetBlockFn>(vtable[kGetBlockSlot])(region, position));
        if (!block) return false;
        *id = *reinterpret_cast<const std::uint32_t*>(block + g_blockNetworkIdOffset);
        return true;
    } __except (memoryException(GetExceptionCode())) { return false; }
}

int memoryException(unsigned code)
{
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}
void* functionStart(std::byte* site)
{
    if (!site) return nullptr;
    DWORD64 imageBase = 0;
    const auto* entry = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(site), &imageBase, nullptr);
    return entry ? reinterpret_cast<void*>(imageBase + entry->BeginAddress) : nullptr;
}

template<class T> bool put(std::byte* at, T value) { return memory::writeGuarded(at, &value, sizeof value); }
template<class T> std::optional<T> get(const std::byte* at)
{
    T value{};
    if (!memory::copyGuarded(at, &value, sizeof value)) return std::nullopt;
    return value;
}
bool putVec(std::byte* at, protocol::Vec3 value) { return put(at, value.x) && put(at + 4, value.y) && put(at + 8, value.z); }
std::optional<protocol::Vec3> getVec(const std::byte* at)
{
    const auto x = get<float>(at), y = get<float>(at + 4), z = get<float>(at + 8);
    if (!x || !y || !z) return std::nullopt;
    return protocol::Vec3{*x, *y, *z};
}
std::optional<std::string> getString(const std::byte* at) { return gamestring::read(at); }

std::string encode(const Packet& packet, std::uint8_t sub)
{
    const auto body = gamepacket::write(packet);
    if (!body) return {};
    std::string out;
    protocol::writeVarUInt32(out, protocol::header(packet.id(), 0, sub));
    out += *body;
    return out;
}
Packet parse(std::uint16_t id, protocol::Bytes body)
{
    Packet packet = gamepacket::create(id);
    if (!packet || !gamepacket::read(packet, body)) return {};
    return packet;
}

struct DescriptorRef { std::shared_ptr<Packet> owner; std::byte* at = nullptr; };
Item itemFrom(const std::shared_ptr<Packet>& owner, std::byte* descriptor)
{
    Item item;
    const auto impl = get<std::uintptr_t>(descriptor + kDescImpl);
    const auto count = get<std::uint16_t>(descriptor + kDescCount);
    if (!impl || !count) return item;
    item.count = *count;
    item.id = *impl && *count ? 1 : 0;
    if (get<std::uint8_t>(descriptor + kDescHasNetId).value_or(0) && get<std::uint8_t>(descriptor + kDescNetIdIndex).value_or(0xff) == 0)
        item.netId = get<std::int32_t>(descriptor + kDescNetId).value_or(0);
    item.handle = std::make_shared<DescriptorRef>(DescriptorRef{owner, descriptor});
    return item;
}
bool destroyDescriptorGuarded(void* descriptor)
{
    __try {
        auto* const vtable = *static_cast<void***>(descriptor);
        reinterpret_cast<DestroyFn>(vtable[0])(descriptor, 0);
        return true;
    } __except (memoryException(GetExceptionCode())) { return false; }
}
bool copyDescriptorGuarded(void* target, const void* source)
{
    __try { reinterpret_cast<CopyFn>(g_copyDescriptor)(target, source); return true; }
    __except (memoryException(GetExceptionCode())) { return false; }
}
bool stackDescriptorGuarded(void* target, const void* stack)
{
    __try { reinterpret_cast<StackDescriptorFn>(g_stackDescriptor)(target, stack); return true; }
    __except (memoryException(GetExceptionCode())) { return false; }
}
bool putItem(std::byte* target, const Item& item)
{
    if (item.empty() || !item.handle) return true;
    const auto ref = std::static_pointer_cast<const DescriptorRef>(item.handle);
    if (!ref->at || !destroyDescriptorGuarded(target)) return false;
    if (!copyDescriptorGuarded(target, ref->at)) return false;
    put<std::uint16_t>(target + kDescCount, item.count);
    if (item.netId && get<std::uint8_t>(target + kDescNetIdIndex).value_or(0xff) == 0) {
        put<std::uint8_t>(target + kDescHasNetId, 1);
        put<std::int32_t>(target + kDescNetId, item.netId);
    }
    return true;
}

struct EmptyTransaction {
    std::unordered_map<std::uint64_t, std::uint64_t> actions;
    std::vector<std::uint64_t> contents;
};
static_assert(sizeof(std::unordered_map<std::uint64_t, std::uint64_t>) == 0x40);
static_assert(offsetof(EmptyTransaction, contents) == 0x40 && sizeof(EmptyTransaction) == 0x58);
bool fromTypeGuarded(void** out, int type, const void* empty)
{
    __try { reinterpret_cast<FromTypeFn>(g_fromType)(out, type, empty); return true; }
    __except (memoryException(GetExceptionCode())) { return false; }
}
bool rebuildPayloadGuarded(std::byte* payload, void** transaction)
{
    __try {
        reinterpret_cast<PayloadDtorFn>(g_payloadDtor)(payload);
        reinterpret_cast<PayloadCtorFn>(g_payloadCtor)(payload, transaction, false);
        return true;
    } __except (memoryException(GetExceptionCode())) { return false; }
}
void deleteObjectGuarded(void* object)
{
    __try {
        auto* const vtable = *static_cast<void***>(object);
        reinterpret_cast<DestroyFn>(vtable[0])(object, 1);
    } __except (memoryException(GetExceptionCode())) {}
}
template<class Fill> std::string transaction(std::uint8_t sub, int type, Fill fill)
{
    if (!g_transactions) return {};
    Packet packet = gamepacket::create(0x1e);
    if (!packet) return {};
    EmptyTransaction empty;
    void* made = nullptr;
    if (!fromTypeGuarded(&made, type, &empty) || !made) return {};
    auto* const object = static_cast<std::byte*>(made);
    if (!fill(object)) { deleteObjectGuarded(made); return {}; }
    if (!rebuildPayloadGuarded(packet.object() + 0x30, &made)) {
        packet.abandon();
        return {};
    }
    if (made) deleteObjectGuarded(made);
    return encode(packet, sub);
}

constexpr std::size_t kSubChunkDimension = 0x30, kSubChunkOffsets = 0x38, kSubChunkPosition = 0x50;
bool g_subChunkRequests = false;
bool putByteVector(std::byte* at, const void* values, std::size_t bytes)
{
    if (bytes == 0) return true;
    if (!memory::isWritable(at, 24)) return false;
    auto* const data = static_cast<std::byte*>(gamestring::allocate(bytes));
    if (!data) return false;
    std::memcpy(data, values, bytes);
    std::byte* const fields[3] = {data, data + bytes, data + bytes};
    std::memcpy(at, fields, sizeof fields);
    return true;
}
std::string buildSubChunkRequest(std::uint8_t sub, std::int32_t dimension, std::int32_t x, std::int32_t y, std::int32_t z,
                                 std::span<const std::array<std::int8_t, 3>> offsets)
{
    if (offsets.empty() || offsets.size() > 4096) return {};
    Packet p = gamepacket::create(0xaf);
    if (!p) return {};
    auto* const o = p.object();
    const auto begin = get<std::byte*>(o + kSubChunkOffsets);
    if (!begin || *begin) return {};
    std::vector<std::int8_t> flat;
    flat.reserve(offsets.size() * 3);
    for (const auto& offset : offsets) flat.insert(flat.end(), offset.begin(), offset.end());
    if (!putByteVector(o + kSubChunkOffsets, flat.data(), flat.size()) || !put(o + kSubChunkDimension, dimension)
        || !put(o + kSubChunkPosition, x) || !put(o + kSubChunkPosition + 4, y) || !put(o + kSubChunkPosition + 8, z))
        return {};
    return encode(p, sub);
}
bool putU64Vector(std::byte* at, std::span<const std::uint64_t> values)
{
    if (values.size() > (std::numeric_limits<std::size_t>::max)() / sizeof(std::uint64_t)) return false;
    return putByteVector(at, values.data(), values.size_bytes());
}
std::optional<std::vector<std::byte*>> elements(const std::byte* vectorAt, std::size_t stride, std::size_t limit)
{
    const auto begin = get<std::byte*>(vectorAt), end = get<std::byte*>(vectorAt + 8);
    if (!begin || !end || *end < *begin || static_cast<std::size_t>(*end - *begin) % stride != 0) return std::nullopt;
    const std::size_t count = static_cast<std::size_t>(*end - *begin) / stride;
    if (count > limit) return std::nullopt;
    std::vector<std::byte*> out;
    for (std::size_t i = 0; i < count; ++i) out.push_back(*begin + i * stride);
    return out;
}

class GameCodec final : public BotCodec {
public:
    std::string subClientLogin(std::uint8_t sub, const guest::Login& login) override
    {
        std::string body;
        if (!protocol::writeString(body, login.bytes())) return {};
        Packet p = parse(0x5e, protocol::bytes(body));
        if (!p) return {};
        return encode(p, sub);
    }
    std::string requestChunkRadius(std::uint8_t sub, std::int32_t radius, std::uint8_t maxRadius) override
    {
        Packet p = gamepacket::create(0x45);
        if (!p || !put(p.object() + 0x30, radius) || !put(p.object() + 0x34, maxRadius)) return {};
        return encode(p, sub);
    }
    std::string setLocalPlayerAsInitialized(std::uint8_t sub, std::uint64_t runtimeId) override
    {
        Packet p = gamepacket::create(0x71);
        if (!p || !put(p.object() + 0x30, runtimeId)) return {};
        return encode(p, sub);
    }
    std::string networkStackLatencyReply(std::uint8_t sub, protocol::Bytes request) override
    {
        Packet p = parse(0x73, request);
        if (!p || !put<std::uint8_t>(p.object() + 0x38, 0)) return {};
        return encode(p, sub);
    }
    std::string disconnect(std::uint8_t sub) override
    {
        Packet p = gamepacket::create(0x05);
        if (!p || !put<std::int32_t>(p.object() + 0x30, 0x1e)) return {};
        return encode(p, sub);
    }
    std::string subChunkRequest(std::uint8_t sub, std::int32_t dimension, std::int32_t x, std::int32_t y, std::int32_t z,
                                std::span<const std::array<std::int8_t, 3>> offsets) override
    {
        if (!g_subChunkRequests) return {};
        return buildSubChunkRequest(sub, dimension, x, y, z, offsets);
    }
    std::string clientCacheBlobHits(std::uint8_t sub, std::span<const std::uint64_t> hits) override
    {
        if (hits.empty() || hits.size() > protocol::kMaxBlobStatusHashes) return {};
        Packet p = gamepacket::create(0x87);
        if (!p || !putU64Vector(p.object() + 0x48, hits)) return {};
        return encode(p, sub);
    }
    std::string playerAuthInput(std::uint8_t sub, const protocol::AuthInput& input) override
    {
        Packet p = gamepacket::create(0x90);
        if (!p) return {};
        auto* const o = p.object();
        constexpr float kRadians = 3.14159265358979f / 180.0f;
        const float pitch = input.pitch * kRadians, yaw = input.yaw * kRadians;
        std::uint64_t flags = std::uint64_t{1} << 48;
        if (input.handledTeleport) flags |= std::uint64_t{1} << 37;
        if (input.verticalCollision) flags |= std::uint64_t{1} << 50;
        for (const auto flag : input.extraFlags) if (flag >= 0 && flag < 64) flags |= std::uint64_t{1} << flag;
        const bool ok = put(o + 0x30, input.pitch) && put(o + 0x34, input.yaw) && putVec(o + 0x38, input.position)
            && put(o + 0x44, input.yaw)
            && putVec(o + 0x48, input.delta)
            && put(o + 0x64, input.moveX) && put(o + 0x68, input.moveZ)
            && put(o + 0x6c, input.pitch) && put(o + 0x70, input.yaw)
            && putVec(o + 0x74, {-std::sin(yaw) * std::cos(pitch), -std::sin(pitch), std::cos(yaw) * std::cos(pitch)})
            && put(o + 0x80, input.moveX) && put(o + 0x84, input.moveZ)
            && put(o + 0x88, flags)
            && put<std::uint32_t>(o + 0x98, 1) && put<std::uint32_t>(o + 0x9c, 2) && put<std::int32_t>(o + 0xa0, 0)
            && put(o + 0xa8, input.tick);
        return ok ? encode(p, sub) : std::string();
    }
    std::string respawn(std::uint8_t sub, protocol::Vec3 position, std::uint8_t state, std::uint64_t runtimeId) override
    {
        Packet p = gamepacket::create(0x2d);
        if (!p || !putVec(p.object() + 0x30, position) || !put(p.object() + 0x3c, state) || !put(p.object() + 0x40, runtimeId)) return {};
        return encode(p, sub);
    }
    std::string playerAction(std::uint8_t sub, std::uint64_t runtimeId, std::int32_t action) override
    {
        Packet p = gamepacket::create(0x24);
        if (!p || !put<std::int32_t>(p.object() + 0x48, -1) || !put(p.object() + 0x4c, action) || !put(p.object() + 0x50, runtimeId)) return {};
        return encode(p, sub);
    }
    std::string loadingScreen(std::uint8_t sub, std::int32_t type, std::optional<std::uint32_t> screenId) override
    {
        Packet p = gamepacket::create(0x138);
        if (!p || !put(p.object() + 0x30, type)) return {};
        if (screenId && (!put(p.object() + 0x34, *screenId) || !put<std::uint8_t>(p.object() + 0x38, 1))) return {};
        return encode(p, sub);
    }
    std::string animateSwing(std::uint8_t sub, std::uint64_t runtimeId) override
    {
        Packet p = gamepacket::create(0x2c);
        if (!p || !put(p.object() + 0x30, runtimeId) || !put<std::uint8_t>(p.object() + 0x38, 1) || !put(p.object() + 0x3c, 0.0f)) return {};
        return encode(p, sub);
    }
    std::string useItem(std::uint8_t sub, const UseItem& v) override
    {
        return transaction(sub, 2, [&](std::byte* t) {
            return put(t + tx::kAction, v.action) && put(t + tx::kUseTrigger, static_cast<std::uint8_t>(v.trigger))
                && put(t + tx::kUseX, v.x) && put(t + tx::kUseY, v.y) && put(t + tx::kUseZ, v.z) && put(t + tx::kUseBlock, v.blockRuntimeId)
                && put(t + tx::kUseFace, static_cast<std::uint8_t>(v.face)) && put(t + tx::kUseSlot, v.hotbarSlot) && put<std::uint8_t>(t + tx::kUseHand, 0)
                && putItem(t + tx::kUseItem, v.held) && putVec(t + tx::kUsePosition, v.position) && putVec(t + tx::kUseClicked, v.clicked)
                && put(t + tx::kUsePrediction, v.prediction) && put<std::uint8_t>(t + tx::kUseCooldown, 0);
        });
    }
    std::string releaseItem(std::uint8_t sub, const ReleaseItem& v) override
    {
        return transaction(sub, 4, [&](std::byte* t) {
            return put(t + tx::kAction, v.action) && put(t + tx::kReleaseSlot, v.hotbarSlot) && putItem(t + tx::kReleaseItem, v.held)
                && putVec(t + tx::kReleaseHead, v.headPosition);
        });
    }
    std::string mobEquipment(std::uint8_t sub, std::uint64_t runtimeId, const Item& item, std::uint8_t slot) override
    {
        Packet p = gamepacket::create(0x1f);
        if (!p) return {};
        auto* o = p.object();
        if (!put(o + 0x30, runtimeId) || !put<std::int32_t>(o + 0x98, slot)
            || !put<std::int32_t>(o + 0x9c, slot) || !put<std::uint8_t>(o + 0xa0, 0)) return {};
        if (!putItem(o + 0x38, item)) { p.abandon(); return {}; }
        return encode(p, sub);
    }
    std::string interact(std::uint8_t sub, std::uint8_t action, std::uint64_t runtimeId) override
    {
        Packet p = gamepacket::create(0x21);
        if (!p || !put(p.object() + 0x30, action) || !put(p.object() + 0x38, runtimeId)) return {};
        return encode(p, sub);
    }
    std::string containerClose(std::uint8_t sub, std::uint8_t window, std::uint8_t type, bool serverInitiated) override
    {
        Packet p = gamepacket::create(0x2f);
        if (!p || !put(p.object() + 0x30, window) || !put(p.object() + 0x31, type)
            || !put<std::uint8_t>(p.object() + 0x32, serverInitiated ? 1 : 0)) return {};
        return encode(p, sub);
    }
    std::optional<std::vector<StackResponse>> readItemStackResponse(protocol::Bytes body) override
    {
        Packet p = parse(0x94, body);
        const auto responses = p ? elements(p.object() + 0x30, 0x30, 4096) : std::nullopt;
        if (!responses) return std::nullopt;
        std::vector<StackResponse> out;
        for (auto* r : *responses) {
            const auto status = get<std::uint8_t>(r);
            const auto id = get<std::int32_t>(r + 0x10);
            const auto containers = elements(r + 0x18, 0x28, 4096);
            if (!status || !id || !containers) return std::nullopt;
            StackResponse response{*status, *id, {}};
            for (auto* c : *containers) {
                const auto container = get<std::uint8_t>(c);
                const auto slots = elements(c + 0x10, 0x58, 4096);
                if (!container || !slots) return std::nullopt;
                StackContainer content{*container, {}};
                for (auto* at : *slots) {
                    const auto slot = get<std::uint8_t>(at), count = get<std::uint8_t>(at + 2);
                    const auto netId = get<std::int32_t>(at + 4);
                    if (!slot || !count || !netId) return std::nullopt;
                    content.slots.push_back({*slot, *count, *netId});
                }
                response.containers.push_back(std::move(content));
            }
            out.push_back(std::move(response));
        }
        return out;
    }
    std::optional<std::int32_t> readPlayStatus(protocol::Bytes body) override
    {
        Packet p = parse(0x02, body);
        return p ? get<std::int32_t>(p.object() + 0x30) : std::nullopt;
    }
    std::optional<protocol::StartGame> readStartGame(protocol::Bytes body) override
    {
        Packet p = parse(0x0b, body);
        if (!p) return std::nullopt;
        const auto* o = p.object();
        const auto unique = get<std::int64_t>(o + 0x570);
        const auto runtime = get<std::uint64_t>(o + 0x578);
        if (!unique || !runtime) return std::nullopt;
        protocol::StartGame result{*unique, *runtime, std::nullopt};
        const auto position = getVec(o + 0x584);
        const auto pitch = get<float>(o + 0x590), yaw = get<float>(o + 0x594);
        if (position && pitch && yaw) result.spawn = protocol::Spawn{*position, *pitch, *yaw};
        return result;
    }
    std::optional<std::int32_t> readChunkRadiusUpdated(protocol::Bytes body) override
    {
        Packet p = parse(0x46, body);
        return p ? get<std::int32_t>(p.object() + 0x30) : std::nullopt;
    }
    std::optional<bool> readNetworkStackLatencyNeedsResponse(protocol::Bytes body) override
    {
        Packet p = parse(0x73, body);
        if (!p) return std::nullopt;
        const auto value = get<std::uint8_t>(p.object() + 0x38);
        if (!value || *value > 1) return std::nullopt;
        return *value != 0;
    }
    std::optional<protocol::Disconnect> readDisconnect(protocol::Bytes body) override
    {
        Packet p = parse(0x05, body);
        if (!p) return std::nullopt;
        const auto reason = get<std::int32_t>(p.object() + 0x30);
        if (!reason) return std::nullopt;
        auto message = getString(p.object() + 0x38).value_or(std::string());
        const bool skip = get<std::uint8_t>(p.object() + 0x78).value_or(0) != 0;
        return protocol::Disconnect{*reason, skip, skip ? std::string() : std::move(message)};
    }
    std::optional<protocol::MovePlayer> readMovePlayer(protocol::Bytes body) override
    {
        Packet p = parse(0x13, body);
        if (!p) return std::nullopt;
        const auto* o = p.object();
        const auto runtime = get<std::uint64_t>(o + 0x30);
        const auto position = getVec(o + 0x38);
        const auto pitch = get<float>(o + 0x44), yaw = get<float>(o + 0x48);
        const auto mode = get<std::uint8_t>(o + 0x50), onGround = get<std::uint8_t>(o + 0x51);
        if (!runtime || !position || !pitch || !yaw || !mode || !onGround) return std::nullopt;
        return protocol::MovePlayer{*runtime, *position, *pitch, *yaw, *mode, *onGround != 0};
    }
    std::optional<protocol::Correction> readCorrectPlayerMovePrediction(protocol::Bytes body) override
    {
        Packet p = parse(0xa1, body);
        if (!p) return std::nullopt;
        const auto* o = p.object();
        const auto position = getVec(o + 0x30), delta = getVec(o + 0x3c);
        const auto tick = get<std::uint64_t>(o + 0x58);
        const auto onGround = get<std::uint8_t>(o + 0x60), type = get<std::uint8_t>(o + 0x61);
        if (!position || !delta || !tick || !onGround || !type) return std::nullopt;
        return protocol::Correction{*type, *position, *delta, *onGround != 0, *tick};
    }
    std::optional<protocol::Respawn> readRespawn(protocol::Bytes body) override
    {
        Packet p = parse(0x2d, body);
        if (!p) return std::nullopt;
        const auto position = getVec(p.object() + 0x30);
        const auto state = get<std::uint8_t>(p.object() + 0x3c);
        const auto runtime = get<std::uint64_t>(p.object() + 0x40);
        if (!position || !state || !runtime) return std::nullopt;
        return protocol::Respawn{*position, *state, *runtime};
    }
    std::optional<protocol::ChangeDimension> readChangeDimension(protocol::Bytes body) override
    {
        Packet p = parse(0x3d, body);
        if (!p) return std::nullopt;
        const auto* o = p.object();
        const auto dimension = get<std::int32_t>(o + 0x30);
        const auto position = getVec(o + 0x34);
        const auto respawn = get<std::uint8_t>(o + 0x40), has = get<std::uint8_t>(o + 0x48);
        const auto id = get<std::uint32_t>(o + 0x44);
        if (!dimension || !position || !respawn || !has || !id) return std::nullopt;
        return protocol::ChangeDimension{*dimension, *position, *respawn != 0, *has ? std::optional<std::uint32_t>(*id) : std::nullopt};
    }
    std::optional<std::vector<std::uint64_t>> readLevelChunkBlobs(protocol::Bytes body) override
    {
        Packet p = parse(0x3a, body);
        if (!p) return std::nullopt;
        const auto list = elements(p.object() + 0x50, sizeof(std::uint64_t), 0x10000);
        if (!list) return std::nullopt;
        std::vector<std::uint64_t> hashes;
        for (auto* at : *list) if (const auto v = get<std::uint64_t>(at)) hashes.push_back(*v);
        return hashes;
    }
    std::optional<MobEquipment> readMobEquipment(protocol::Bytes body) override
    {
        Packet p = parse(0x1f, body);
        if (!p) return std::nullopt;
        const auto runtime = get<std::uint64_t>(p.object() + 0x30);
        const auto slot = get<std::uint8_t>(p.object() + 0x98), hotbar = get<std::uint8_t>(p.object() + 0x9c);
        const auto window = get<std::uint8_t>(p.object() + 0xa0);
        if (!runtime || !slot || !hotbar || !window) return std::nullopt;
        return MobEquipment{*runtime, *slot, *hotbar, *window};
    }
    std::optional<PlayerHotbar> readPlayerHotbar(protocol::Bytes body) override
    {
        Packet p = parse(0x30, body);
        if (!p) return std::nullopt;
        const auto slot = get<std::uint32_t>(p.object() + 0x30);
        const auto select = get<std::uint8_t>(p.object() + 0x34), window = get<std::uint8_t>(p.object() + 0x35);
        if (!slot || !select || !window) return std::nullopt;
        return PlayerHotbar{*slot, *window, *select != 0};
    }
    std::optional<InventoryContent> readInventoryContent(protocol::Bytes body) override
    {
        auto p = std::make_shared<Packet>(parse(0x31, body));
        if (!*p) return std::nullopt;
        const auto window = get<std::uint8_t>(p->object() + 0x30);
        const auto list = elements(p->object() + 0xa0, kDescriptorSize, 1024);
        if (!window || !list) return std::nullopt;
        InventoryContent content{*window, {}};
        for (auto* at : *list) content.items.push_back(itemFrom(p, at));
        return content;
    }
    std::optional<InventorySlot> readInventorySlot(protocol::Bytes body) override
    {
        auto p = std::make_shared<Packet>(parse(0x32, body));
        if (!*p) return std::nullopt;
        const auto window = get<std::uint8_t>(p->object() + 0x30);
        const auto slot = get<std::uint32_t>(p->object() + 0x34);
        if (!window || !slot) return std::nullopt;
        return InventorySlot{*window, *slot, itemFrom(p, p->object() + 0xb0)};
    }
    std::uint32_t blockRuntimeId(std::int32_t x, std::int32_t y, std::int32_t z) override
    {
        if (!g_blockNetworkIdOffset) return 0;
        void* const region = blockwrite::renderRegion();
        if (!region || !blockwrite::regionIsAlive(region)) return 0;
        const std::int32_t position[3] = {x, y, z};
        std::uint32_t id = 0;
        return blockIdGuarded(region, position, &id) ? id : 0;
    }
};
GameCodec g_codec;

struct Check { const char* name; bool ok; };
std::vector<Check> g_checks;
std::string mobEquipmentForCheck(std::uint64_t runtimeId, std::uint8_t slot)
{
    return g_codec.mobEquipment(1, runtimeId, {}, slot);
}
Packet reread(const std::string& packet, std::uint16_t id)
{
    if (packet.empty()) return {};
    const auto head = protocol::readHeader(protocol::bytes(packet));
    if (!head || head->id != id) return {};
    return parse(id, protocol::bytes(packet).subspan(head->bodyOffset));
}
template<class T> bool same(const Packet& p, std::size_t at, T expected)
{
    const auto value = get<T>(p.object() + at);
    return value && std::memcmp(&*value, &expected, sizeof(T)) == 0;
}
bool checkSends()
{
    bool all = true;
    const auto record = [&](const char* name, bool ok) { g_checks.push_back({name, ok}); all = all && ok; };
    {
        protocol::AuthInput in;
        in.pitch = 1.5f; in.yaw = 2.5f; in.position = {3.5f, 4.5f, 5.5f}; in.delta = {6.5f, 7.5f, 8.5f};
        in.moveX = 0.25f; in.moveZ = 0.75f; in.tick = 0x123456; in.handledTeleport = true; in.verticalCollision = true; in.extraFlags = {10, 53};
        const Packet r = reread(g_codec.playerAuthInput(1, in), 0x90);
        const std::uint64_t flags = (1ull << 10) | (1ull << 37) | (1ull << 48) | (1ull << 50) | (1ull << 53);
        record("PlayerAuthInput", r && same(r, 0x30, 1.5f) && same(r, 0x34, 2.5f) && same(r, 0x38, 3.5f) && same(r, 0x40, 5.5f)
            && same(r, 0x48, 6.5f) && same(r, 0x50, 8.5f) && same(r, 0x64, 0.25f) && same(r, 0x68, 0.75f) && same(r, 0x80, 0.25f)
            && same(r, 0x88, flags) && same(r, 0xa8, std::uint64_t{0x123456}));
    }
    {
        const Packet r = reread(g_codec.respawn(1, {1.5f, 2.5f, 3.5f}, 2, 0x2a), 0x2d);
        record("Respawn", r && same(r, 0x30, 1.5f) && same(r, 0x38, 3.5f) && same(r, 0x3c, std::uint8_t{2}) && same(r, 0x40, std::uint64_t{0x2a}));
    }
    {
        const Packet r = reread(g_codec.playerAction(1, 0x2a, 7), 0x24);
        record("PlayerAction", r && same(r, 0x48, std::int32_t{-1}) && same(r, 0x4c, std::int32_t{7}) && same(r, 0x50, std::uint64_t{0x2a}));
    }
    {
        const Packet r = reread(g_codec.loadingScreen(1, 1, 0x11223344u), 0x138);
        record("ServerboundLoadingScreen", r && same(r, 0x30, std::int32_t{1}) && same(r, 0x34, 0x11223344u) && same(r, 0x38, std::uint8_t{1}));
    }
    {
        const Packet r = reread(g_codec.requestChunkRadius(1, 7, 9), 0x45);
        record("RequestChunkRadius", r && same(r, 0x30, std::int32_t{7}) && same(r, 0x34, std::uint8_t{9}));
    }
    {
        const Packet r = reread(g_codec.setLocalPlayerAsInitialized(1, 0x2a), 0x71);
        record("SetLocalPlayerAsInitialized", r && same(r, 0x30, std::uint64_t{0x2a}));
    }
    {
        const std::uint64_t hashes[] = {0x1122334455667788ull, 0x99aabbccddeeff00ull};
        const Packet r = reread(g_codec.clientCacheBlobHits(1, hashes), 0x87);
        const auto list = r ? elements(r.object() + 0x48, 8, 16) : std::nullopt;
        record("ClientCacheBlobStatus", list && list->size() == 2 && get<std::uint64_t>((*list)[1]) == hashes[1]);
    }
    {
        const Packet r = reread(g_codec.disconnect(1), 0x05);
        record("Disconnect", r && same(r, 0x30, std::int32_t{0x1e}));
    }
    return all;
}
bool checkActions()
{
    bool all = true;
    const auto record = [&](const char* name, bool ok) { g_checks.push_back({name, ok}); all = all && ok; };
    {
        const Packet r = reread(mobEquipmentForCheck(0x2a, 3), 0x1f);
        record("MobEquipment", r && same(r, 0x30, std::uint64_t{0x2a}) && same(r, 0x98, std::int32_t{3}) && same(r, 0x9c, std::int32_t{3})
            && same(r, 0xa0, std::uint8_t{0}));
    }
    {
        const Packet r = reread(g_codec.animateSwing(1, 0x2a), 0x2c);
        record("Animate", r && same(r, 0x30, std::uint64_t{0x2a}) && same(r, 0x38, std::uint8_t{1}));
    }
    const std::size_t v = 0x30 + g_variantOffset;
    {
        UseItem in;
        in.action = 0; in.trigger = 1; in.x = 10; in.y = 20; in.z = 30; in.face = 1; in.hotbarSlot = 3; in.blockRuntimeId = 0x2b;
        in.position = {4.5f, 5.5f, 6.5f}; in.clicked = {0.5f, 1.0f, 0.5f}; in.prediction = 1;
        const auto bytes = g_codec.useItem(1, in);
        const Packet r = reread(bytes, 0x1e);
        if (r && !(same(r, v + tx::kUseTrigger, std::uint8_t{1}) && same(r, v + tx::kUseFace, std::uint8_t{1}) && same(r, v + tx::kUseSlot, std::int32_t{3}))) {
            std::wstring fields;
            for (const std::size_t at : {tx::kAction, tx::kUseTrigger, tx::kUseX, tx::kUseZ, tx::kUseBlock, tx::kUseFace, tx::kUseSlot, tx::kUsePosition + 8, tx::kUsePrediction})
                fields += std::format(L" +{:x}={:x}", at, get<std::uint32_t>(r.object() + v + at).value_or(0xdeadbeef));
            std::wstring hex;
            for (const unsigned char c : bytes) hex += std::format(L"{:02x}", c);
            log().info(L"Bot: self-check use item:{} bytes {}", fields, hex);
        }
        record("InventoryTransaction (use)", r && same(r, v + tx::kAction, std::int32_t{0}) && same(r, v + tx::kUseTrigger, std::uint8_t{1})
            && same(r, v + tx::kUseZ, std::int32_t{30}) && same(r, v + tx::kUseBlock, 0x2bu) && same(r, v + tx::kUseFace, std::uint8_t{1})
            && same(r, v + tx::kUseSlot, std::int32_t{3}) && same(r, v + tx::kUsePosition + 8, 6.5f) && same(r, v + tx::kUsePrediction, std::uint8_t{1}));
    }
    {
        ReleaseItem in;
        in.action = 0; in.hotbarSlot = 2; in.headPosition = {7.5f, 8.5f, 9.5f};
        const Packet r = reread(g_codec.releaseItem(1, in), 0x1e);
        record("InventoryTransaction (release)", r && same(r, v + tx::kReleaseSlot, std::int32_t{2}) && same(r, v + tx::kReleaseHead + 8, 9.5f));
    }
    return all;
}
bool checkInventoryView()
{
    bool all = true;
    const auto record = [&](const char* name, bool ok) { g_checks.push_back({name, ok}); all = all && ok; };
    {
        const Packet r = reread(containerCloseToGame(99, 2, true), 0x2f);
        record("InventoryView ContainerClose", r && same(r, 0x30, std::uint8_t{99}) && same(r, 0x31, std::uint8_t{2})
            && same(r, 0x32, std::uint8_t{1}));
    }
    {
        const Packet r = reread(g_codec.interact(1, 6, 42), 0x21);
        record("InventoryView Interact", r && same(r, 0x30, std::uint8_t{6}) && same(r, 0x38, std::uint64_t{42}));
    }
    {
        const Packet r = reread(inventorySlotToGame(99, 41, {}), 0x32);
        record("InventoryView InventorySlot", r && same(r, 0x30, std::uint8_t{99}) && same(r, 0x34, std::uint32_t{41})
            && same(r, 0xb0 + kDescImpl, std::uintptr_t{0}));
    }
    {
        const Packet r = reread(g_codec.mobEquipment(1, 42, {}, 8), 0x1f);
        record("InventoryView MobEquipment", r && same(r, 0x30, std::uint64_t{42}) && same(r, 0x98, std::uint8_t{8})
            && same(r, 0x9c, std::uint8_t{8}) && same(r, 0xa0, std::uint8_t{0}));
    }
    const auto bodyOf = [](const std::string& bytes) {
        const auto h = protocol::readHeader(protocol::bytes(bytes));
        return h ? bytes.substr(h->bodyOffset) : std::string();
    };
    {
        const std::int32_t ids[] = {-5, 7};
        std::string expected(1, 2);
        for (const auto id : ids) { expected.push_back(1); protocol::writeVarInt32(expected, id); expected.push_back(0); }
        record("InventoryView failure", bodyOf(stackFailureToGame(ids)) == expected);
    }
    for (std::uint8_t type = 0; type <= 4; ++type) {
        std::string body(1, 1); protocol::writeVarInt32(body, -7); body.push_back(1); body.push_back(static_cast<char>(type)); body.push_back(static_cast<char>(type));
        if (type != 2) body.push_back(0x11);
        const auto slot = [&](std::uint8_t c, std::uint8_t index, std::uint32_t net) {
            body.push_back(static_cast<char>(c)); body.push_back(0); body.push_back(static_cast<char>(index));
            for (unsigned i = 0; i < 4; ++i) body.push_back(static_cast<char>(net >> (8 * i)));
        };
        slot(7, 0x21, 0x1234);
        if (type <= 2) slot(59, 0, 0);
        if (type == 3) body.push_back(0);
        body.append(5, '\0');
        Packet p = parse(0x93, protocol::bytes(body));
        const auto written = p ? gamepacket::write(p) : std::nullopt;
        const auto requests = p ? elements(p.object() + 0x30, 0x48, 1) : std::nullopt;
        const auto actions = requests && requests->size() == 1 ? elements((*requests)[0] + 0x10, 0x60, 1) : std::nullopt;
        bool ok = written && *written == body && actions && actions->size() == 1;
        if (ok) {
            const auto* a = (*actions)[0]; const auto* from = a + (type == 2 ? 0 : 8);
            ok = get<std::int32_t>((*requests)[0] + 8) == -7 && get<std::uint8_t>(a + 0x58) == type
                && get<std::uint8_t>(from) == 7 && get<std::uint8_t>(from + 8) == 0
                && get<std::uint8_t>(from + 0xc) == 0x21 && get<std::int32_t>(from + 0x10) == 0x1234;
            if (type != 2) ok = ok && get<std::uint8_t>(a) == 0x11;
            if (type <= 2) {
                const auto* to = a + (type == 2 ? 0x28 : 0x30);
                ok = ok && get<std::uint8_t>(to) == 59 && get<std::uint8_t>(to + 8) == 0
                    && get<std::uint8_t>(to + 0xc) == 0 && get<std::int32_t>(to + 0x10) == 0;
            }
            if (type == 3) ok = ok && get<std::uint8_t>(a + 0x30) == 0;
        }
        static constexpr const char* names[] = {"InventoryView Take", "InventoryView Place", "InventoryView Swap", "InventoryView Drop", "InventoryView Destroy"};
        record(names[type], ok);
    }
    {
        static constexpr unsigned char bytes[] = {1, 0, 9, 1, 1, 28, 0, 1, 5, 5, 7, 1, 0xaa, 0x91, 2, 0, 0, 0};
        const protocol::Bytes body(reinterpret_cast<const std::uint8_t*>(bytes), sizeof bytes);
        Packet p = parse(0x94, body);
        const auto written = p ? gamepacket::write(p) : std::nullopt;
        const auto responses = g_codec.readItemStackResponse(body);
        bool ok = written && *written == std::string(reinterpret_cast<const char*>(bytes), sizeof bytes)
            && responses && responses->size() == 1 && (*responses)[0].status == 0 && (*responses)[0].requestId == -5
            && (*responses)[0].containers.size() == 1;
        if (ok) {
            const auto& c = (*responses)[0].containers[0];
            ok = c.container == 28 && c.slots.size() == 1 && c.slots[0].slot == 5 && c.slots[0].count == 7 && c.slots[0].netId == 0x4455;
        }
        record("InventoryView ItemStackResponse", ok);
    }
    {
        const auto check = [&](const std::string& packet, std::uint8_t type) {
            const Packet r = reread(packet, 0x93);
            const auto requests = r ? elements(r.object() + 0x30, 0x48, 1) : std::nullopt;
            const auto actions = requests && requests->size() == 1 ? elements((*requests)[0] + 0x10, 0x60, 1) : std::nullopt;
            if (!actions || actions->size() != 1) return false;
            const auto* a = (*actions)[0];
            bool ok = get<std::int32_t>((*requests)[0] + 8) == -1001 && get<std::uint8_t>(a + 0x58) == type && get<std::uint8_t>(a) == 7
                && get<std::uint8_t>(a + 8) == 59 && get<std::int32_t>(a + 8 + 0x10) == 0x4455;
            if (type == 1) ok = ok && get<std::uint8_t>(a + 0x30) == 29 && get<std::uint8_t>(a + 0x30 + 0xc) == 17;
            return ok;
        };
        record("InventoryView cursor place", check(cursorRequest(1, -1001, 7, 0x4455, std::pair<std::uint8_t, std::uint8_t>{29, 17}), 1));
        record("InventoryView cursor drop", check(cursorRequest(1, -1001, 7, 0x4455, std::nullopt), 3));
    }
    return all;
}

}

bool initialize()
{
    if (g_ready) return true;
    if (!gamepacket::available()) return false;
    g_copyDescriptor = functionStart(net::site(Site::DescriptorCopyCtor));
    g_stackDescriptor = functionStart(net::site(Site::StackDescriptorCtorSite));
    if (!g_stackDescriptor) log().warn(L"Bot: the game's item descriptor constructor is unavailable; other players' inventories cannot be shown");
    g_fromType = functionStart(net::site(Site::TransactionFromType));
    g_payloadCtor = functionStart(net::site(Site::TransactionPayloadCtor));
    g_payloadDtor = net::site(Site::TransactionPayloadDtor);
    if (auto* site = net::site(Site::BlockNetworkIdSite)) memory::copyGuarded(site + 11, &g_blockNetworkIdOffset, 4);
    if (!g_blockNetworkIdOffset || g_blockNetworkIdOffset > 0x1000) {
        g_blockNetworkIdOffset = 0;
        log().warn(L"Bot: cannot read the block network id field; placing blocks may be refused by the server");
    }
    std::uint8_t variant = 0;
    if (g_payloadDtor) memory::copyGuarded(static_cast<std::byte*>(g_payloadDtor) + 0x35, &variant, 1);
    g_variantOffset = variant;
    g_transactions = g_copyDescriptor && g_fromType && g_payloadCtor && g_payloadDtor && g_variantOffset;
    if (!g_transactions) log().warn(L"Bot: the game's inventory transaction functions are unavailable; bot actions that use items are unavailable");
    g_ready = true;
    {
        static constexpr std::array<std::int8_t, 3> kOffsets[] = {{1, 2, 3}, {-1, 0, 1}};
        g_subChunkRequests = true;
        const auto packet = buildSubChunkRequest(1, 0, 3, 4, 5, kOffsets);
        const auto head = packet.empty() ? std::nullopt : protocol::readHeader(protocol::bytes(packet));
        static const std::string kExpected("\x00\x02\x01\x02\x03\xff\x00\x01\x03\x00\x00\x00\x04\x00\x00\x00\x05\x00\x00\x00", 20);
        g_subChunkRequests = head && head->id == 0xaf && packet.substr(head->bodyOffset) == kExpected;
        g_checks.push_back({"SubChunkRequest", g_subChunkRequests});
        if (!g_subChunkRequests) log().warn(L"Bot: the game's SubChunkRequest did not keep the expected fields; far bots cannot fetch terrain for a path");
    }
    {
        static constexpr unsigned char kChunk[] = {4, 3, 0, 0, 1, 1, 1, 1, 9, 9, 9, 9, 9, 9, 9, 9, 1, 0, 0};
        std::string body(reinterpret_cast<const char*>(kChunk), sizeof kChunk), expected = body;
        for (int i = 0; i < 8; ++i) expected[8 + i] = static_cast<char>(0x11 * (i + 1));
        g_levelChunkBlobs = true;
        auto packet = levelChunkToProxy(protocol::bytes(body), 0x8877665544332211ull);
        auto head = packet.empty() ? std::nullopt : protocol::readHeader(protocol::bytes(packet));
        g_levelChunkBlobs = head && head->id == 0x3a && head->sender == 0 && head->recipient == 0 && packet.substr(head->bodyOffset) == expected;
        g_checks.push_back({"LevelChunkBlob", g_levelChunkBlobs});
        if (!g_levelChunkBlobs) log().warn(L"Bot: the game's LevelChunk did not keep the expected blob field; far bots wait for the game's chunk cache");
        body[5] = 0x1a; expected = body; expected[5] = 0x01;
        g_levelChunkLimit = true;
        packet = levelChunkToProxy(protocol::bytes(body), std::nullopt);
        head = packet.empty() ? std::nullopt : protocol::readHeader(protocol::bytes(packet));
        g_levelChunkLimit = head && head->id == 0x3a && packet.substr(head->bodyOffset) == expected;
        g_checks.push_back({"LevelChunkLimit", g_levelChunkLimit});
        if (!g_levelChunkLimit) log().warn(L"Bot: the game's LevelChunk did not keep the expected sub-chunk limit field; far bots may miss blocks built after they received a chunk");
    }
    const bool core = checkSends();
    const bool actions = g_transactions && checkActions();
    g_inventoryView = true;
    g_inventoryView = g_copyDescriptor && checkInventoryView();
    std::wstring failed;
    for (const auto& check : g_checks) if (!check.ok) failed += L" " + std::wstring(check.name, check.name + std::strlen(check.name));
    if (!core) {
        g_ready = false;
        notice::failOnce("BotCodec", L"Bot: the game's packets did not keep the expected fields:" + failed,
                         "Bot is unavailable: the game's packets changed (see the log)");
        return false;
    }
    if (!actions) {
        g_transactions = false; g_copyDescriptor = nullptr;
        notice::failOnce("BotCodecActions", L"Bot: the game's item packets did not keep the expected fields:" + failed,
                         "Bot actions that use items are unavailable: the game's packets changed (see the log)");
    }
    if (!g_inventoryView)
        notice::failOnce("BotInventoryView", L"Bot: the game's inventory view packets did not keep the expected fields:" + failed,
                         "Bot inventory view is unavailable: the game's packets changed (see the log)");
    log().info(L"Bot: game packet codec ready ({} checks; transaction variant +{:#x}{})", g_checks.size(), g_variantOffset,
        actions ? L"" : L"; item actions off");
    return true;
}
BotCodec* instance() { return g_ready ? &g_codec : nullptr; }

bool inventoryViewAvailable() { return g_ready && g_inventoryView; }
std::string containerCloseToGame(std::uint8_t window, std::uint8_t type, bool serverInitiated)
{
    return g_ready ? g_codec.containerClose(0, window, type, serverInitiated) : std::string();
}
std::string inventorySlotToGame(std::uint8_t window, std::uint32_t slot, const Item& item)
{
    if (!g_ready || (!item.empty() && !item.handle)) return {};
    Packet p = gamepacket::create(0x32);
    if (!p || !put(p.object() + 0x30, window) || !put(p.object() + 0x34, slot)) return {};
    if (!putItem(p.object() + 0xb0, item)) { p.abandon(); return {}; }
    return encode(p, 0);
}
Item itemFromStack(const void* stack)
{
    if (!g_ready || !g_stackDescriptor || !stack) return {};
    auto packet = std::make_shared<Packet>(gamepacket::create(0x32));
    if (!*packet) return {};
    auto* const at = packet->object() + 0xb0;
    if (!destroyDescriptorGuarded(at) || !stackDescriptorGuarded(at, stack)) { packet->abandon(); return {}; }
    return itemFrom(packet, at);
}
std::string cursorRequest(std::uint8_t sub, std::int32_t requestId, std::uint8_t count, std::int32_t cursorNetId,
                          std::optional<std::pair<std::uint8_t, std::uint8_t>> destination)
{
    if (!g_ready || !inventoryViewAvailable()) return {};
    std::string body(1, 1);
    protocol::writeVarInt32(body, requestId);
    body.push_back(1);
    const std::uint8_t type = destination ? 1 : 3;
    body.push_back(static_cast<char>(type)); body.push_back(static_cast<char>(type)); body.push_back(static_cast<char>(count));
    const auto slot = [&](std::uint8_t container, std::uint8_t index, std::int32_t net) {
        body.push_back(static_cast<char>(container)); body.push_back(0); body.push_back(static_cast<char>(index));
        for (unsigned i = 0; i < 4; ++i) body.push_back(static_cast<char>(static_cast<std::uint32_t>(net) >> (8 * i)));
    };
    slot(59, 0, cursorNetId);
    if (destination) slot(destination->first, destination->second, 0);
    else body.push_back(0);
    body.append(5, '\0');
    Packet p = parse(0x93, protocol::bytes(body));
    return p ? encode(p, sub) : std::string();
}
std::string stackFailureToGame(std::span<const std::int32_t> ids)
{
    if (!g_ready) return {};
    std::string body; protocol::writeVarUInt32(body, static_cast<std::uint32_t>(ids.size()));
    for (const auto id : ids) { body.push_back(1); protocol::writeVarInt32(body, id); body.push_back(0); }
    Packet p = parse(0x94, protocol::bytes(body));
    return p ? encode(p, 0) : std::string();
}
std::optional<std::string> translateStackRequest(protocol::Bytes body, std::uint8_t sub, const SlotMap& map,
                                               std::vector<std::int32_t>& ids)
{
    ids.clear();
    if (!inventoryViewAvailable()) return std::nullopt;
    Packet p = parse(0x93, body);
    const auto requests = p ? elements(p.object() + 0x30, 0x48, 4096) : std::nullopt;
    if (!requests) return std::nullopt;
    for (auto* r : *requests) {
        const auto id = get<std::int32_t>(r + 8);
        if (!id) return std::nullopt;
        ids.push_back(*id);
    }
    const auto translate = [&](std::byte* slotInfo) {
        auto c = get<std::uint8_t>(slotInfo), slot = get<std::uint8_t>(slotInfo + 0xc);
        if (!c || !slot || !map(*c, *slot)) return false;
        return put(slotInfo, *c) && put(slotInfo + 0xc, *slot);
    };
    for (auto* r : *requests) {
        const auto actions = elements(r + 0x10, 0x60, 4096);
        if (!actions) return std::nullopt;
        for (auto* a : *actions) {
            const auto type = get<std::uint8_t>(a + 0x58);
            if (!type || *type > 4) return std::nullopt;
            if (*type == 2) {
                if (!translate(a) || !translate(a + 0x28)) return std::nullopt;
            } else {
                if (!translate(a + 8)) return std::nullopt;
                if (*type < 2 && !translate(a + 0x30)) return std::nullopt;
            }
        }
    }
    auto encoded = encode(p, sub);
    return encoded.empty() ? std::nullopt : std::optional<std::string>(std::move(encoded));
}
std::optional<std::string> translateStackResponseToGame(protocol::Bytes body, const SlotMap& map,
                                                       std::vector<ResponseSlot>& seen)
{
    seen.clear();
    if (!inventoryViewAvailable()) return std::nullopt;
    Packet p = parse(0x94, body);
    const auto responses = p ? elements(p.object() + 0x30, 0x30, 4096) : std::nullopt;
    if (!responses) return std::nullopt;
    std::vector<ResponseSlot> translated;
    for (auto* r : *responses) {
        const auto containers = elements(r + 0x18, 0x28, 4096);
        if (!containers) return std::nullopt;
        for (auto* c : *containers) {
            const auto container = get<std::uint8_t>(c);
            const auto slots = elements(c + 0x10, 0x58, 4096);
            if (!container || !slots) return std::nullopt;
            std::optional<std::uint8_t> mappedContainer;
            for (auto* at : *slots) {
                auto slot = get<std::uint8_t>(at);
                auto mapped = *container;
                const auto count = get<std::uint8_t>(at + 2);
                const auto netId = get<std::int32_t>(at + 4);
                if (!slot || !count || !netId || !map(mapped, *slot)) return std::nullopt;
                if (mappedContainer && *mappedContainer != mapped) return std::nullopt;
                mappedContainer = mapped;
                if (!put(at, *slot) || !put<std::uint8_t>(at + 1, *slot)) return std::nullopt;
                translated.push_back({mapped, *slot, *count, *netId});
            }
            if (!mappedContainer) {
                auto mapped = *container;
                std::uint8_t slot = 0;
                if (!map(mapped, slot)) return std::nullopt;
                mappedContainer = mapped;
            }
            if (!put(c, *mappedContainer)) return std::nullopt;
        }
    }
    auto packet = encode(p, 0);
    if (packet.empty()) return std::nullopt;
    seen = std::move(translated);
    return packet;
}

std::string updateSoftEnumToGame(const std::string& name, const std::vector<std::string>& values, std::uint8_t action)
{
    if (!g_ready || values.size() > 4096) return {};
    Packet p = gamepacket::create(0x72);
    if (!p || !put(p.object() + 0x30, action) || !net::assignToGameString(p.object() + 0x38, name)) return {};
    if (!values.empty()) {
        const std::size_t bytes = values.size() * sizeof(std::string);
        auto* const at = p.object() + 0x58;
        if (!memory::isWritable(at, 24)) return {};
        auto* const data = static_cast<std::byte*>(gamestring::allocate(bytes));
        if (!data) return {};
        for (std::size_t i = 0; i < values.size(); ++i) gamestring::initialize(data + i * sizeof(std::string));
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (gamestring::assign(data + i * sizeof(std::string), values[i])) continue;
            for (std::size_t j = 0; j < i; ++j) gamestring::destroy(data + j * sizeof(std::string));
            gamestring::free(data, bytes);
            return {};
        }
        std::byte* const fields[3] = {data, data + bytes, data + bytes};
        std::memcpy(at, fields, sizeof fields);
    }
    return encode(p, 0);
}
std::string levelChunkToProxy(protocol::Bytes body, std::optional<std::uint64_t> blob)
{
    if (!g_ready || (blob ? !g_levelChunkBlobs : !g_levelChunkLimit)) return {};
    Packet p = parse(0x3a, body);
    if (!p) return {};
    if (blob) {
        const auto list = elements(p.object() + kChunkBlobs, sizeof(std::uint64_t), 2);
        if (!list || list->size() != 1 || !put((*list)[0], *blob)) return {};
    }
    if (g_levelChunkLimit) {
        const auto has = get<std::uint8_t>(p.object() + kChunkHasLimit);
        if (has && *has == 1 && !put(p.object() + kChunkLimit, std::int32_t{-1})) return {};
    }
    return encode(p, 0);
}
std::string requestNetworkSettings(std::int32_t protocol)
{
    if (!g_ready) return {};
    Packet p = gamepacket::create(0xc1);
    if (!p || !put(p.object() + 0x30, protocol)) return {};
    return encode(p, 0);
}
std::string login(std::int32_t protocol, std::string_view request)
{
    if (!g_ready) return {};
    std::string body;
    const auto value = static_cast<std::uint32_t>(protocol);
    for (int shift = 24; shift >= 0; shift -= 8) body.push_back(static_cast<char>(value >> shift));
    if (!protocol::writeString(body, request)) return {};
    Packet p = parse(0x01, protocol::bytes(body));
    return p ? encode(p, 0) : std::string();
}
std::string clientToServerHandshake()
{
    if (!g_ready) return {};
    Packet p = gamepacket::create(0x04);
    return p ? encode(p, 0) : std::string();
}
std::string resourcePackResponse(std::uint8_t response)
{
    if (!g_ready) return {};
    Packet p = gamepacket::create(0x08);
    if (!p || !put(p.object() + 0x40, response)) return {};
    return encode(p, 0);
}
std::optional<NetworkSettings> readNetworkSettings(protocol::Bytes body)
{
    if (!g_ready) return std::nullopt;
    Packet p = parse(0x8f, body);
    if (!p) return std::nullopt;
    const auto threshold = get<std::uint16_t>(p.object() + 0x30), algorithm = get<std::uint16_t>(p.object() + 0x32);
    if (!threshold || !algorithm) return std::nullopt;
    return NetworkSettings{*threshold, *algorithm};
}
std::string disconnectToGame(const std::string& message)
{
    if (!g_ready) return {};
    Packet p = gamepacket::create(0x05);
    if (!p || !put<std::int32_t>(p.object() + 0x30, 0) || !put<std::uint8_t>(p.object() + 0x78, 0)
        || !net::assignToGameString(p.object() + 0x38, message) || !net::assignToGameString(p.object() + 0x58, message)) return {};
    return encode(p, 0);
}

}
