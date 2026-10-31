#include "Possess.h"
#include "SkinSteal.h"
#include <algorithm>
#include <bit>
#include <cstring>

namespace tsukuyomi::bot::possess {
namespace {
using protocol::Bytes;
class Editor {
public:
    explicit Editor(Bytes in) : in_(in) {}
    bool ok() const { return ok_; }
    std::string take() { copyRest(); return std::move(out_); }
    void copy(std::size_t count)
    {
        if (!ok_ || in_.size() - pos_ < count) { ok_ = false; return; }
        out_.append(reinterpret_cast<const char*>(in_.data() + pos_), count);
        pos_ += count;
    }
    std::optional<std::uint64_t> readVar()
    {
        std::uint64_t value = 0;
        for (unsigned shift = 0; shift < 70 && ok_; shift += 7) {
            if (pos_ == in_.size()) break;
            const auto next = in_[pos_++];
            value |= static_cast<std::uint64_t>(next & 0x7f) << shift;
            if ((next & 0x80) == 0) return value;
        }
        ok_ = false;
        return std::nullopt;
    }
    std::optional<std::uint64_t> copyVarValue()
    {
        const auto start = pos_;
        const auto value = readVar();
        if (value) out_.append(reinterpret_cast<const char*>(in_.data() + start), pos_ - start);
        return value;
    }
    void copyVar() { copyVarValue(); }
    void copyString()
    {
        const auto length = readVar();
        if (!length) return;
        protocol::writeVarUInt64(out_, *length);
        copy(static_cast<std::size_t>(*length));
    }
    void runtime(const Ids& ids)
    {
        const auto value = readVar();
        if (!value) return;
        protocol::writeVarUInt64(out_, swapRuntime(*value, ids));
    }
    void unique(const Ids& ids)
    {
        const auto raw = readVar();
        if (!raw) return;
        const auto value = std::bit_cast<std::int64_t>((*raw >> 1) ^ (std::uint64_t{0} - (*raw & 1)));
        protocol::writeVarInt64(out_, swapUnique(value, ids));
    }
    void unique64(const Ids& ids)
    {
        if (in_.size() - pos_ < 8) { ok_ = false; return; }
        std::int64_t value = 0;
        std::memcpy(&value, in_.data() + pos_, 8);
        pos_ += 8;
        value = swapUnique(value, ids);
        out_.append(reinterpret_cast<const char*>(&value), 8);
    }
    void tick(std::int64_t delta)
    {
        const auto value = readVar();
        if (!value) return;
        protocol::writeVarUInt64(out_, static_cast<std::uint64_t>(static_cast<std::int64_t>(*value) + delta));
    }
    std::optional<std::uint8_t> peekByte() const { return pos_ < in_.size() ? std::optional<std::uint8_t>(in_[pos_]) : std::nullopt; }
    void swapUnique64InRest(const Ids& ids)
    {
        if (!ok_) return;
        std::string rest(reinterpret_cast<const char*>(in_.data() + pos_), in_.size() - pos_);
        pos_ = in_.size();
        if (ids.active) {
            for (std::size_t at = 0; at + 8 <= rest.size(); ++at) {
                std::int64_t value = 0;
                std::memcpy(&value, rest.data() + at, 8);
                if (value != ids.uniqueA && value != ids.uniqueB) continue;
                value = swapUnique(value, ids);
                std::memcpy(rest.data() + at, &value, 8);
                at += 7;
            }
        }
        out_ += rest;
    }
    void swapUuidInRest(const Ids& ids)
    {
        if (!ok_) return;
        std::string rest(reinterpret_cast<const char*>(in_.data() + pos_), in_.size() - pos_);
        pos_ = in_.size();
        if (ids.active && ids.uuids) {
            for (std::size_t at = 0; at + 16 <= rest.size(); ++at) {
                const Uuid* to = nullptr;
                if (std::memcmp(rest.data() + at, ids.uuidA.data(), 16) == 0) to = &ids.uuidB;
                else if (std::memcmp(rest.data() + at, ids.uuidB.data(), 16) == 0) to = &ids.uuidA;
                if (!to) continue;
                std::memcpy(rest.data() + at, to->data(), 16);
                at += 15;
            }
        }
        out_ += rest;
    }
    static std::uint64_t swapRuntime(std::uint64_t value, const Ids& ids)
    {
        if (!ids.active) return value;
        return value == ids.runtimeA ? ids.runtimeB : value == ids.runtimeB ? ids.runtimeA : value;
    }
    static std::int64_t swapUnique(std::int64_t value, const Ids& ids)
    {
        if (!ids.active) return value;
        return value == ids.uniqueA ? ids.uniqueB : value == ids.uniqueB ? ids.uniqueA : value;
    }
private:
    void copyRest() { if (ok_ && pos_ < in_.size()) copy(in_.size() - pos_); }
    Bytes in_;
    std::size_t pos_ = 0;
    std::string out_;
    bool ok_ = true;
};
bool runtimeFirst(std::uint16_t id)
{
    switch (id) {
    case 0x12: case 0x13: case 0x1b: case 0x1c: case 0x1d: case 0x1f: case 0x20: case 0x24: case 0x27: case 0x28:
    case 0x4b: case 0x62: case 0x6f: case 0x71: case 0x8a: case 0x98: case 0x9d: case 0x13e:
        return true;
    default:
        return false;
    }
}
bool uniqueFirst(std::uint16_t id)
{
    switch (id) {
    case 0x0e: case 0x4a: case 0x9b: case 0xb6: case 0x145: case 0x146:
        return true;
    default:
        return false;
    }
}
}
std::optional<std::string> rewrite(std::string_view packet, const Ids& ids, std::uint8_t recipient, std::uint8_t sender,
                                   std::int64_t tickDelta)
{
    const auto data = protocol::bytes(packet);
    const auto head = protocol::readHeader(data);
    if (!head) return std::nullopt;
    std::string out;
    protocol::writeVarUInt32(out, protocol::header(head->id, recipient, sender));
    Editor e(data.subspan(head->bodyOffset));
    const auto id = head->id;
    if (runtimeFirst(id)) e.runtime(ids);
    else if (uniqueFirst(id)) e.unique(ids);
    else switch (id) {
    case 0x0c: {
        if (data.size() - head->bodyOffset < 16) return std::nullopt;
        Uuid uuid{};
        std::memcpy(uuid.data(), data.data() + head->bodyOffset, 16);
        if (ids.active && ids.uuids && uuid == ids.uuidA) uuid = ids.uuidB;
        else if (ids.active && ids.uuids && uuid == ids.uuidB) uuid = ids.uuidA;
        out.append(reinterpret_cast<const char*>(uuid.data()), 16);
        Editor f(data.subspan(head->bodyOffset + 16));
        f.copyString(); f.runtime(ids); f.swapUnique64InRest(ids);
        if (!f.ok()) return std::nullopt;
        return out + f.take();
    }
    case 0x3f: case 0x5d:
    case 0x4d: case 0x4f:
        e.swapUuidInRest(ids);
        break;
    case 0x0d: case 0x0f: case 0x16:
        e.unique(ids); e.runtime(ids);
        break;
    case 0x11:
        e.runtime(ids); e.runtime(ids);
        break;
    case 0x21:
        e.copy(1); e.runtime(ids);
        break;
    case 0x23:
        e.unique64(ids);
        break;
    case 0x2c:
        e.copyVar(); e.runtime(ids);
        break;
    case 0x2d:
        e.copy(13); e.runtime(ids);
        break;
    case 0x49:
        e.unique(ids); e.unique(ids);
        break;
    case 0x90: {
        e.copy(32);
        const auto count = e.copyVarValue();
        if (!count || *count > 256) return std::nullopt;
        for (std::uint64_t i = 0; i < *count; ++i) e.copyVar();
        e.copyVar(); e.copyVar(); e.copyVar();
        e.copy(8);
        e.tick(tickDelta);
        break;
    }
    case 0x97:
        e.copyVar(); e.unique(ids); e.tick(tickDelta);
        break;
    case 0xa1: {
        e.copy(1 + 12 + 12 + 8);
        const auto has = e.peekByte();
        if (!has || *has > 1) return std::nullopt;
        e.copy(1);
        if (*has) e.copy(4);
        e.copy(1);
        e.tick(tickDelta);
        break;
    }
    case 0xbb:
        e.unique64(ids);
        break;
    default:
        break;
    }
    if (!e.ok()) return std::nullopt;
    out += e.take();
    return out;
}
std::optional<AuthInputHead> readAuthInputHead(protocol::Bytes body)
{
    protocol::Reader r(body);
    AuthInputHead result;
    const auto pitch = r.float32(), yaw = r.float32(), x = r.float32(), y = r.float32(), z = r.float32();
    if (!pitch || !yaw || !x || !y || !z) return std::nullopt;
    result.pitch = *pitch; result.yaw = *yaw; result.position = {*x, *y, *z};
    for (int i = 0; i < 3; ++i) if (!r.float32()) return std::nullopt;
    const auto count = r.varUInt32();
    if (!count || *count > 256) return std::nullopt;
    for (std::uint32_t i = 0; i < *count; ++i) if (!r.varInt32()) return std::nullopt;
    if (!r.varUInt32() || !r.varUInt32() || !r.varInt32() || !r.float32() || !r.float32()) return std::nullopt;
    const auto tick = r.varUInt64();
    if (!tick) return std::nullopt;
    result.tick = *tick;
    return result;
}
std::optional<StartGameHead> readStartGameHead(protocol::Bytes body)
{
    protocol::Reader r(body);
    const auto unique = r.varInt64();
    const auto runtime = r.varUInt64();
    const auto mode = r.varInt32();
    const auto x = r.float32(), y = r.float32(), z = r.float32(), pitch = r.float32(), yaw = r.float32();
    if (!unique || !runtime || !mode || !x || !y || !z || !pitch || !yaw) return std::nullopt;
    StartGameHead head{*unique, *runtime, *mode, {*x, *y, *z}, *pitch, *yaw, std::nullopt};
    if (r.fixed64() && r.fixedBytes(2) && r.string()) {
        head.dimension = r.varInt32();
        if (*mode == 5 && head.dimension && r.varInt32())
            if (const auto world = r.varInt32()) head.gameMode = *world;
    }
    return head;
}
namespace {
constexpr std::size_t kMaxEntities = 1024;
std::optional<std::int64_t> firstUnique(protocol::Bytes body) { return protocol::Reader(body).varInt64(); }
std::string text(protocol::Bytes body) { return {reinterpret_cast<const char*>(body.data()), body.size()}; }
}
bool StateKeeper::wants(std::uint16_t id)
{
    switch (id) {
    case 0x0c: case 0x0d: case 0x0e: case 0x0f: case 0x16: case 0x3a: case 0x3f: case 0x79:
        return true;
    default:
        return false;
    }
}
void StateKeeper::reset()
{
    entities_.clear(); runtimeToUnique_.clear(); listed_.clear(); chunks_.clear();
}
void StateKeeper::observe(std::uint16_t id, protocol::Bytes body)
{
    switch (id) {
    case 0x0c: {
        protocol::Reader r(body);
        if (!r.fixedBytes(8) || !r.fixedBytes(8) || !r.string()) return;
        const auto runtime = r.varUInt64();
        if (!runtime || *runtime == selfRuntime_) return;
        const auto key = -static_cast<std::int64_t>(*runtime) - 1;
        if (entities_.size() >= kMaxEntities && !entities_.count(key)) return;
        auto& entity = entities_[key];
        entity = Entity{};
        entity.runtimeId = *runtime; entity.spawnId = id; entity.spawn = text(body);
        runtimeToUnique_[*runtime] = key;
        return;
    }
    case 0x0d: case 0x0f: case 0x16: {
        protocol::Reader r(body);
        const auto unique = r.varInt64();
        const auto runtime = r.varUInt64();
        if (!unique || !runtime || *runtime == selfRuntime_) return;
        if (entities_.size() >= kMaxEntities && !entities_.count(*unique)) return;
        auto& entity = entities_[*unique];
        entity = Entity{};
        entity.runtimeId = *runtime; entity.spawnId = id; entity.spawn = text(body);
        runtimeToUnique_[*runtime] = *unique;
        return;
    }
    case 0x0e: {
        const auto unique = firstUnique(body);
        if (!unique) return;
        const auto found = entities_.find(*unique);
        if (found != entities_.end()) { runtimeToUnique_.erase(found->second.runtimeId); entities_.erase(found); return; }
        for (auto it = entities_.begin(); it != entities_.end(); ++it) {
            if (it->second.spawnId != 0x0c) continue;
            const auto& spawn = it->second.spawn;
            bool hit = false;
            for (std::size_t at = 0; at + 8 <= spawn.size() && !hit; ++at) {
                std::int64_t value = 0;
                std::memcpy(&value, spawn.data() + at, 8);
                hit = value == *unique;
            }
            if (hit) { runtimeToUnique_.erase(it->second.runtimeId); entities_.erase(it); return; }
        }
        return;
    }
    case 0x3a: {
        protocol::Reader r(body);
        const auto x = r.varInt32(), z = r.varInt32(), dimension = r.varInt32();
        if (!x || !z || !dimension) return;
        const auto key = std::tuple{*dimension, *x, *z};
        if (chunks_.size() >= 4096 && !chunks_.count(key)) return;
        chunks_[key] = text(body);
        return;
    }
    case 0x79: {
        protocol::Reader r(body);
        const auto x = r.varInt32(), y = r.varInt32(), z = r.varInt32();
        const auto blocks = r.varUInt32();
        if (!x || !y || !z || !blocks || *blocks < 16) return;
        dropChunksOutside(*x >> 4, *z >> 4, static_cast<std::int32_t>((std::min)(*blocks, 4096u) / 16) + kChunkMargin);
        return;
    }
    case 0x3f:
        if (const auto rows = skin::readPlayerList(body)) {
            for (const auto& row : *rows) {
                if (row.add) listed_[row.uniqueId] = row.uuid;
                else std::erase_if(listed_, [&](const auto& entry) { return entry.second == row.uuid; });
            }
        }
        return;
    default:
        return;
    }
}
void StateKeeper::dropChunksOutside(std::int32_t chunkX, std::int32_t chunkZ, std::int32_t radius)
{
    const std::int64_t limit = std::int64_t{radius} * radius;
    std::erase_if(chunks_, [&](const auto& entry) {
        const std::int64_t dx = std::int64_t{std::get<1>(entry.first)} - chunkX, dz = std::int64_t{std::get<2>(entry.first)} - chunkZ;
        return dx * dx + dz * dz > limit;
    });
}
std::vector<std::string> StateKeeper::chunksNear(std::int32_t chunkX, std::int32_t chunkZ, std::int32_t dimension, std::int32_t radius) const
{
    std::vector<std::pair<std::int64_t, const std::string*>> near;
    for (const auto& [key, body] : chunks_) {
        const auto& [dim, x, z] = key;
        const std::int64_t dx = x - chunkX, dz = z - chunkZ;
        if (dim != dimension || dx * dx + dz * dz > static_cast<std::int64_t>(radius) * radius) continue;
        near.emplace_back(dx * dx + dz * dz, &body);
    }
    std::stable_sort(near.begin(), near.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<std::string> out;
    out.reserve(near.size());
    for (const auto& [distance, body] : near) out.push_back(*body);
    return out;
}
std::optional<Uuid> StateKeeper::listedUuid(std::int64_t uniqueId) const
{
    if (const auto it = listed_.find(uniqueId); it != listed_.end()) return it->second;
    return std::nullopt;
}
std::optional<Uuid> StateKeeper::playerUuid(std::uint64_t runtimeId) const
{
    const auto found = runtimeToUnique_.find(runtimeId);
    if (found == runtimeToUnique_.end()) return std::nullopt;
    const auto entity = entities_.find(found->second);
    if (entity == entities_.end() || entity->second.spawnId != 0x0c || entity->second.spawn.size() < 16) return std::nullopt;
    Uuid uuid{};
    std::memcpy(uuid.data(), entity->second.spawn.data(), 16);
    return uuid;
}
std::optional<std::string> StateKeeper::playerName(std::uint64_t runtimeId) const
{
    const auto found = runtimeToUnique_.find(runtimeId);
    if (found == runtimeToUnique_.end()) return std::nullopt;
    const auto entity = entities_.find(found->second);
    if (entity == entities_.end() || entity->second.spawnId != 0x0c) return std::nullopt;
    protocol::Reader r(protocol::bytes(entity->second.spawn));
    if (!r.fixedBytes(8) || !r.fixedBytes(8)) return std::nullopt;
    return r.string();
}
}
