#include "BotCodec.h"

namespace tsukuyomi::bot {
namespace {
class NullCodec final : public BotCodec {
public:
    std::string subClientLogin(std::uint8_t, const guest::Login&) override { return {}; }
    std::string requestChunkRadius(std::uint8_t, std::int32_t, std::uint8_t) override { return {}; }
    std::string setLocalPlayerAsInitialized(std::uint8_t, std::uint64_t) override { return {}; }
    std::string networkStackLatencyReply(std::uint8_t, protocol::Bytes) override { return {}; }
    std::string disconnect(std::uint8_t) override { return {}; }
    std::string clientCacheBlobHits(std::uint8_t, std::span<const std::uint64_t>) override { return {}; }
    std::string subChunkRequest(std::uint8_t, std::int32_t, std::int32_t, std::int32_t, std::int32_t, std::span<const std::array<std::int8_t, 3>>) override { return {}; }
    std::string playerAuthInput(std::uint8_t, const protocol::AuthInput&) override { return {}; }
    std::string respawn(std::uint8_t, protocol::Vec3, std::uint8_t, std::uint64_t) override { return {}; }
    std::string playerAction(std::uint8_t, std::uint64_t, std::int32_t) override { return {}; }
    std::string loadingScreen(std::uint8_t, std::int32_t, std::optional<std::uint32_t>) override { return {}; }
    std::string animateSwing(std::uint8_t, std::uint64_t) override { return {}; }
    std::string useItem(std::uint8_t, const UseItem&) override { return {}; }
    std::string releaseItem(std::uint8_t, const ReleaseItem&) override { return {}; }
    std::optional<std::int32_t> readPlayStatus(protocol::Bytes) override { return std::nullopt; }
    std::optional<protocol::StartGame> readStartGame(protocol::Bytes) override { return std::nullopt; }
    std::optional<std::int32_t> readChunkRadiusUpdated(protocol::Bytes) override { return std::nullopt; }
    std::optional<bool> readNetworkStackLatencyNeedsResponse(protocol::Bytes) override { return std::nullopt; }
    std::optional<protocol::Disconnect> readDisconnect(protocol::Bytes) override { return std::nullopt; }
    std::optional<protocol::MovePlayer> readMovePlayer(protocol::Bytes) override { return std::nullopt; }
    std::optional<protocol::Correction> readCorrectPlayerMovePrediction(protocol::Bytes) override { return std::nullopt; }
    std::optional<protocol::Respawn> readRespawn(protocol::Bytes) override { return std::nullopt; }
    std::optional<protocol::ChangeDimension> readChangeDimension(protocol::Bytes) override { return std::nullopt; }
    std::optional<std::vector<std::uint64_t>> readLevelChunkBlobs(protocol::Bytes) override { return std::nullopt; }
    std::optional<MobEquipment> readMobEquipment(protocol::Bytes) override { return std::nullopt; }
    std::optional<PlayerHotbar> readPlayerHotbar(protocol::Bytes) override { return std::nullopt; }
    std::optional<InventoryContent> readInventoryContent(protocol::Bytes) override { return std::nullopt; }
    std::optional<InventorySlot> readInventorySlot(protocol::Bytes) override { return std::nullopt; }
};
NullCodec g_null;
BotCodec* g_codec = &g_null;
}
BotCodec& codec() { return *g_codec; }
void setCodec(BotCodec* value) { g_codec = value ? value : &g_null; }
}
