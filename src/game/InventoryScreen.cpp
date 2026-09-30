#include "game/InventoryScreen.h"

#include <Windows.h>

namespace tsukuyomi {

InventoryScreen& InventoryScreen::instance()
{
    static InventoryScreen screen;
    return screen;
}

void InventoryScreen::onController()
{
    m_seenAtMs.store(GetTickCount64(), std::memory_order_relaxed);
}

bool InventoryScreen::screenLooksOpen() const
{
    const unsigned long long seen = m_seenAtMs.load(std::memory_order_relaxed);
    if (seen == 0) {
        return false;
    }
    const unsigned long long now = GetTickCount64();
    return now >= seen && now - seen <= kFreshMs;
}

}
