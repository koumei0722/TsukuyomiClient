#pragma once
#include "BotProtocol.h"
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace tsukuyomi::bot::possess {

using Uuid = std::array<std::uint8_t, 16>;
struct Ids {
    std::uint64_t runtimeA = 0, runtimeB = 0;
    std::int64_t uniqueA = 0, uniqueB = 0;
    bool active = false;
    Uuid uuidA{}, uuidB{};
    bool uuids = false;
};

std::optional<std::string> rewrite(std::string_view packet, const Ids& ids, std::uint8_t recipient, std::uint8_t sender,
                                   std::int64_t tickDelta);

struct AuthInputHead { float pitch = 0, yaw = 0; protocol::Vec3 position; std::uint64_t tick = 0; };
std::optional<AuthInputHead> readAuthInputHead(protocol::Bytes body);

struct StartGameHead { std::int64_t uniqueId = 0; std::uint64_t runtimeId = 0; std::int32_t gameMode = 0; protocol::Vec3 position; float pitch = 0, yaw = 0;
                       std::optional<std::int32_t> dimension; };
std::optional<StartGameHead> readStartGameHead(protocol::Bytes body);

class StateKeeper {
public:
    static bool wants(std::uint16_t id);
    void reset();
    void setSelf(std::uint64_t runtimeId) { selfRuntime_ = runtimeId; }
    void observe(std::uint16_t id, protocol::Bytes body);
    std::vector<std::string> chunksNear(std::int32_t chunkX, std::int32_t chunkZ, std::int32_t dimension, std::int32_t radius) const;
    static constexpr std::int32_t kChunkMargin = 4;
    std::optional<Uuid> playerUuid(std::uint64_t runtimeId) const;
    std::optional<std::string> playerName(std::uint64_t runtimeId) const;
    std::optional<Uuid> listedUuid(std::int64_t uniqueId) const;
private:
    struct Entity {
        std::uint64_t runtimeId = 0;
        std::uint16_t spawnId = 0;
        std::string spawn;
    };
    std::uint64_t selfRuntime_ = 0;
    std::map<std::int64_t, Entity> entities_;
    std::map<std::uint64_t, std::int64_t> runtimeToUnique_;
    std::map<std::int64_t, Uuid> listed_;
    std::map<std::tuple<std::int32_t, std::int32_t, std::int32_t>, std::string> chunks_;
    void dropChunksOutside(std::int32_t chunkX, std::int32_t chunkZ, std::int32_t radius);
};

}
