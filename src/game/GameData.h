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

    unsigned long long msSinceView() const;

    void setGameMode(void* gameMode);
    void* gameMode() const;

    void setPlayer(void* player);
    void* player() const;
    unsigned long long playerSerial() const;

    bool setPlayerAlt(void* player);
    void* playerAlt() const;

    void onScansReady();
    bool knowsServerPlayer() const;
    bool isServerPlayer(const void* player) const;
    const void* serverPlayerVtable() const { return m_serverPlayerVtable.load(std::memory_order_acquire); }
    const void* playerVtable() const { return m_playerVtable.load(std::memory_order_acquire); }

    bool playerFeet(float& outX, float& outY, float& outZ) const;

    bool playerBoxFeet(float& outX, float& outY, float& outZ) const;

    static constexpr unsigned int kAabbShapeTypeId = 0xBAC1B3CFu;
    static constexpr std::size_t kAabbShapeStride = 0x20;

    bool rawPlayerPos(float& outX, float& outY, float& outZ) const;

    bool hasLivePlayer() const;
    bool findPlayerFromClient(void* clientInstance);

    bool adoptPlayerFromEntity(void* entityContext);

    static bool rawPosOf(const void* player, float& outX, float& outY, float& outZ);
    static bool previousPosOf(const void* player, float& outX, float& outY, float& outZ);

    static constexpr float kEyeHeight = 1.62f;

    bool isPlayerEntity(const void* entityContext) const;

    static constexpr std::ptrdiff_t kPlayerPositionOffset = 0x594;
    static constexpr std::ptrdiff_t kPlayerPreviousPositionOffset = 0x588;

private:
    GameData() = default;

    mutable std::mutex m_mutex;
    PlayerView m_view;
    bool m_valid = false;

    std::atomic<void*> m_gameMode{nullptr};

    std::atomic<void*> m_player{nullptr};
    mutable std::atomic<void*> m_playerAlt{nullptr};
    std::atomic<const void*> m_serverPlayerVtable{nullptr};
    std::atomic<const void*> m_playerVtable{nullptr};
    std::atomic<int> m_playerAltLogs{0};
    std::atomic<unsigned long long> m_playerSerial{0};
    std::atomic<unsigned long long> m_adoptAt{0};
    std::atomic<unsigned long long> m_viewAt{0};
};

}
