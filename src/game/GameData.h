#pragma once

#include <atomic>
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

    static constexpr float kEyeHeight = 1.62f;

private:
    GameData() = default;

    mutable std::mutex m_mutex;
    PlayerView m_view;
    bool m_valid = false;

    std::atomic<void*> m_gameMode{nullptr};
};

}
