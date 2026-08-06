#include "modules/FastRightClick.h"

#include "core/Logger.h"
#include "hooks/Detours.h"
#include "input/Foreground.h"
#include "memory/Scanner.h"

#include <Windows.h>

namespace tsukuyomi {

FastRightClick& FastRightClick::instance()
{
    static FastRightClick module;
    return module;
}

bool FastRightClick::available() const
{
    return Scanner::instance().found(Target::UseItem);
}

void FastRightClick::saveConfig(nlohmann::json& section) const
{
    Module::saveConfig(section);

    section.erase("uses");
    section.erase("intervalMs");
}

int FastRightClick::onUseItem(void* gameMode, void* itemStack)
{

    const int result = hooks::callUseItem(gameMode, itemStack);

    if (m_repeating || !enabled()) {
        return result;
    }

    if ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) == 0 || !input::isGameForeground()) {
        return result;
    }

    constexpr int kExtra = kUsesPerBurst - 1;

    m_repeating = true;
    for (int i = 0; i < kExtra; ++i) {
        hooks::callUseItem(gameMode, itemStack);
    }
    m_repeating = false;

    m_extraSinceLog += kExtra;
    const Clock::time_point now = Clock::now();
    if (now >= m_nextLog) {
        m_nextLog = now + std::chrono::milliseconds(kLogIntervalMs);

        log().info(L"FastRightClick: {} extra uses since last report", m_extraSinceLog);
        m_extraSinceLog = 0;
    }

    return result;
}

}
