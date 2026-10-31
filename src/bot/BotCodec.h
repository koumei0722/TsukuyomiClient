#pragma once
#include "BotProtocol.h"
#include "GuestLogin.h"
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tsukuyomi::bot {

struct Item {
    std::int32_t id = 0;
    std::uint16_t count = 0;
    std::int32_t netId = 0;
    std::shared_ptr<const void> handle;
    bool empty() const { return id == 0 || count == 0; }
};

struct UseItem {
    std::int32_t action = 1;
    std::int32_t trigger = 1;
    std::int32_t x = 0, y = 0, z = 0;
    std::int32_t face = 0;
    std::int32_t hotbarSlot = 0;
    Item held;
    protocol::Vec3 position, clicked;
    std::uint32_t blockRuntimeId = 0;
    std::uint8_t prediction = 1;
};
struct ReleaseItem {
    std::int32_t action = 0;
    std::int32_t hotbarSlot = 0;
    Item held;
    protocol::Vec3 headPosition;
};

struct MobEquipment { std::uint64_t runtimeId = 0; std::uint8_t slot = 0, hotbarSlot = 0, window = 0; };
struct PlayerHotbar { std::uint32_t slot = 0; std::uint8_t window = 0; bool select = false; };
struct InventoryContent { std::uint32_t window = 0; std::vector<Item> items; };
struct InventorySlot { std::uint32_t window = 0, slot = 0; Item item; };

struct StackSlot { std::uint8_t slot = 0, count = 0; std::int32_t netId = 0; };
struct StackContainer { std::uint8_t container = 0; std::vector<StackSlot> slots; };
struct StackResponse { std::uint8_t status = 0; std::int32_t requestId = 0; std::vector<StackContainer> containers; };

class BotCodec {
public:
    virtual ~BotCodec() = default;
    virtual std::string subClientLogin(std::uint8_t sub, const guest::Login& login) = 0;
    virtual std::string requestChunkRadius(std::uint8_t sub, std::int32_t radius, std::uint8_t maxRadius) = 0;
    virtual std::string setLocalPlayerAsInitialized(std::uint8_t sub, std::uint64_t runtimeId) = 0;
    virtual std::string networkStackLatencyReply(std::uint8_t sub, protocol::Bytes request) = 0;
    virtual std::string disconnect(std::uint8_t sub) = 0;
    virtual std::string clientCacheBlobHits(std::uint8_t sub, std::span<const std::uint64_t> hits) = 0;
    virtual std::string subChunkRequest(std::uint8_t sub, std::int32_t dimension, std::int32_t x, std::int32_t y, std::int32_t z,
                                        std::span<const std::array<std::int8_t, 3>> offsets) = 0;
    virtual std::string playerAuthInput(std::uint8_t sub, const protocol::AuthInput& input) = 0;
    virtual std::string respawn(std::uint8_t sub, protocol::Vec3 position, std::uint8_t state, std::uint64_t runtimeId) = 0;
    virtual std::string playerAction(std::uint8_t sub, std::uint64_t runtimeId, std::int32_t action) = 0;
    virtual std::string loadingScreen(std::uint8_t sub, std::int32_t type, std::optional<std::uint32_t> screenId) = 0;
    virtual std::string animateSwing(std::uint8_t sub, std::uint64_t runtimeId) = 0;
    virtual std::string useItem(std::uint8_t sub, const UseItem& value) = 0;
    virtual std::string releaseItem(std::uint8_t sub, const ReleaseItem& value) = 0;
    virtual std::string mobEquipment(std::uint8_t, std::uint64_t, const Item&, std::uint8_t) { return {}; }
    virtual std::string interact(std::uint8_t, std::uint8_t, std::uint64_t) { return {}; }
    virtual std::string containerClose(std::uint8_t, std::uint8_t, std::uint8_t, bool) { return {}; }
    virtual std::optional<std::vector<StackResponse>> readItemStackResponse(protocol::Bytes) { return std::nullopt; }
    virtual std::optional<std::int32_t> readPlayStatus(protocol::Bytes) = 0;
    virtual std::optional<protocol::StartGame> readStartGame(protocol::Bytes) = 0;
    virtual std::optional<std::int32_t> readChunkRadiusUpdated(protocol::Bytes) = 0;
    virtual std::optional<bool> readNetworkStackLatencyNeedsResponse(protocol::Bytes) = 0;
    virtual std::optional<protocol::Disconnect> readDisconnect(protocol::Bytes) = 0;
    virtual std::optional<protocol::MovePlayer> readMovePlayer(protocol::Bytes) = 0;
    virtual std::optional<protocol::Correction> readCorrectPlayerMovePrediction(protocol::Bytes) = 0;
    virtual std::optional<protocol::Respawn> readRespawn(protocol::Bytes) = 0;
    virtual std::optional<protocol::ChangeDimension> readChangeDimension(protocol::Bytes) = 0;
    virtual std::optional<std::vector<std::uint64_t>> readLevelChunkBlobs(protocol::Bytes) = 0;
    virtual std::optional<MobEquipment> readMobEquipment(protocol::Bytes) = 0;
    virtual std::optional<PlayerHotbar> readPlayerHotbar(protocol::Bytes) = 0;
    virtual std::optional<InventoryContent> readInventoryContent(protocol::Bytes) = 0;
    virtual std::optional<InventorySlot> readInventorySlot(protocol::Bytes) = 0;
    virtual std::uint32_t blockRuntimeId(std::int32_t, std::int32_t, std::int32_t) { return 0; }
};

BotCodec& codec();
void setCodec(BotCodec* value);

}
