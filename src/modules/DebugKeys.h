#pragma once

#include "input/Hotkey.h"
#include "game/DebugLines.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <vector>

namespace tsukuyomi {

class DebugKeys {
public:
    static DebugKeys& instance();

    void onScansReady();
    void onUpdate();
    void onEnabledChanged(bool enabled);
    void shutdown();
    void loadKeys(const nlohmann::json& section);
    void saveKeys(nlohmann::json& section) const;
    void onPlayerViewUpdate();
    void onItemHoverText(const void* stack, void* out);
    bool pausedByKey() const { return m_pausedByKey.load(std::memory_order_acquire); }

    void selectGameMode(int mode) { m_selectedMode.store(mode, std::memory_order_release); }
    void onPauseWheelClosed() { m_pauseWheelClosed.store(true, std::memory_order_release); }

private:
    DebugKeys() = default;
    static bool active();

    static constexpr int kF3 = 0x72;
    Hotkey m_locationKey{{kF3, 'C'}};
    Hotkey m_versionKey{{kF3, 'V'}};
    Hotkey m_dataKey{{kF3, 'I'}};
    Hotkey m_pauseKey{{kF3, 'P'}};
    Hotkey m_advancedKey{{kF3, 'H'}};
    Hotkey m_chunksKey{{kF3, 'A'}};
    Hotkey m_hitboxKey{{kF3, 'B'}};
    Hotkey m_chunkBorderKey{{kF3, 'G'}};
    Hotkey m_clearChatKey{{kF3, 'D'}};
    Hotkey m_pauseGameKey{{kF3, 0x1B}};
    Hotkey m_gameModeKey{{kF3, 0x73}};
    Hotkey m_spectatorKey{{kF3, 'N'}};
    std::vector<int> m_reloadCombo{kF3, 'T'};
    Hotkey m_reloadKey{{kF3, 'T'}, "button.reload_ui_definitions"};
    void applyReloadKey();
    std::atomic<unsigned> m_actions{0};
    std::atomic<bool> m_advancedShown{false};
    std::atomic<bool> m_hitboxesShown{false};
    std::atomic<bool> m_chunkBordersShown{false};
    bool m_linesPublished = false;
    std::atomic<bool> m_pausedByKey{false};
    std::atomic<bool> m_pauseWheelClosed{false};
    std::atomic<bool> m_stopped{false};
    void* m_viewPerspective = nullptr;
    int m_perspective = 0;
    bool m_perspectiveKnown = false;
    int m_perspectiveUnknownFrames = 0;
    std::atomic<bool> m_scansReady{false};
    std::uint64_t m_lastViewAt = 0;
    std::int32_t m_guiDataField = 0;
    void* m_clearMessages = nullptr;
    void* m_pauseGameA = nullptr;
    void* m_chatCommand = nullptr;
    void* m_pauseGameB = nullptr;
    std::atomic<int> m_selectedMode{-1};
    void* m_lastPlayer = nullptr;
    debuglines::HitBoxTrack m_hitBoxTrack;
    std::atomic<unsigned> m_samplePending{0};
    std::atomic<std::uint64_t> m_locationAt{0};
    std::atomic<std::uint64_t> m_dataAt{0};
    std::atomic<std::uint64_t> m_locationSampleSeen{0};
    std::atomic<std::uint64_t> m_dataSampleSeen{0};
    std::int32_t m_clientOptionsSlot = 0;
    std::int32_t m_optionLookupSlot = 0;
    std::int32_t m_pauseOptionId = 0;
    std::int32_t m_smoothLightingId = 0;
    std::int32_t m_profanityFilterId = 0;
    bool flipOptionAndBack(std::int32_t optionId, bool& changed);
    void* m_boolOptionSet = nullptr;
    std::int32_t m_maxDamageSlot = 0;
    void* m_damageValue = nullptr;

    void copyLocation();
    void dumpVersion();
    void copyData();
    void togglePause();
    void reloadChunks();
    void updateDebugLines();
    void clearChat();
    void setGamePaused(bool paused);
    void pauseWithScreen();
    void changeMode(int mode);
};

}
