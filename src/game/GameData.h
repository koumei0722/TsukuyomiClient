#pragma once

#include <string>

#include <atomic>
#include <cstddef>
#include <cstdint>
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
    static void* componentAddress(const void* actor, std::uint32_t typeId, std::size_t stride);
    static bool copyComponent(const void* actor, std::uint32_t typeId, std::size_t stride, void* out);

    void setPlayerView(const PlayerView& view);
    PlayerView playerView() const;
    bool hasPlayerView() const;

    unsigned long long msSinceView() const;
    void noteClientSession();
    unsigned long long msSinceClientSession() const;

    void setGameMode(void* gameMode);
    void* gameMode() const;
    void setPlayerGameModeOffset(std::size_t offset);

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

    bool onLevelTick(const void* level, unsigned long thread, unsigned long clientThread);
    bool integratedServer() const;

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

    static constexpr std::ptrdiff_t kPlayerPositionOffset = 0x5BC;
    static constexpr std::ptrdiff_t kPlayerPreviousPositionOffset = 0x5B0;

private:
    GameData() = default;

    mutable std::mutex m_mutex;
    PlayerView m_view;
    bool m_valid = false;

    std::atomic<void*> m_gameMode{nullptr};
    std::atomic<void*> m_droppedGameMode{nullptr};
    std::atomic<std::size_t> m_playerGameModeOffset{0};

    std::atomic<void*> m_player{nullptr};
    mutable std::atomic<void*> m_playerAlt{nullptr};
    std::atomic<const void*> m_serverPlayerVtable{nullptr};
    std::atomic<const void*> m_playerVtable{nullptr};
    std::atomic<int> m_playerAltLogs{0};
    std::atomic<unsigned long long> m_playerSerial{0};
    std::atomic<unsigned long long> m_adoptAt{0};
    std::atomic<unsigned long long> m_viewAt{0};
    std::atomic<unsigned long long> m_clientSessionAt{0};
    std::atomic<const void*> m_tickLevels[2]{};
    std::atomic<unsigned long long> m_tickLevelAt[2]{};
    std::atomic<unsigned long long> m_tickLevelSince[2]{};
    std::atomic<int> m_tickLevelLogs{0};
};

}
