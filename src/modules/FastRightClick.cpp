#include "modules/FastRightClick.h"

#include "core/Logger.h"
#include "hooks/Detours.h"
#include "input/Foreground.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"

#include <Windows.h>

#include <cstdint>

namespace tsukuyomi {

namespace {

int accessViolationFilter(unsigned long code)
{
    return (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR)
               ? EXCEPTION_EXECUTE_HANDLER
               : EXCEPTION_CONTINUE_SEARCH;
}

bool callTransactionGuarded(void* gameMode, void* itemStack)
{
    __try {
        hooks::callUseItemTransaction(gameMode, itemStack);
        return true;
    } __except (accessViolationFilter(GetExceptionCode())) {
        return false;
    }
}

}

FastRightClick& FastRightClick::instance()
{
    static FastRightClick module;
    return module;
}

bool FastRightClick::available() const
{

    return Scanner::instance().found(Target::UseItem)
           || Scanner::instance().found(Target::UseItemTransaction);
}

void FastRightClick::saveConfig(nlohmann::json& section) const
{
    Module::saveConfig(section);

    section.erase("uses");
    section.erase("intervalMs");
}

bool FastRightClick::shouldRepeat() const
{

    return (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0 && input::isInGameplay();
}

void FastRightClick::noteExtra(int extra)
{
    if (extra <= 0) {
        return;
    }

    m_extraSinceLog += extra;
    const Clock::time_point now = Clock::now();
    if (now >= m_nextLog) {
        m_nextLog = now + std::chrono::milliseconds(kLogIntervalMs);

        log().info(L"FastRightClick: {} extra uses since last report", m_extraSinceLog);
        m_extraSinceLog = 0;
    }
}

int FastRightClick::onUseItem(void* gameMode, void* itemStack)
{

    const int result = hooks::callUseItem(gameMode, itemStack);

    if (m_repeating || m_inTransaction || !enabled() || !shouldRepeat()) {
        return result;
    }

    constexpr int kExtra = kUsesPerBurst - 1;

    m_repeating = true;
    for (int i = 0; i < kExtra; ++i) {
        hooks::callUseItem(gameMode, itemStack);
    }
    m_repeating = false;

    noteExtra(kExtra);

    return result;
}

int FastRightClick::onUseItemTransaction(void* gameMode, void* itemStack)
{

    const bool wasInTransaction = m_inTransaction;
    m_inTransaction = true;

    const int result = hooks::callUseItemTransaction(gameMode, itemStack);

    if (m_repeating || !enabled() || !shouldRepeat()) {
        m_inTransaction = wasInTransaction;
        return result;
    }

    constexpr int kExtra = kUsesPerBurst - 1;

    if (!memory::isWritable(itemStack, kItemStackCountOffset + 1)) {
        m_inTransaction = wasInTransaction;
        return result;
    }

    auto* const countByte = static_cast<std::uint8_t*>(itemStack) + kItemStackCountOffset;
    const std::uint8_t original = *countByte;

    int limit = static_cast<int>(original) - 1;
    if (limit > kExtra) {
        limit = kExtra;
    }

    m_repeating = true;
    bool faulted = false;
    int done = 0;
    for (; done < limit; ++done) {
        *countByte = static_cast<std::uint8_t>(original - (done + 1));
        if (!callTransactionGuarded(gameMode, itemStack)) {
            faulted = true;
            break;
        }
    }

    *countByte = original;
    m_repeating = false;
    m_inTransaction = wasInTransaction;

    if (faulted) {

        log().warn(L"FastRightClick: the game faulted on extra use {}; stopped this burst",
                   done + 1);
    }

    noteExtra(done);

    return result;
}

}
