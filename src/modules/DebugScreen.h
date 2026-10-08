#pragma once

#include "game/DebugScreenLayout.h"
#include "game/DebugScreenText.h"
#include "game/LevelDbIndex.h"
#include "input/Hotkey.h"
#include "modules/Module.h"

#include <mutex>
#include <atomic>
#include <cstdint>

namespace tsukuyomi {

class DebugScreen : public Module {
public:
    static DebugScreen& instance();
    static bool localServerRunning();

    const wchar_t* name() const override { return L"DebugScreen"; }
    bool available() const override;

    MenuItem buildMenu() override;
    void loadConfig(const nlohmann::json& section) override;
    void saveConfig(nlohmann::json& section) const override;

    void onScansReady() override;
    void shutdown() override;

    static void onFrameRendered();
    static void onServerTick(long long qpcTicks);
    static void onPacketSent();
    static void onPacketReceived();
    static void onStartGame(const char* version);
    static std::uint64_t worldEntry() { return s_startGameEntry.load(std::memory_order_acquire); }
    static void onChunkTicked();
    static void onChunkTickHookReady();

    static bool wantsSounds() { return s_wantSounds.load(std::memory_order_relaxed); }
    static bool soundSampleDue();
    static void onSounds(int statics, int streams, int cap, int loadedSounds, long long memoryBytes);
    static bool wantsParticles() { return s_wantParticles.load(std::memory_order_relaxed); }
    static void onParticles(int total);
    static bool wantsChunkRender() { return s_wantChunkRender.load(std::memory_order_relaxed); }
    static void onBuilderStats(int pending, int freeBuffers);

    void onPlayerViewUpdate();
    static constexpr unsigned kKeyLocation = 1u;
    static constexpr unsigned kKeyData = 2u;
    void requestKeySample(unsigned flags) { m_keySampleRequests.fetch_or(flags, std::memory_order_release); }
    unsigned takeKeySampleFlags() { return m_keySampleReady.exchange(0, std::memory_order_acq_rel); }
    std::uint64_t keySampleGeneration() const { return m_keySampleGeneration.load(std::memory_order_acquire); }
    void fillVersionInfo(dbgtext::Sample& out) const;

    bool overlayOpen() const { return m_overlayOpen.load(std::memory_order_relaxed); }
    dbgscreen::ElementState stateOf(int element) const;
    void setStateOf(int element, dbgscreen::ElementState state);
    void applyProfile(bool performance);

    bool hideCoordinates() const { return m_hideCoords.load(std::memory_order_relaxed); }
    bool hideDaysPlayed() const { return m_hideDays.load(std::memory_order_relaxed); }
    bool chatBottomLeft() const { return m_chatBottom.load(std::memory_order_relaxed); }
    void setHideCoordinates(bool on);
    void setHideDaysPlayed(bool on);
    void setChatBottomLeft(bool on);

protected:
    void onUpdate() override;
    void onEnabledChanged(bool enabled) override;

private:
    DebugScreen() = default;

    static void onHudCreated(void* ctrl);

    void publish();
    void fillSample(dbgtext::Sample& out);
    void hideAllRows();
    void publishHudOptions();
    bool sampleFps(int& outFps);
    void sampleNetwork();
    bool elementShown(int element) const;
    bool anyShown() const;

    static constexpr int kDefaultOverlayKey = 0x72;
    Hotkey m_overlayKey{{kDefaultOverlayKey}};
    std::atomic<bool> m_overlayOpen{false};

    std::atomic<int> m_states[dbgscreen::kElementCount]{};
    std::atomic<bool> m_definitionRegistered{false};
    std::atomic<bool> m_shuttingDown{false};
    std::atomic<unsigned> m_keySampleRequests{0};
    std::atomic<unsigned> m_keySampleReady{0};
    std::atomic<std::uint64_t> m_keySampleGeneration{0};
    std::atomic<bool> m_hideCoords{false};
    std::atomic<bool> m_hideDays{false};
    std::atomic<bool> m_chatBottom{false};
    bool m_enabledWasInConfig = false;

    static std::atomic<std::uint64_t> s_frames;
    static std::atomic<std::uint64_t> s_serverTicks;
    static std::atomic<std::uint64_t> s_serverTickQpc;
    static std::atomic<std::uint64_t> s_packetsSent;
    static std::atomic<std::uint64_t> s_packetsReceived;
    static std::atomic<std::uint64_t> s_chunkTicks;
    static std::atomic<bool> s_chunkTickHookReady;
    static std::atomic<std::uint64_t> s_startGameEntry;
    static std::atomic<std::uint64_t> s_versionEntry;
    static std::atomic<char> s_serverVersion[32];
    static std::atomic<bool> s_wantSounds;
    static std::atomic<bool> s_wantParticles;
    static std::atomic<std::uint64_t> s_soundSampleTick;
    static std::atomic<int> s_soundStatics;
    static std::atomic<int> s_soundStreams;
    static std::atomic<int> s_soundCap;
    static std::atomic<std::uint64_t> s_soundTick;
    static std::atomic<int> s_particleTotal;
    static std::atomic<std::uint64_t> s_particleTick;
    static std::atomic<int> s_soundLoaded;
    static std::atomic<long long> s_soundMemory;
    static std::atomic<bool> s_wantChunkRender;
    static std::atomic<int> s_builderPending;
    static std::atomic<int> s_builderFree;
    static std::atomic<std::uint64_t> s_builderTick;
    const std::int32_t* m_renderedActors = nullptr;
    std::uint64_t m_framesAtMark = 0;
    std::uint64_t m_markTick = 0;
    int m_fps = -1;

    std::uint64_t m_lastPublishTick = 0;
    std::mutex m_publishMutex;
    leveldb::WorldIndex m_worldIndex;
    std::atomic<std::uint32_t> m_mainThreadId{0};
    std::atomic<bool> m_allowDbRead{false};
    bool m_loggedDbFound = false, m_loggedDbMissing = false, m_loggedManifestUnreadable = false;
    bool m_loggedMultipleManifestHandles = false;
    char m_lines[dbgscreen::kElementCount][dbgscreen::kMaxRowsPerElement][dbgtext::kLineBytes]{};
    dbgscreen::Contribution m_items[dbgscreen::kElementCount]{};
    dbgscreen::Arranged m_arranged{};

    std::uint64_t m_optionsTick = 0;
    int m_gpuPercent = -1;
    std::uint64_t m_gpuTick = 0;
    double m_tickMs = -1.0;
    int m_txPerSecond = -1;
    int m_rxPerSecond = -1;
    int m_tickingChunks = -1;
    std::uint64_t m_netMarkTick = 0;
    std::uint64_t m_networkEntry = 0;
    std::uint64_t m_ticksAtMark = 0;
    std::uint64_t m_tickQpcAtMark = 0;
    std::uint64_t m_sentAtMark = 0;
    std::uint64_t m_receivedAtMark = 0;
    std::uint64_t m_chunkTicksAtMark = 0;
};

}
