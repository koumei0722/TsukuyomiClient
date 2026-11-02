#pragma once
#include "replay/JavaPackets.h"
#include <map>
#include <optional>
#include <string>
#include <variant>

namespace tsukuyomi::replay::bedrock {
struct Vec3 { float x = 0, y = 0, z = 0; bool operator==(const Vec3&) const = default; };
struct BlockPos { std::int32_t x = 0, y = 0, z = 0; bool operator==(const BlockPos&) const = default; };
struct Nbt {
    std::uint8_t type = 0;
    std::int64_t integer = 0;
    double real = 0;
    std::string text;
    Bytes bytes;
    std::vector<Nbt> list;
    std::vector<std::pair<std::string, Nbt>> children;
    const Nbt* find(std::string_view key) const;
    bool operator==(const Nbt&) const = default;
};
using MetaValue = std::variant<std::uint8_t, std::int16_t, std::int32_t, float, std::string, Nbt, BlockPos, std::int64_t, Vec3>;
using Metadata = std::map<std::uint32_t, MetaValue>;
struct ItemInstance {
    std::int16_t networkId = 0;
    std::uint16_t count = 0;
    std::uint32_t metadata = 0, blockRuntimeId = 0;
    std::optional<Nbt> userData;
    bool empty() const { return networkId == 0 || count == 0; }
    bool operator==(const ItemInstance&) const = default;
};
struct Attribute { std::string name; float value = 0, min = 0, max = 0, defaultValue = 0; };
struct EntityLink { std::int64_t ridden = 0, rider = 0; std::uint8_t type = 0; bool immediate = false, riderInitiated = false; float angularVelocity = 0; };
struct Text { bool needsTranslation = false; std::uint8_t type = 0; std::string source, message; std::vector<std::string> parameters; };
struct SetTime { std::int32_t time = 0; };
struct StartGameHead { std::int64_t uniqueId = 0; std::uint64_t runtimeId = 0; int gameMode = 0; };
struct AddActor {
    std::int64_t uniqueId = 0; std::uint64_t runtimeId = 0;
    std::string type; Vec3 position, velocity;
    float pitch = 0, yaw = 0, headYaw = 0, bodyYaw = 0;
    std::vector<Attribute> attributes; Metadata metadata; std::vector<EntityLink> links;
};
struct AddPlayer {
    Uuid uuid{}; std::string name; std::uint64_t runtimeId = 0; std::int64_t uniqueId = 0;
    Vec3 position, velocity; float pitch = 0, yaw = 0, headYaw = 0;
    ItemInstance heldItem; int gameMode = 0; Metadata metadata; std::vector<EntityLink> links;
};
struct AddItemActor { std::int64_t uniqueId = 0; std::uint64_t runtimeId = 0; ItemInstance item; Vec3 position, velocity; Metadata metadata; bool fromFishing = false; };
struct RemoveActor { std::int64_t uniqueId = 0; };
struct TakeItemActor { std::uint64_t itemId = 0, takerId = 0; };
struct MoveActorAbsolute { std::uint64_t runtimeId = 0; std::uint8_t flags = 0; Vec3 position; float pitch = 0, yaw = 0, headYaw = 0; };
struct MovePlayer { std::uint64_t runtimeId = 0; Vec3 position; float pitch = 0, yaw = 0, headYaw = 0; std::uint8_t mode = 0; bool onGround = false; std::uint64_t riddenId = 0, tick = 0; };
struct MoveActorDelta { std::uint64_t runtimeId = 0; std::optional<float> x, y, z, pitch, yaw, headYaw; bool onGround = false; std::uint64_t tick = 0; };
struct LevelEvent { int type = 0; Vec3 position; int data = 0; };
struct BlockEvent { BlockPos position; int type = 0, data = 0; };
struct ActorEvent { std::uint64_t runtimeId = 0; std::uint8_t type = 0; int data = 0; std::optional<Vec3> fireAt; };
struct MobEffect { std::uint64_t runtimeId = 0; std::uint8_t operation = 0; int effect = 0, amplifier = 0; bool particles = false; int duration = 0; std::uint64_t tick = 0; bool ambient = false; };
struct UpdateAttributes { std::uint64_t runtimeId = 0; std::vector<Attribute> attributes; std::uint64_t tick = 0; };
struct MobEquipment { std::uint64_t runtimeId = 0; ItemInstance item; std::uint8_t inventorySlot = 0, hotBarSlot = 0, window = 0; };
struct MobArmourEquipment { std::uint64_t runtimeId = 0; ItemInstance helmet, chestplate, leggings, boots, body; };
struct SetActorData { std::uint64_t runtimeId = 0; Metadata metadata; std::uint64_t tick = 0; };
struct SetActorLink { EntityLink link; };
struct SetHealth { int health = 0; };
struct Animate { std::uint8_t action = 0; std::uint64_t runtimeId = 0; float data = 0; std::optional<std::string> source; std::uint8_t hand = 0; };
struct PlayerHotBar { std::uint32_t selected = 0; std::uint8_t window = 0; bool select = false; };
struct ContainerName { std::uint8_t id = 0; std::optional<std::uint32_t> dynamicId; };
struct InventoryContent { std::uint32_t window = 0; std::vector<ItemInstance> items; ContainerName container; ItemInstance storage; };
struct InventorySlot { std::uint32_t window = 0, slot = 0; std::optional<ContainerName> container; std::optional<ItemInstance> storage; ItemInstance item; };
struct BlockActorData { BlockPos position; Nbt nbt; };
struct ChangeDimension { int dimension = 0; Vec3 position; bool respawn = false; std::optional<std::uint32_t> loadingScreen; };
struct SetPlayerGameType { int gameMode = 0; };
struct UpdatePlayerGameType { int gameMode = 0; std::int64_t uniqueId = 0; std::uint64_t tick = 0; };
struct PlayerListEntry { bool add = true; Uuid uuid{}; std::int64_t uniqueId = 0; std::string name, xuid; int buildPlatform = 0; };
struct PlayerList { std::vector<PlayerListEntry> entries; };
struct BossEvent { std::int64_t uniqueId = 0; std::uint8_t type = 0; std::string title, filteredTitle; float health = 0; std::uint8_t colour = 0, overlay = 0; int screenDarkening = 0; };
struct PlaySound { std::string name; Vec3 position; float volume = 0, pitch = 0; };
struct SetTitle { int action = 0; std::string text; int fadeIn = 0, stay = 0, fadeOut = 0; };
struct SpawnParticleEffect { std::uint8_t dimension = 0; std::int64_t uniqueId = 0; Vec3 position; std::string name; };
struct LevelSoundEvent { std::string sound; Vec3 position; int extraData = 0; std::string entity; bool baby = false, disableRelativeVolume = false; std::int64_t uniqueId = 0; std::optional<Vec3> fireAt; };
struct PlayerAuthInputHead { float pitch = 0, yaw = 0; Vec3 position; float headYaw = 0; std::vector<int> flags; };
struct ItemRegistry { std::vector<std::pair<int, std::string>> items; };
using Packet = std::variant<Text, SetTime, StartGameHead, AddPlayer, AddActor, RemoveActor, AddItemActor, TakeItemActor,
    MoveActorAbsolute, MovePlayer, LevelEvent, BlockEvent, ActorEvent, MobEffect, UpdateAttributes, MobEquipment,
    MobArmourEquipment, SetActorData, SetActorLink, SetHealth, Animate, PlayerHotBar, InventoryContent, InventorySlot,
    BlockActorData, ChangeDimension, SetPlayerGameType, PlayerList, BossEvent, PlaySound, SetTitle, MoveActorDelta,
    SpawnParticleEffect, LevelSoundEvent, UpdatePlayerGameType, ItemRegistry, PlayerAuthInputHead>;
struct Received { bool sent = false; Packet packet; };
bool wanted(std::uint16_t id, bool sent);
std::optional<Packet> parse(std::uint16_t id, std::span<const std::uint8_t> body, bool sent);
void setShieldNetworkId(int id);
}
