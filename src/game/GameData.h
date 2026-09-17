#pragma once

#include <string>

#include <atomic>
#include <cstddef>
#include <mutex>

namespace tsukuyomi {

struct PlayerView {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float pitch = 0.0f;
    float yaw = 0.0f;
};

class GameData {
public:
    static GameData& instance();

    void setPlayerView(const PlayerView& view);
    PlayerView playerView() const;
    bool hasPlayerView() const;

    float yaw() const;

    void setGameMode(void* gameMode);
    void* gameMode() const;

    void setPlayer(void* player);
    void* player() const;

    void setPlayerAlt(void* player);
    void* playerAlt() const;

    bool playerFeetY(float& outY) const;

    bool playerFeet(float& outX, float& outY, float& outZ) const;

    bool rawPlayerPos(float& outX, float& outY, float& outZ) const;
    bool writeRawPlayerPos(float x, float y, float z);

    bool hasLivePlayer() const;
    bool findPlayerFromClient(void* clientInstance);

    bool adoptPlayerFromEntity(void* entityContext);

    int players(void* out[2]) const;
    static bool rawPosOf(const void* player, float& outX, float& outY, float& outZ);
    static bool targetPosOf(const void* player, float& outX, float& outY, float& outZ);
    static bool writeRawPosOf(void* player, float x, float y, float z);

    static constexpr float kEyeHeight = 1.62f;

    void* playerComponent(unsigned int typeId, std::size_t stride) const;

    bool isPlayerEntity(const void* entityContext) const;

    static constexpr std::ptrdiff_t kPlayerPositionOffset = 0x594;

    static constexpr std::ptrdiff_t kPlayerTargetPosOffset = 0x1330;
    static constexpr std::ptrdiff_t kPlayerTargetPosSize = 24;

    static constexpr std::ptrdiff_t kPlayerTargetArgOffset = 0x1320;

private:
    GameData() = default;

    mutable std::mutex m_mutex;
    PlayerView m_view;
    bool m_valid = false;

    std::atomic<void*> m_gameMode{nullptr};

    std::atomic<void*> m_player{nullptr};
    std::atomic<void*> m_playerAlt{nullptr};
    std::atomic<unsigned long long> m_adoptAt{0};
};

}
