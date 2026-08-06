#include "game/GameData.h"

namespace tsukuyomi {

GameData& GameData::instance()
{
    static GameData data;
    return data;
}

void GameData::setPlayerView(const PlayerView& view)
{
    std::lock_guard lock(m_mutex);
    m_view = view;
    m_valid = true;
}

PlayerView GameData::playerView() const
{
    std::lock_guard lock(m_mutex);
    return m_view;
}

bool GameData::hasPlayerView() const
{
    std::lock_guard lock(m_mutex);
    return m_valid;
}

float GameData::yaw() const
{
    std::lock_guard lock(m_mutex);
    return m_view.yaw;
}

void GameData::setGameMode(void* gameMode)
{
    m_gameMode.store(gameMode, std::memory_order_relaxed);
}

void* GameData::gameMode() const
{
    return m_gameMode.load(std::memory_order_relaxed);
}

}
