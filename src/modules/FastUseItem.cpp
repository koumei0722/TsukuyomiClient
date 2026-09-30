#include "modules/FastUseItem.h"
#include "input/GameButtons.h"

#include "config/Config.h"
#include "core/Logger.h"
#include "hooks/Detours.h"
#include "input/Foreground.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"
#include "ui/Menu.h"

#include <Windows.h>

#include <cstdint>

namespace tsukuyomi {

void FastUseItem::onScansReady()
{
    m_useButton = GameButtons::instance().watchButton(gamebuttonlogic::button::buildOrInteract);
}

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

FastUseItem& FastUseItem::instance()
{
    static FastUseItem module;
    return module;
}

bool FastUseItem::available() const
{
    return (Scanner::instance().found(Target::UseItem)
            || Scanner::instance().found(Target::UseItemTransaction));
}

MenuItem FastUseItem::buildMenu()
{
    std::vector<MenuItem> children;
    children.push_back(menu::back());
    children.push_back(enabledItem());
    children.push_back(toggleKeyItem());
    children.push_back(menu::toggle(L"Sneak only", [this] { return m_sneakOnly; },
                                    [this] { m_sneakOnly = !m_sneakOnly; }));
    MenuItem item = menu::submenu(name(), std::move(children));
    item.available = [this] { return available(); };
    item.isOn = [this] { return enabled(); };
    return item;
}

void FastUseItem::loadConfig(const nlohmann::json& section)
{
    Module::loadConfig(section);
    m_sneakOnly = Config::getBool(section, "sneakOnly", true);
}

bool FastUseItem::playerSneaking(void* gameMode)
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

void FastUseItem::saveConfig(nlohmann::json& section) const
{
    Module::saveConfig(section);
    section["sneakOnly"] = m_sneakOnly;

    section.erase("uses");
    section.erase("intervalMs");
}

bool FastUseItem::shouldRepeat(void* gameMode) const
{
    if (!GameButtons::instance().buttonHeld(m_useButton) || !input::isInGameplay()) {
        return false;
    }
    if (!m_sneakOnly) {
        return true;
    }
    if (!Scanner::instance().found(Target::SneakingCheck)) {
        if (!m_missingSneakingLogged) {
            m_missingSneakingLogged = true;
            log().warn(L"FastUseItem: sneak check not found; Sneak only prevents extra uses");
        }
        return false;
    }
    if (!playerSneaking(gameMode)) {
        if (!m_notSneakingLogged) {
            m_notSneakingLogged = true;
            log().info(L"FastUseItem: not sneaking, so this use is not repeated (it only "
                       L"repeats while you sneak)");
        }
        return false;
    }
    return true;
}

void FastUseItem::noteExtra(int extra)
{
    if (extra <= 0) {
        return;
    }

    m_extraSinceLog += extra;
    const Clock::time_point now = Clock::now();
    if (now >= m_nextLog) {
        m_nextLog = now + std::chrono::milliseconds(kLogIntervalMs);
        log().info(L"FastUseItem: {} extra uses since last report", m_extraSinceLog);
        m_extraSinceLog = 0;
    }
}

int FastUseItem::onUseItem(void* gameMode, void* itemStack, int extra)
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

int FastUseItem::onUseItemTransaction(void* gameMode, void* itemStack, int extra)
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
        log().warn(L"FastUseItem: the game faulted on extra use {}; stopped this burst",
                   done + 1);
    }

    noteExtra(done);

    return result;
}

}
