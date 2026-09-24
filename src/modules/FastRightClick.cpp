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

bool callTransactionGuarded(void* gameMode, void* itemStack, int extra)
{
    __try {
        hooks::callUseItemTransaction(gameMode, itemStack, extra);
        return true;
    } __except (accessViolationFilter(GetExceptionCode())) {
        return false;
    }
}

using SneakingCheckFn = bool(__fastcall*)(void*);

bool askSneakingGuarded(SneakingCheckFn check, void* gameMode, std::size_t playerOffset,
                        std::size_t contextOffset, bool& sneaking)
{
    __try {
        void* const player = *static_cast<void* const*>(
            static_cast<void*>(static_cast<std::uint8_t*>(gameMode) + playerOffset));
        if (player == nullptr) {
            return false;
        }
        sneaking = check(static_cast<std::uint8_t*>(player) + contextOffset);
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
    return (Scanner::instance().found(Target::UseItem)
            || Scanner::instance().found(Target::UseItemTransaction))
           && Scanner::instance().found(Target::SneakingCheck);
}

bool FastRightClick::playerSneaking(void* gameMode)
{
    if (gameMode == nullptr) {
        return false;
    }
    const auto check = Scanner::instance().addressAs<SneakingCheckFn>(Target::SneakingCheck);
    if (check == nullptr) {
        return false;
    }
    bool sneaking = false;
    if (!askSneakingGuarded(check, gameMode, kGameModePlayerOffset, kPlayerContextOffset,
                            sneaking)) {
        return false;
    }
    return sneaking;
}

void FastRightClick::saveConfig(nlohmann::json& section) const
{
    Module::saveConfig(section);

    section.erase("uses");
    section.erase("intervalMs");
}

bool FastRightClick::shouldRepeat(void* gameMode) const
{
    if ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) == 0 || !input::isInGameplay()) {
        return false;
    }
    if (!playerSneaking(gameMode)) {
        if (!m_notSneakingLogged) {
            m_notSneakingLogged = true;
            log().info(L"FastRightClick: not sneaking, so this use is not repeated (it only "
                       L"repeats while you sneak)");
        }
        return false;
    }
    return true;
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

int FastRightClick::onUseItem(void* gameMode, void* itemStack, int extra)
{
    const int result = hooks::callUseItem(gameMode, itemStack, extra);

    if (m_repeating || m_inTransaction || !enabled() || !shouldRepeat(gameMode)) {
        return result;
    }

    constexpr int kExtra = kUsesPerBurst - 1;

    m_repeating = true;
    for (int i = 0; i < kExtra; ++i) {
        hooks::callUseItem(gameMode, itemStack, extra);
    }
    m_repeating = false;

    noteExtra(kExtra);

    return result;
}

int FastRightClick::onUseItemTransaction(void* gameMode, void* itemStack, int extra)
{
    const bool wasInTransaction = m_inTransaction;
    m_inTransaction = true;

    const int result = hooks::callUseItemTransaction(gameMode, itemStack, extra);

    if (m_repeating || !enabled() || !shouldRepeat(gameMode)) {
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
        if (!callTransactionGuarded(gameMode, itemStack, extra)) {
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
