#pragma once

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

    bool playerFeetY(float& outY) const;

    bool playerFeet(float& outX, float& outY, float& outZ) const;

    static constexpr float kEyeHeight = 1.62f;

    void* playerComponent(unsigned int typeId, std::size_t stride) const;

    bool isPlayerEntity(const void* entityContext) const;

    static constexpr std::ptrdiff_t kPlayerPositionOffset = 0x594;

private:
    GameData() = default;

    mutable std::mutex m_mutex;
    PlayerView m_view;
    bool m_valid = false;

    std::atomic<void*> m_gameMode{nullptr};

    std::atomic<void*> m_player{nullptr};
};

}
