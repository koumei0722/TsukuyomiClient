#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tsukuyomi::replay {
using Bytes = std::vector<std::uint8_t>;
using Uuid = std::array<std::uint8_t, 16>;
class ByteWriter {
public:
    Bytes data;
    void u8(std::uint8_t v) { data.push_back(v); }
    void varint(std::uint32_t v);
    void varlong(std::uint64_t v);
    void i16(std::uint16_t v);
    void i32(std::uint32_t v);
    void i64(std::uint64_t v);
    void f32(float v);
    void f64(double v);
    void string(std::string_view v);
    void uuid(const Uuid& v) { bytes(v); }
    void bitset(std::uint64_t bits);
    void bytes(std::span<const std::uint8_t> v) { data.insert(data.end(), v.begin(), v.end()); }
};
enum class Dimension { Overworld, Nether, End };
struct DimensionInfo { int sections, minSection; std::string_view key; int type, seaLevel, biome; };
const DimensionInfo& dimensionInfo(Dimension dim);
struct Section {
    std::vector<std::uint16_t> blocks;
    std::array<std::uint16_t, 64> biomes{};
    Bytes sky, block;
};
struct Column { Dimension dim = Dimension::Overworld; int cx = 0, cz = 0; std::vector<Section> sections; };
struct BlockChange { int x, y, z; std::uint16_t state; };
struct PlayerPosition {
    double x = 0, y = 0, z = 0;
    float yaw = 0, pitch = 0;
    bool onGround = false;
    bool operator==(const PlayerPosition&) const = default;
};
struct EntityPose { int id; double x, y, z; float yaw, pitch, headYaw; bool onGround; };
class NbtWriter {
public:
    ByteWriter out;
    void string(std::string_view utf8);
    void tag(std::uint8_t type, std::string_view name) { out.u8(type); string(name); }
    void end() { out.u8(0); }
};
namespace java {
using Component = Bytes;
std::string profileName(std::string_view name);
Component textComponent(std::string_view text);
struct ItemStack {
    int count = 0, id = 0;
    std::optional<bool> glint;
    std::optional<Component> name;
    std::optional<std::uint32_t> colour;
    std::optional<int> damage;
    bool operator==(const ItemStack&) const = default;
};
void writeItem(ByteWriter& out, const ItemStack& item);
struct EntityData { std::uint8_t index; int serializer; Bytes value; bool operator==(const EntityData&) const = default; };
EntityData dataByte(int index, std::uint8_t value);
EntityData dataInt(int index, int value, int serializer = 1);
EntityData dataFloat(int index, float value);
EntityData dataBoolean(int index, bool value);
EntityData dataComponent(int index, const std::optional<Component>& value);
EntityData dataItem(int index, const ItemStack& value);
struct Equipment { std::uint8_t slot; ItemStack item; };
struct PlayerInfo { Uuid uuid{}; std::string name; int gameMode = 0; bool listed = true; std::optional<Component> displayName; };
Bytes addEntity(int id, const Uuid& uuid, int type, double x, double y, double z, float pitch, float yaw, float headYaw, int data = 0);
Bytes removeEntities(std::span<const int> ids);
Bytes setEntityData(int id, std::span<const EntityData> values);
Bytes setEquipment(int id, std::span<const Equipment> slots);
Bytes swingAnimation(int id, bool offhand);
Bytes animate(int id, std::uint8_t action);
Bytes hurtAnimation(int id, float yaw);
Bytes entityEvent(int id, std::uint8_t event);
Bytes takeItemEntity(int item, int collector, int count);
Bytes setPassengers(int vehicle, std::span<const int> riders);
Bytes setHealth(float health, int food, float saturation);
Bytes setExperience(float progress, int level, int total);
Bytes updateMobEffect(int id, int effect, int amplifier, int duration, std::uint8_t flags);
Bytes removeMobEffect(int id, int effect);
Bytes playerInfoUpdate(std::uint8_t actions, std::span<const PlayerInfo> entries);
Bytes playerInfoRemove(std::span<const Uuid> uuids);
Bytes systemChat(const Component& component, bool overlay);
Bytes setTitleText(const Component& component);
Bytes setSubtitleText(const Component& component);
Bytes setActionBarText(const Component& component);
Bytes setTitlesAnimation(int in, int stay, int out);
Bytes clearTitles(bool reset);
Bytes bossEventAdd(const Uuid& uuid, const Component& title, float progress, int colour, int overlay, std::uint8_t flags);
Bytes bossEventRemove(const Uuid& uuid);
Bytes bossEventProgress(const Uuid& uuid, float progress);
Bytes bossEventName(const Uuid& uuid, const Component& title);
Bytes bossEventStyle(const Uuid& uuid, int colour, int overlay);
Bytes bossEventProperties(const Uuid& uuid, std::uint8_t flags);
Bytes gameEvent(std::uint8_t event, float value);
Bytes levelEvent(int type, int x, int y, int z, int data, bool global);
Bytes levelParticles(int particle, double x, double y, double z, int count);
Bytes sound(int soundId, int source, double x, double y, double z, float volume, float pitch, std::uint64_t seed);
Bytes blockEvent(int x, int y, int z, std::uint8_t action, std::uint8_t param, int block);
Bytes blockEntityData(int x, int y, int z, int type, const Bytes& nbt);
Bytes containerSetSlot(int slot, const ItemStack& item);
Bytes setHeldSlot(int slot);
struct Login {
    Dimension dim = Dimension::Overworld;
    int playerId = 1, maxPlayers = 1, chunkRadius = 32, simulationDistance = 12;
    std::uint64_t hashedSeed = 0;
    int gamemode = 0, previousGamemode = -1;
};
Bytes login(const Login& options);
Bytes playerInfoAdd(const Uuid& uuid, std::string_view name, int gamemode);
Bytes setTime(std::uint64_t gameTime, std::uint64_t dayTicks);
Bytes gameEventWaitChunks();
Bytes blockUpdate(int x, int y, int z, int state);
Bytes sectionBlocksUpdate(int sx, int sy, int sz, std::span<const BlockChange> changes);
void palettedContainer(ByteWriter& out, std::span<const std::uint16_t> values, bool biome);
Bytes levelChunkWithLight(const Column& column);
Bytes createLocalPlayer(const Uuid& uuid, std::string_view name, int gamemode, const PlayerPosition& pos);
Bytes moveEntities(std::string_view dimKey, int id, double x, double y, double z,
                   float yaw, float pitch, float headYaw, bool onGround);
Bytes moveEntities(std::string_view dimKey, std::span<const EntityPose> entities);
}
}
