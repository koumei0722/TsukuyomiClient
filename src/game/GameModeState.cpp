#include "game/GameModeState.h"

namespace tsukuyomi {

GameModeState& GameModeState::instance()
{
    static GameModeState state;
    return state;
}

void GameModeState::onSetGameMode(int mode, unsigned long long nowMs)
{
    const int current = m_current.load(std::memory_order_relaxed);
    if (current != mode && gamemode::isSelectable(current)) {
        m_previous.store(current, std::memory_order_relaxed);
    }
    m_current.store(mode, std::memory_order_relaxed);
    m_lastSetMs.store(nowMs, std::memory_order_relaxed);
}

void GameModeState::onPlayerView(unsigned long long nowMs)
{
    const unsigned long long last = m_lastViewMs.exchange(nowMs, std::memory_order_relaxed);
    if (last == 0 || nowMs - last <= kViewGapMs) {
        return;
    }
    if (m_lastSetMs.load(std::memory_order_relaxed) > last) {
        return;
    }
    m_current.store(gamemode::kUnknown, std::memory_order_relaxed);
    m_previous.store(gamemode::kUnknown, std::memory_order_relaxed);
}

int GameModeState::spectatorTarget() const
{
    if (m_current.load(std::memory_order_relaxed) != gamemode::kSpectator) {
        return gamemode::kSpectator;
    }
    const int previous = m_previous.load(std::memory_order_relaxed);
    return (gamemode::isSelectable(previous) && previous != gamemode::kSpectator) ? previous
                                                                                 : gamemode::kCreative;
}

}
