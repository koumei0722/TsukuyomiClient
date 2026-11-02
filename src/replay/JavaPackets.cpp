#include "replay/JavaPackets.h"
#include "replay/JeTables.h"
#include "replay/JeWorldTables.h"
#include <algorithm>
#include <bit>
#include <map>
#include <cmath>
#include <stdexcept>

namespace tsukuyomi::replay {
void ByteWriter::varint(std::uint32_t v) { varlong(v); }
void ByteWriter::varlong(std::uint64_t v)
{ do { u8(static_cast<std::uint8_t>((v & 127) | (v > 127 ? 128 : 0))); v >>= 7; } while (v); }
void ByteWriter::i16(std::uint16_t v) { u8(static_cast<std::uint8_t>(v >> 8)); u8(static_cast<std::uint8_t>(v)); }
void ByteWriter::i32(std::uint32_t v) { i16(static_cast<std::uint16_t>(v >> 16)); i16(static_cast<std::uint16_t>(v)); }
void ByteWriter::i64(std::uint64_t v) { i32(static_cast<std::uint32_t>(v >> 32)); i32(static_cast<std::uint32_t>(v)); }
void ByteWriter::f32(float v) { i32(std::bit_cast<std::uint32_t>(v)); }
void ByteWriter::f64(double v) { i64(std::bit_cast<std::uint64_t>(v)); }
void ByteWriter::string(std::string_view v) { varint(static_cast<std::uint32_t>(v.size())); bytes({reinterpret_cast<const std::uint8_t*>(v.data()), v.size()}); }
void ByteWriter::bitset(std::uint64_t bits)
{
    const auto count = static_cast<unsigned>((std::bit_width(bits) + 7) / 8);
    varint(static_cast<std::uint32_t>(count));
    for (unsigned i = 0; i < count; ++i) { u8(static_cast<std::uint8_t>(bits)); bits >>= 8; }
}
void NbtWriter::string(std::string_view utf8)
{
    ByteWriter encoded;
    auto unit = [&](std::uint32_t c) {
        if (c && c < 0x80) encoded.u8(static_cast<std::uint8_t>(c));
        else if (c < 0x800) { encoded.u8(static_cast<std::uint8_t>(0xc0 | (c >> 6))); encoded.u8(static_cast<std::uint8_t>(0x80 | (c & 63))); }
        else { encoded.u8(static_cast<std::uint8_t>(0xe0 | (c >> 12))); encoded.u8(static_cast<std::uint8_t>(0x80 | ((c >> 6) & 63))); encoded.u8(static_cast<std::uint8_t>(0x80 | (c & 63))); }
    };
    for (std::size_t i = 0; i < utf8.size();) {
        const auto b = static_cast<unsigned char>(utf8[i++]); std::uint32_t c = b; unsigned n = 0;
        if (b >= 0xf0 && b <= 0xf4) { c = b & 7; n = 3; }
        else if (b >= 0xe0 && b <= 0xef) { c = b & 15; n = 2; }
        else if (b >= 0xc2 && b <= 0xdf) { c = b & 31; n = 1; }
        else if (b >= 0x80) c = 0xfffd;
        const unsigned total = n;
        bool valid = true;
        while (n--) {
            if (i == utf8.size() || (static_cast<unsigned char>(utf8[i]) & 0xc0) != 0x80) { valid = false; break; }
            c = (c << 6) | (static_cast<unsigned char>(utf8[i++]) & 63);
        }
        if (!valid || (total == 1 && c < 0x80) || (total == 2 && c < 0x800) || (total == 3 && c < 0x10000) || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) c = 0xfffd;
        if (c < 0x10000) unit(c);
        else { c -= 0x10000; unit(0xd800 | (c >> 10)); unit(0xdc00 | (c & 1023)); }
    }
    if (encoded.data.size() > 65535) throw std::runtime_error("NBT string exceeds modified UTF-8 limit");
    out.i16(static_cast<std::uint16_t>(encoded.data.size())); out.bytes(encoded.data);
}
const DimensionInfo& dimensionInfo(Dimension dim)
{
    static constexpr DimensionInfo info[] = {
        {24, -4, "minecraft:overworld", 0, 63, 41},
        {16, 0, "minecraft:the_nether", 3, 32, 35},
        {16, 0, "minecraft:the_end", 2, 0, 58}
    };
    return info[static_cast<unsigned>(dim)];
}
namespace java {
namespace {
ByteWriter packet(int id) { ByteWriter w; w.varint(id); return w; }
void packedPosition(ByteWriter& w, int x, int y, int z) {
    w.i64((static_cast<std::uint64_t>(x & 0x3ffffff) << 38) | (static_cast<std::uint64_t>(z & 0x3ffffff) << 12) | (y & 0xfff));
}
ByteWriter boss(const Uuid& uuid, int operation) { auto w = packet(9); w.uuid(uuid); w.varint(operation); return w; }
Bytes componentPacket(int id, const Component& c) { auto w = packet(id); w.bytes(c); return w.data; }
void profile(ByteWriter& w, const Uuid& uuid, std::string_view name)
{ w.uuid(uuid); w.string(profileName(name)); w.varint(0); }
void position(ByteWriter& w, double x, double y, double z, float yaw, float pitch, float head)
{ w.f64(x); w.f64(y); w.f64(z); w.f32(yaw); w.f32(pitch); w.f32(head); }
void packed(ByteWriter& w, std::span<const std::uint16_t> values, unsigned bits)
{
    const unsigned perLong = 64 / bits;
    for (std::size_t i = 0; i < values.size(); i += perLong) {
        std::uint64_t word = 0;
        for (unsigned j = 0; j < perLong && i + j < values.size(); ++j)
            word |= static_cast<std::uint64_t>(values[i + j]) << (j * bits);
        w.i64(word);
    }
}
}
Bytes login(const Login& o)
{
    const auto& d = dimensionInfo(o.dim);
    ByteWriter w;
    w.varint(50); w.i32(o.playerId); w.u8(0); w.varint(3);
    for (auto dim : {Dimension::Overworld, Dimension::Nether, Dimension::End}) w.string(dimensionInfo(dim).key);
    w.varint(o.maxPlayers); w.varint(o.chunkRadius); w.varint(o.simulationDistance);
    w.u8(0); w.u8(1); w.u8(0); w.varint(d.type); w.string(d.key); w.i64(o.hashedSeed);
    w.varint(static_cast<std::uint32_t>(o.gamemode));
    w.varint(o.previousGamemode < 0 ? 0u : static_cast<std::uint32_t>(o.previousGamemode) + 1);
    w.u8(0); w.u8(0); w.u8(0); w.varint(0); w.varint(d.seaLevel); w.u8(0); w.u8(0);
    return w.data;
}
Bytes playerInfoAdd(const Uuid& uuid, std::string_view name, int mode)
{
    ByteWriter w; w.varint(71); w.u8(13); w.varint(1); profile(w, uuid, name); w.varint(mode); w.u8(1); return w.data;
}
Bytes setTime(std::uint64_t gameTime, std::uint64_t dayTicks)
{
    ByteWriter w; w.varint(115); w.i64(gameTime); w.varint(2);
    for (int i = 0; i < 2; ++i) { w.varint(i); w.varlong(dayTicks); w.f32(0); w.f32(1); }
    return w.data;
}
Bytes gameEventWaitChunks() { ByteWriter w; w.varint(39); w.u8(13); w.f32(0); return w.data; }
Bytes blockUpdate(int x, int y, int z, int state)
{
    ByteWriter w; w.varint(8);
    w.i64((static_cast<std::uint64_t>(x & 0x3ffffff) << 38) | (static_cast<std::uint64_t>(z & 0x3ffffff) << 12) | (y & 0xfff));
    w.varint(state); return w.data;
}
Bytes sectionBlocksUpdate(int sx, int sy, int sz, std::span<const BlockChange> changes)
{
    ByteWriter w; w.varint(86);
    w.i64((static_cast<std::uint64_t>(sx & 0x3fffff) << 42) | (static_cast<std::uint64_t>(sz & 0x3fffff) << 20) | (sy & 0xfffff));
    w.varint(static_cast<std::uint32_t>(changes.size()));
    for (const auto& b : changes) w.varlong((static_cast<std::uint64_t>(b.state) << 12) | ((b.x & 15) << 8) | ((b.z & 15) << 4) | (b.y & 15));
    return w.data;
}
void palettedContainer(ByteWriter& w, std::span<const std::uint16_t> values, bool biome)
{
    std::vector<std::uint16_t> palette;
    std::map<std::uint16_t, std::uint16_t> indices;
    std::vector<std::uint16_t> mapped;
    mapped.reserve(values.size());
    for (auto v : values) {
        auto [it, added] = indices.try_emplace(v, static_cast<std::uint16_t>(palette.size()));
        if (added) palette.push_back(v);
        mapped.push_back(it->second);
    }
    if (palette.size() <= 1) { w.u8(0); w.varint(palette.empty() ? 0 : palette[0]); return; }
    unsigned bits = static_cast<unsigned>(std::bit_width(palette.size() - 1));
    if (!biome) bits = std::max(4u, bits);
    const bool direct = bits > (biome ? 3u : 8u);
    if (direct) bits = biome ? 7 : 16;
    w.u8(static_cast<std::uint8_t>(bits));
    if (!direct) {
        w.varint(static_cast<std::uint32_t>(palette.size()));
        for (auto v : palette) w.varint(v);
    }
    packed(w, direct ? values : std::span<const std::uint16_t>(mapped), bits);
}
Bytes levelChunkWithLight(const Column& c)
{
    const auto& d = dimensionInfo(c.dim);
    ByteWriter w, sections, entities;
    w.varint(46); w.i32(c.cx); w.i32(c.cz); w.varint(3);
    std::array<std::array<std::uint16_t, 256>, 3> heights{};
    std::uint32_t entityCount = 0;
    std::uint64_t skyMask = 0, blockMask = 0, emptySky = c.dim == Dimension::Overworld ? 1 : 0, emptyBlock = 0;
    std::vector<Bytes> skies, lights;
    std::array<std::uint16_t, 4096> air{};
    Section missing; missing.biomes.fill(static_cast<std::uint16_t>(d.biome));
    int highest = -1;
    for (int s = 0; s < d.sections; ++s) {
        const auto& section = s < static_cast<int>(c.sections.size()) ? c.sections[s] : missing;
        if (std::any_of(section.blocks.begin(), section.blocks.end(), [](auto v) { return (jetables::stateFlags(v) & 1) == 0; })) highest = s;
    }
    for (int s = 0; s < d.sections; ++s) {
        const auto& section = s < static_cast<int>(c.sections.size()) ? c.sections[s] : missing;
        const std::span<const std::uint16_t> blocks = section.blocks.size() == 4096 ? std::span<const std::uint16_t>(section.blocks) : std::span<const std::uint16_t>(air);
        unsigned nonAir = 0, fluid = 0;
        for (unsigned i = 0; i < 4096; ++i) {
            const auto state = blocks[i]; const auto flags = jetables::stateFlags(state);
            const auto h = static_cast<std::uint16_t>(s * 16 + (i >> 8) + 1);
            const unsigned cell = i & 255;
            if (!(flags & 1)) { ++nonAir; heights[0][cell] = h; }
            if (flags & 2) ++fluid;
            if (flags & 4) heights[1][cell] = h;
            if (flags & 8) heights[2][cell] = h;
            if (flags & 16) {
                const int type = jetables::blockEntityType(state);
                if (type >= 0) {
                    ++entityCount; entities.u8(static_cast<std::uint8_t>(((i & 15) << 4) | ((i >> 4) & 15)));
                    entities.i16(static_cast<std::uint16_t>((s + d.minSection) * 16 + (i >> 8))); entities.varint(type); entities.u8(0);
                }
            }
        }
        sections.i16(static_cast<std::uint16_t>(nonAir)); sections.i16(static_cast<std::uint16_t>(fluid));
        palettedContainer(sections, blocks, false); palettedContainer(sections, section.biomes, true);
        const auto bit = std::uint64_t{1} << (s + 1);
        if (section.block.size() == 2048 && std::any_of(section.block.begin(), section.block.end(), [](auto v) { return v != 0; })) { blockMask |= bit; lights.push_back(section.block); }
        else emptyBlock |= bit;
        if (c.dim == Dimension::Overworld && s <= highest && section.sky.size() == 2048) {
            if (std::all_of(section.sky.begin(), section.sky.end(), [](auto v) { return v == 0; })) emptySky |= bit;
            else if (!std::all_of(section.sky.begin(), section.sky.end(), [](auto v) { return v == 255; })) { skyMask |= bit; skies.push_back(section.sky); }
        }
    }
    const int types[] = {1, 4, 5};
    for (unsigned i = 0; i < 3; ++i) { w.varint(types[i]); w.varint(37); packed(w, heights[i], 9); }
    w.varint(static_cast<std::uint32_t>(sections.data.size())); w.bytes(sections.data);
    w.varint(entityCount); w.bytes(entities.data);
    w.bitset(skyMask); w.bitset(blockMask); w.bitset(emptySky); w.bitset(emptyBlock);
    for (const auto* arrays : {&skies, &lights}) {
        w.varint(static_cast<std::uint32_t>(arrays->size()));
        for (const auto& a : *arrays) { w.varint(2048); w.bytes(a); }
    }
    return w.data;
}
Bytes createLocalPlayer(const Uuid& uuid, std::string_view name, int mode, const PlayerPosition& p)
{
    ByteWriter w; w.uuid(uuid); position(w, p.x, p.y, p.z, p.yaw, p.pitch, p.yaw);
    w.f64(0); w.f64(0); w.f64(0); profile(w, uuid, name); w.varint(mode); return w.data;
}
Bytes moveEntities(std::string_view key, int id, double x, double y, double z, float yaw, float pitch, float head, bool ground)
{
    const EntityPose entity{id, x, y, z, yaw, pitch, head, ground}; return moveEntities(key, {&entity, 1});
}
Bytes moveEntities(std::string_view key, std::span<const EntityPose> entities)
{
    ByteWriter w; w.varint(1); w.string(key); w.varint(static_cast<std::uint32_t>(entities.size()));
    for (const auto& e : entities) { w.varint(e.id); position(w, e.x, e.y, e.z, e.yaw, e.pitch, e.headYaw); w.u8(e.onGround ? 1 : 0); }
    return w.data;
}
std::string profileName(std::string_view name) {
    std::size_t at = 0; unsigned units = 0;
    while (at < name.size()) {
        const auto c = static_cast<unsigned char>(name[at]);
        const unsigned size = c < 0x80 ? 1 : (c >= 0xc2 && c <= 0xdf) ? 2 : (c >= 0xe0 && c <= 0xef) ? 3 : (c >= 0xf0 && c <= 0xf4) ? 4 : 0;
        const unsigned used = size == 4 ? 2 : 1;
        if (!size || units + used > 16 || size > name.size() - at) break;
        bool valid = true; for (unsigned i = 1; i < size; ++i) if ((static_cast<unsigned char>(name[at + i]) & 0xc0) != 0x80) valid = false;
        if (!valid) break; at += size; units += used;
    }
    return std::string(name.substr(0, at));
}
Component textComponent(std::string_view text) { NbtWriter n; n.out.u8(8); n.string(text); return n.out.data; }
void writeItem(ByteWriter& w, const ItemStack& item) {
    const bool empty = item.count <= 0 || item.id <= 0; w.varint(empty ? 0 : item.count); if (empty) return;
    w.varint(item.id); w.varint(static_cast<unsigned>(item.glint.has_value()) + static_cast<unsigned>(item.name.has_value()) + static_cast<unsigned>(item.colour.has_value()) + static_cast<unsigned>(item.damage.has_value())); w.varint(0);
    if (item.glint) { w.varint(jeworld::kComponentEnchantmentGlintOverride); w.u8(*item.glint); }
    if (item.name) { w.varint(jeworld::kComponentCustomName); w.bytes(*item.name); }
    if (item.colour) { w.varint(jeworld::kComponentDyedColor); w.i32(*item.colour); }
    if (item.damage) { w.varint(jeworld::kComponentDamage); w.varint(*item.damage); }
}
EntityData dataByte(int i, std::uint8_t v) { return {static_cast<std::uint8_t>(i), 0, {v}}; }
EntityData dataInt(int i, int v, int serializer) { ByteWriter w; w.varint(v); return {static_cast<std::uint8_t>(i), serializer, w.data}; }
EntityData dataFloat(int i, float v) { ByteWriter w; w.f32(v); return {static_cast<std::uint8_t>(i), 3, w.data}; }
EntityData dataBoolean(int i, bool v) { return {static_cast<std::uint8_t>(i), 8, {static_cast<std::uint8_t>(v)}}; }
EntityData dataComponent(int i, const std::optional<Component>& v) { ByteWriter w; w.u8(v.has_value()); if (v) w.bytes(*v); return {static_cast<std::uint8_t>(i), 6, w.data}; }
EntityData dataItem(int i, const ItemStack& v) { ByteWriter w; writeItem(w, v); return {static_cast<std::uint8_t>(i), 7, w.data}; }
Bytes addEntity(int id, const Uuid& uuid, int type, double x, double y, double z, float pitch, float yaw, float head, int data) {
    auto w = packet(1); w.varint(id); w.uuid(uuid); w.varint(type); w.f64(x); w.f64(y); w.f64(z); w.u8(0);
    for (float a : {pitch, yaw, head}) w.u8(std::isfinite(a) ? static_cast<std::uint8_t>(static_cast<int>(std::fmod(a, 360.0f) * (256.0f / 360.0f)) & 255) : 0);
    w.varint(data); return w.data;
}
Bytes removeEntities(std::span<const int> ids) { auto w = packet(78); w.varint(static_cast<std::uint32_t>(ids.size())); for (auto id : ids) w.varint(id); return w.data; }
Bytes setEntityData(int id, std::span<const EntityData> values) { auto w = packet(101); w.varint(id); for (const auto& v : values) { w.u8(v.index); w.varint(v.serializer); w.bytes(v.value); } w.u8(255); return w.data; }
Bytes setEquipment(int id, std::span<const Equipment> slots) {
    auto w = packet(104); w.varint(id);
    for (std::size_t i = 0; i < slots.size(); ++i) { w.u8(slots[i].slot | (i + 1 < slots.size() ? 128 : 0)); writeItem(w, slots[i].item); } return w.data;
}
Bytes swingAnimation(int id, bool offhand) { auto w = packet(123); w.varint(id); w.varint(offhand ? 1 : 0); w.u8(1); w.u8(6); return w.data; }
Bytes animate(int id, std::uint8_t action) { auto w = packet(2); w.varint(id); w.u8(action); return w.data; }
Bytes hurtAnimation(int id, float yaw) { auto w = packet(43); w.varint(id); w.f32(yaw); return w.data; }
Bytes entityEvent(int id, std::uint8_t event) { auto w = packet(34); w.i32(id); w.u8(event); return w.data; }
Bytes takeItemEntity(int item, int collector, int count) { auto w = packet(127); w.varint(item); w.varint(collector); w.varint(count); return w.data; }
Bytes setPassengers(int vehicle, std::span<const int> riders) { auto w = packet(109); w.varint(vehicle); w.varint(static_cast<std::uint32_t>(riders.size())); for (int id : riders) w.varint(id); return w.data; }
Bytes setHealth(float health, int food, float saturation) { auto w = packet(106); w.f32(health); w.varint(food); w.f32(saturation); return w.data; }
Bytes setExperience(float progress, int level, int total) { auto w = packet(105); w.f32(progress); w.varint(level); w.varint(total); return w.data; }
Bytes updateMobEffect(int id, int effect, int amplifier, int duration, std::uint8_t flags) { auto w = packet(135); w.varint(id); w.varint(effect); w.varint(amplifier); w.varint(duration); w.u8(flags); return w.data; }
Bytes removeMobEffect(int id, int effect) { auto w = packet(79); w.varint(id); w.varint(effect); return w.data; }
Bytes playerInfoUpdate(std::uint8_t actions, std::span<const PlayerInfo> entries) {
    auto w = packet(71); w.u8(actions); w.varint(static_cast<std::uint32_t>(entries.size()));
    for (const auto& e : entries) {
        w.uuid(e.uuid);
        if (actions & 1) { w.string(profileName(e.name)); w.varint(0); }
        if (actions & 4) w.varint(e.gameMode);
        if (actions & 8) w.u8(e.listed);
        if (actions & 32) { w.u8(e.displayName.has_value()); if (e.displayName) w.bytes(*e.displayName); }
    } return w.data;
}
Bytes playerInfoRemove(std::span<const Uuid> ids) { auto w = packet(70); w.varint(static_cast<std::uint32_t>(ids.size())); for (const auto& id : ids) w.uuid(id); return w.data; }
Bytes systemChat(const Component& c, bool overlay) { auto w = packet(124); w.bytes(c); w.u8(overlay); return w.data; }
Bytes setTitleText(const Component& c) { return componentPacket(116, c); }
Bytes setSubtitleText(const Component& c) { return componentPacket(114, c); }
Bytes setActionBarText(const Component& c) { return componentPacket(89, c); }
Bytes setTitlesAnimation(int in, int stay, int out) { auto w = packet(117); w.i32(in); w.i32(stay); w.i32(out); return w.data; }
Bytes clearTitles(bool reset) { auto w = packet(14); w.u8(reset); return w.data; }
Bytes bossEventAdd(const Uuid& uuid, const Component& title, float progress, int colour, int overlay, std::uint8_t flags) { auto w = boss(uuid, 0); w.bytes(title); w.f32(progress); w.varint(colour); w.varint(overlay); w.u8(flags); return w.data; }
Bytes bossEventRemove(const Uuid& uuid) { return boss(uuid, 1).data; }
Bytes bossEventProgress(const Uuid& uuid, float progress) { auto w = boss(uuid, 2); w.f32(progress); return w.data; }
Bytes bossEventName(const Uuid& uuid, const Component& title) { auto w = boss(uuid, 3); w.bytes(title); return w.data; }
Bytes bossEventStyle(const Uuid& uuid, int colour, int overlay) { auto w = boss(uuid, 4); w.varint(colour); w.varint(overlay); return w.data; }
Bytes bossEventProperties(const Uuid& uuid, std::uint8_t flags) { auto w = boss(uuid, 5); w.u8(flags); return w.data; }
Bytes gameEvent(std::uint8_t event, float value) { auto w = packet(39); w.u8(event); w.f32(value); return w.data; }
Bytes levelEvent(int type, int x, int y, int z, int data, bool global) { auto w = packet(47); w.i32(type); packedPosition(w, x, y, z); w.i32(data); w.u8(global); return w.data; }
Bytes levelParticles(int particle, double x, double y, double z, int count) {
    auto w = packet(48); w.varint(particle); w.u8(0); w.u8(0); w.f64(x); w.f64(y); w.f64(z);
    for (int i = 0; i < 3; ++i) w.f32(0); for (int i = 0; i < 3; ++i) w.f32(1);
    w.varint(count); w.varint(0); return w.data;
}
Bytes sound(int id, int source, double x, double y, double z, float volume, float pitch, std::uint64_t seed) {
    auto w = packet(119); w.varint(id + 1); w.varint(source);
    for (double a : {x, y, z}) w.i32(static_cast<std::uint32_t>(static_cast<std::int32_t>(a * 8)));
    w.f32(volume); w.f32(pitch); w.i64(seed); return w.data;
}
Bytes blockEvent(int x, int y, int z, std::uint8_t action, std::uint8_t param, int block) { auto w = packet(7); packedPosition(w, x, y, z); w.u8(action); w.u8(param); w.varint(block); return w.data; }
Bytes blockEntityData(int x, int y, int z, int type, const Bytes& nbt) { auto w = packet(6); packedPosition(w, x, y, z); w.varint(type); w.bytes(nbt); return w.data; }
Bytes containerSetSlot(int slot, const ItemStack& item) { auto w = packet(20); w.varint(0); w.varint(0); w.i16(static_cast<std::uint16_t>(slot)); writeItem(w, item); return w.data; }
Bytes setHeldSlot(int slot) { auto w = packet(107); w.varint(slot); return w.data; }
}
}
