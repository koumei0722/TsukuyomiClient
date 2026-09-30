#pragma once

#include <atomic>

#include "game/GameModeIds.h"

namespace tsukuyomi {

class GameModeState {
public:
    static GameModeState& instance();
    GameModeState() = default;

    void onSetGameMode(int mode, unsigned long long nowMs);
    void onPlayerView(unsigned long long nowMs);

    int currentMode() const { return m_current.load(std::memory_order_relaxed); }
    int spectatorTarget() const;

private:
    static constexpr unsigned long long kViewGapMs = 1500;

    std::atomic<int> m_current{gamemode::kUnknown};
    std::atomic<int> m_previous{gamemode::kUnknown};
    std::atomic<unsigned long long> m_lastSetMs{0};
    std::atomic<unsigned long long> m_lastViewMs{0};
};

}
