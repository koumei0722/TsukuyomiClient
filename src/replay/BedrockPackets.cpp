#include "replay/BedrockPackets.h"
#include "replay/JeWorldTables.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>

namespace tsukuyomi::replay::bedrock {
const Nbt* Nbt::find(std::string_view key) const
{
    for (auto it = children.rbegin(); it != children.rend(); ++it) if (it->first == key) return &it->second;
    return nullptr;
}
namespace {
int defaultShield()
{
    for (int id : jeworld::kBedrockItemIds) if (jeworld::bedrockItemName(id) == "minecraft:shield") return id;
    return 0;
}
std::atomic<int>& shieldId() { static std::atomic<int> id{defaultShield()}; return id; }
struct Reader {
    std::span<const std::uint8_t> data;
    std::size_t at = 0;
    bool ok = true;
    std::size_t left() const { return data.size() - at; }
    std::span<const std::uint8_t> take(std::size_t n) {
        if (n > left()) { ok = false; return {}; }
        auto out = data.subspan(at, n); at += n; return out;
    }
    std::uint8_t u8() { auto s = take(1); return s.empty() ? 0 : s[0]; }
    bool boolean() { auto v = u8(); if (v > 1) ok = false; return v != 0; }
    template<class T> T fixed() {
        T v{}; auto s = take(sizeof(T)); if (!s.empty()) std::memcpy(&v, s.data(), sizeof(T)); return v;
    }
    std::uint64_t vu(unsigned bits = 64) {
        std::uint64_t v = 0;
        for (unsigned s = 0; s < bits; s += 7) {
            const auto b = u8();
            if (!ok || (s + 7 > bits && (b & 127u) >= (1u << (bits - s)))) { ok = false; return 0; }
            v |= static_cast<std::uint64_t>(b & 127u) << s;
            if (!(b & 128)) return v;
        }
        ok = false; return 0;
    }
    std::uint32_t vu32() { return static_cast<std::uint32_t>(vu(32)); }
    std::int32_t vi32() { auto n = vu32(); return std::bit_cast<std::int32_t>((n >> 1) ^ (0u - (n & 1))); }
    std::int64_t vi64() { auto n = vu(); return std::bit_cast<std::int64_t>((n >> 1) ^ (0ull - (n & 1))); }
    float f() { return fixed<float>(); }
    std::string strN(std::size_t n) { const auto s = take(n); return {s.begin(), s.end()}; }
    std::string str() { auto n = vu32(); return strN(n); }
    std::span<const std::uint8_t> blob() { auto n = vu32(); return take(n); }
    std::size_t count() { auto n = vu32(); if (n > left()) { ok = false; return 0; } return n; }
    template<class F> auto list(F f) {
        using T = decltype(f()); std::vector<T> out;
        auto n = count(); for (std::size_t i = 0; ok && i < n; ++i) out.push_back(f()); return out;
    }
    template<class F> auto optional(F f) {
        using T = decltype(f()); std::optional<T> out; if (boolean() && ok) out = f(); return out;
    }
    Vec3 vec() { const auto x = f(), y = f(), z = f(); return {x, y, z}; }
    BlockPos pos() { const auto x = vi32(), y = vi32(), z = vi32(); return {x, y, z}; }
    float angle() { return u8() * (360.0f / 256.0f); }
    Uuid uuid() {
        Uuid out{}; const auto s = take(16); if (s.size() == 16) {
            std::reverse_copy(s.begin(), s.begin() + 8, out.begin()); std::reverse_copy(s.begin() + 8, s.end(), out.begin() + 8);
        } return out;
    }
    std::string nbtString(bool network) { const auto n = network ? vu32() : fixed<std::uint16_t>(); return strN(n); }
    Nbt nbt(bool network = true) {
        auto type = u8(); if (!type) return {};
        nbtString(network); return nbtValue(type, network, 0);
    }
    Nbt nbtValue(std::uint8_t type, bool network, unsigned depth) {
        Nbt out; out.type = type;
        if (!ok || depth > 512) { ok = false; return out; }
        auto i32 = [&] { return network ? vi32() : fixed<std::int32_t>(); };
        auto i64 = [&] { return network ? vi64() : fixed<std::int64_t>(); };
        switch (type) {
        case 1: out.integer = fixed<std::int8_t>(); break;
        case 2: out.integer = fixed<std::int16_t>(); break;
        case 3: out.integer = i32(); break;
        case 4: out.integer = i64(); break;
        case 5: out.real = f(); break;
        case 6: out.real = fixed<double>(); break;
        case 8: out.text = nbtString(network); break;
        case 7: { const auto n = i32(); if (n < 0) ok = false; else { auto s = take(n); out.bytes.assign(s.begin(), s.end()); } break; }
        case 9: case 11: case 12: {
            const auto sub = type == 9 ? u8() : static_cast<std::uint8_t>(type == 11 ? 3 : 4);
            const auto n = i32();
            if (n < 0 || static_cast<std::size_t>(n) > left() || (n && !sub)) { ok = false; break; }
            for (int i = 0; ok && i < n; ++i) out.list.push_back(nbtValue(sub, network, depth + 1));
            break;
        }
        case 10:
            while (ok) { const auto sub = u8(); if (!sub) break; auto key = nbtString(network); auto val = nbtValue(sub, network, depth + 1); out.children.emplace_back(std::move(key), std::move(val)); }
            break;
        default: ok = false;
        }
        return out;
    }
    ItemInstance item() {
        ItemInstance x; x.networkId = fixed<std::int16_t>(); x.count = fixed<std::uint16_t>(); x.metadata = vu32();
        if (boolean()) vi32(); x.blockRuntimeId = vu32();
        Reader s{blob()};
        if (!s.data.empty()) {
            const auto len = s.fixed<std::int16_t>();
            if (len) { if (len == -1 && s.u8() != 1) s.ok = false; x.userData = s.nbt(false); }
            for (int k = 0; k < 2 && s.ok; ++k) {
                const auto n = s.fixed<std::uint32_t>(); if (n > s.left()) { s.ok = false; break; }
                for (std::uint32_t i = 0; s.ok && i < n; ++i) { auto size = s.fixed<std::int16_t>(); if (size < 0) s.ok = false; else s.take(size); }
            }
            if (x.networkId == shieldId().load(std::memory_order_relaxed)) s.fixed<std::int64_t>();
        }
        if (!s.ok || s.left()) ok = false;
        return x;
    }
    Metadata metadata() {
        Metadata out; auto n = count();
        for (std::size_t i = 0; ok && i < n; ++i) {
            const auto key = vu32(), type = vu32(); if (u8() != type) { ok = false; break; }
            switch (type) {
            case 0: out[key] = u8(); break;
            case 1: out[key] = fixed<std::int16_t>(); break;
            case 2: out[key] = vi32(); break;
            case 3: out[key] = f(); break;
            case 4: out[key] = str(); break;
            case 5: out[key] = nbt(); break;
            case 6: out[key] = pos(); break;
            case 7: out[key] = vi64(); break;
            case 8: out[key] = vec(); break;
            default: ok = false;
            }
        } return out;
    }
    void properties() {
        auto n = count(); for (std::size_t i = 0; ok && i < n; ++i) { vu32(); vi32(); }
        n = count(); for (std::size_t i = 0; ok && i < n; ++i) { vu32(); f(); }
    }
    EntityLink link() { EntityLink x; x.ridden = vi64(); x.rider = vi64(); x.type = u8(); x.immediate = boolean(); x.riderInitiated = boolean(); x.angularVelocity = f(); return x; }
    void passenger() { if (boolean()) { pos(); vec(); f(); f(); u8(); } }
    ContainerName container() { ContainerName x; x.id = u8(); x.dynamicId = optional([&] { return fixed<std::uint32_t>(); }); return x; }
    Attribute attribute(bool full) {
        Attribute x;
        if (full) {
            x.min = f(); x.max = f(); x.value = f(); f(); f(); x.defaultValue = f(); x.name = str();
            auto n = count(); for (std::size_t i = 0; ok && i < n; ++i) { str(); str(); f(); fixed<std::int32_t>(); fixed<std::int32_t>(); boolean(); }
        } else { x.name = str(); x.min = f(); x.value = f(); x.max = f(); }
        return x;
    }
    void skin() {
        str(); blob(); auto width = fixed<std::uint32_t>(), height = fixed<std::uint32_t>(); const auto image = blob();
        auto area = static_cast<std::uint64_t>(width) * height;
        if (area > UINT64_MAX / 4 || area * 4 != image.size()) ok = false;
        auto n = count(); for (std::size_t i = 0; ok && i < n; ++i) { fixed<std::uint32_t>(); fixed<std::uint32_t>(); blob(); vu32(); f(); vu32(); }
        width = fixed<std::uint32_t>(); height = fixed<std::uint32_t>(); const auto cape = blob();
        area = static_cast<std::uint64_t>(width) * height;
        if (area > UINT64_MAX / 4 || area * 4 != cape.size()) ok = false;
        blob(); blob(); blob(); str(); str(); u8(); take(4);
        n = count(); for (std::size_t i = 0; ok && i < n; ++i) { str(); fixed<std::uint32_t>(); uuid(); boolean(); str(); }
        n = count(); for (std::size_t i = 0; ok && i < n; ++i) { str(); take(16); }
        for (int i = 0; i < 5; ++i) boolean(); str(); str();
    }
};
Packet readPacket(Reader& r, std::uint16_t id) {
    switch (id) {
    case 0x09: {
        Text x; x.needsTranslation = r.boolean(); auto cat = r.u8(); x.type = r.u8();
        const auto t = x.type; const auto expected = (t == 1 || t == 7 || t == 8) ? 1 : (t == 2 || t == 3 || t == 4) ? 2 : 0;
        if (t > 11 || cat != expected) r.ok = false;
        if (expected == 1) x.source = r.str(); x.message = r.str();
        if (expected == 2) x.parameters = r.list([&] { return r.str(); });
        r.str(); r.str(); r.optional([&] { return r.str(); }); return x;
    }
    case 0x0a: return SetTime{r.vi32()};
    case 0x0b: { StartGameHead x; x.uniqueId = r.vi64(); x.runtimeId = r.vu(); x.gameMode = r.vi32(); return x; }
    case 0x0c: {
        AddPlayer x; x.uuid = r.uuid(); x.name = r.str(); x.runtimeId = r.vu(); r.str(); x.position = r.vec(); x.velocity = r.vec();
        x.pitch = r.f(); x.yaw = r.f(); x.headYaw = r.f(); x.heldItem = r.item(); x.gameMode = r.vi32(); x.metadata = r.metadata(); r.properties();
        x.uniqueId = r.fixed<std::int64_t>(); r.u8(); r.u8();
        auto n = r.count(); for (std::size_t i = 0; r.ok && i < n; ++i) { r.fixed<std::uint16_t>(); r.fixed<std::uint32_t>(); r.fixed<std::uint32_t>(); r.f(); r.f(); r.f(); }
        x.links = r.list([&] { return r.link(); }); r.passenger(); r.str(); r.fixed<std::int32_t>(); return x;
    }
    case 0x0d: {
        AddActor x; x.uniqueId = r.vi64(); x.runtimeId = r.vu(); x.type = r.str(); x.position = r.vec(); x.velocity = r.vec();
        x.pitch = r.f(); x.yaw = r.f(); x.headYaw = r.f(); x.bodyYaw = r.f(); x.attributes = r.list([&] { return r.attribute(false); });
        x.metadata = r.metadata(); r.properties(); x.links = r.list([&] { return r.link(); }); r.passenger(); return x;
    }
    case 0x0e: return RemoveActor{r.vi64()};
    case 0x0f: { AddItemActor x; x.uniqueId = r.vi64(); x.runtimeId = r.vu(); x.item = r.item(); x.position = r.vec(); x.velocity = r.vec(); x.metadata = r.metadata(); x.fromFishing = r.boolean(); return x; }
    case 0x11: { TakeItemActor x; x.itemId = r.vu(); x.takerId = r.vu(); return x; }
    case 0x12: { MoveActorAbsolute x; x.runtimeId = r.vu(); x.flags = r.u8(); x.position = r.vec(); x.pitch = r.angle(); x.yaw = r.angle(); x.headYaw = r.angle(); return x; }
    case 0x13: { MovePlayer x; x.runtimeId = r.vu(); x.position = r.vec(); x.pitch = r.f(); x.yaw = r.f(); x.headYaw = r.f(); x.mode = r.u8(); x.onGround = r.boolean(); x.riddenId = r.vu(); if (r.boolean()) { r.fixed<std::int32_t>(); r.fixed<std::int32_t>(); } x.tick = r.vu(); return x; }
    case 0x19: { LevelEvent x; x.type = r.vi32(); x.position = r.vec(); x.data = r.vi32(); return x; }
    case 0x1a: { BlockEvent x; x.position = r.pos(); x.type = r.vi32(); x.data = r.vi32(); return x; }
    case 0x1b: { ActorEvent x; x.runtimeId = r.vu(); x.type = r.u8(); x.data = r.vi32(); x.fireAt = r.optional([&] { return r.vec(); }); return x; }
    case 0x1c: { MobEffect x; x.runtimeId = r.vu(); x.operation = r.u8(); x.effect = r.vi32(); x.amplifier = r.vi32(); x.particles = r.boolean(); x.duration = r.vi32(); x.tick = r.vu(); x.ambient = r.boolean(); return x; }
    case 0x1d: { UpdateAttributes x; x.runtimeId = r.vu(); x.attributes = r.list([&] { return r.attribute(true); }); x.tick = r.vu(); return x; }
    case 0x1f: { MobEquipment x; x.runtimeId = r.vu(); x.item = r.item(); x.inventorySlot = r.u8(); x.hotBarSlot = r.u8(); x.window = r.u8(); return x; }
    case 0x20: { MobArmourEquipment x; x.runtimeId = r.vu(); x.helmet = r.item(); x.chestplate = r.item(); x.leggings = r.item(); x.boots = r.item(); x.body = r.item(); return x; }
    case 0x27: { SetActorData x; x.runtimeId = r.vu(); x.metadata = r.metadata(); r.properties(); x.tick = r.vu(); return x; }
    case 0x29: return SetActorLink{r.link()};
    case 0x2a: return SetHealth{r.vi32()};
    case 0x2c: {
        Animate x; x.action = r.u8(); x.runtimeId = r.vu(); x.data = r.f(); x.source = r.optional([&] { return r.str(); }); x.hand = r.u8();
        constexpr std::string_view sources[] = {"none", "build", "mine", "interact", "attack", "useitem", "throwitem", "dropitem", "event"};
        if (x.source && std::find(std::begin(sources), std::end(sources), *x.source) == std::end(sources)) r.ok = false; return x;
    }
    case 0x30: { PlayerHotBar x; x.selected = r.vu32(); x.window = r.u8(); x.select = r.boolean(); return x; }
    case 0x31: { InventoryContent x; x.window = r.vu32(); x.items = r.list([&] { return r.item(); }); x.container = r.container(); x.storage = r.item(); return x; }
    case 0x32: { InventorySlot x; x.window = r.vu32(); x.slot = r.vu32(); x.container = r.optional([&] { return r.container(); }); x.storage = r.optional([&] { return r.item(); }); x.item = r.item(); return x; }
    case 0x38: { BlockActorData x; x.position = r.pos(); x.nbt = r.nbt(); return x; }
    case 0x3d: { ChangeDimension x; x.dimension = r.vi32(); x.position = r.vec(); x.respawn = r.boolean(); x.loadingScreen = r.optional([&] { return r.fixed<std::uint32_t>(); }); return x; }
    case 0x3e: return SetPlayerGameType{r.vi32()};
    case 0x3f: {
        PlayerList x; x.entries = r.list([&] {
            PlayerListEntry e; auto variant = r.vu32(); r.u8(); if (variant > 1) r.ok = false; e.add = variant == 1; e.uuid = r.uuid();
            if (e.add) { e.uniqueId = r.vi64(); e.name = r.str(); e.xuid = r.str(); r.str(); r.str(); e.buildPlatform = r.fixed<std::int32_t>(); r.skin(); r.boolean(); r.boolean(); r.boolean(); r.take(4); } return e;
        }); return x;
    }
    case 0x4a: { BossEvent x; x.uniqueId = r.vi64(); x.type = r.u8(); x.title = r.str(); x.filteredTitle = r.str(); x.health = r.f(); x.colour = r.u8(); x.overlay = r.u8(); return x; }
    case 0x56: { PlaySound x; x.name = r.str(); auto p = r.pos(); x.position = {p.x / 8.0f, p.y / 8.0f, p.z / 8.0f}; x.volume = r.f(); x.pitch = r.f(); r.vi32(); r.boolean(); r.optional([&] { return r.fixed<std::uint64_t>(); }); r.optional([&] { return r.f(); }); return x; }
    case 0x58: { SetTitle x; x.action = r.vi32(); x.text = r.str(); x.fadeIn = r.vi32(); x.stay = r.vi32(); x.fadeOut = r.vi32(); r.str(); r.str(); r.str(); return x; }
    case 0x6f: { MoveActorDelta x; x.runtimeId = r.vu(); x.x = r.optional([&] { return r.f(); }); x.y = r.optional([&] { return r.f(); }); x.z = r.optional([&] { return r.f(); }); x.pitch = r.optional([&] { return r.angle(); }); x.yaw = r.optional([&] { return r.angle(); }); x.headYaw = r.optional([&] { return r.angle(); }); x.onGround = r.boolean(); r.boolean(); r.boolean(); r.boolean(); x.tick = r.vu(); return x; }
    case 0x76: { SpawnParticleEffect x; x.dimension = r.u8(); x.uniqueId = r.vi64(); x.position = r.vec(); x.name = r.str(); r.optional([&] { return r.blob(); }); return x; }
    case 0x7b: { LevelSoundEvent x; x.sound = r.str(); x.position = r.vec(); x.extraData = r.vi32(); x.entity = r.str(); x.baby = r.boolean(); x.disableRelativeVolume = r.boolean(); x.uniqueId = r.fixed<std::int64_t>(); x.fireAt = r.optional([&] { return r.vec(); }); return x; }
    case 0x90: {
        PlayerAuthInputHead x; x.pitch = r.f(); x.yaw = r.f(); x.position = r.vec(); r.f(); r.f(); x.headYaw = r.f();
        x.flags = r.list([&] { return r.vi32(); });
        constexpr int flagCount = 66;
        for (std::size_t i = 0; i < x.flags.size(); ++i) if (x.flags[i] < 0 || x.flags[i] >= flagCount || std::find(x.flags.begin(), x.flags.begin() + i, x.flags[i]) != x.flags.begin() + i) r.ok = false;
        return x;
    }
    case 0x97: { UpdatePlayerGameType x; x.gameMode = r.vi32(); x.uniqueId = r.vi64(); x.tick = r.vu(); return x; }
    case 0xa2: { ItemRegistry x; x.items = r.list([&] { auto name = r.str(); int rid = r.fixed<std::int16_t>(); r.boolean(); r.vi32(); r.nbt(); return std::pair{rid, std::move(name)}; }); return x; }
    default: r.ok = false; return Text{};
    }
}
}
void setShieldNetworkId(int id) { shieldId().store(id, std::memory_order_relaxed); }
bool wanted(std::uint16_t id, bool sent) {
    if (sent) return id == 0x1f || id == 0x2c || id == 0x7b || id == 0x90;
    switch (id) {
    case 0x09: case 0x0a: case 0x0b: case 0x0c: case 0x0d: case 0x0e: case 0x0f: case 0x11: case 0x12: case 0x13:
    case 0x19: case 0x1a: case 0x1b: case 0x1c: case 0x1d: case 0x1f: case 0x20: case 0x27: case 0x29: case 0x2a: case 0x2c:
    case 0x30: case 0x31: case 0x32: case 0x38: case 0x3d: case 0x3e: case 0x3f: case 0x4a: case 0x56: case 0x58:
    case 0x6f: case 0x76: case 0x7b: case 0x97: case 0xa2: return true;
    default: return false;
    }
}
std::optional<Packet> parse(std::uint16_t id, std::span<const std::uint8_t> body, bool sent) {
    if (!wanted(id, sent)) return std::nullopt;
    try {
        Reader r{body}; auto out = readPacket(r, id);
        if (!r.ok || (id != 0x0b && id != 0x90 && r.left())) return std::nullopt;
        if (auto* items = std::get_if<ItemRegistry>(&out)) for (const auto& [rid, name] : items->items) if (name == "minecraft:shield") setShieldNetworkId(rid);
        return out;
    } catch (...) { return std::nullopt; }
}
}
