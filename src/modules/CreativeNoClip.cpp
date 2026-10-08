#include "modules/CreativeNoClip.h"

#include "config/Config.h"
#include "core/Logger.h"
#include "game/Abilities.h"
#include "memory/Scanner.h"
#include "modules/DebugScreen.h"
#include "ui/Menu.h"

#include <Windows.h>

#include <chrono>

namespace tsukuyomi {

namespace {

constexpr std::size_t kRoomStanding = 0xc;

int accessFilter(unsigned long code)
{
    return code == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

bool swapByte(void* at, std::uint8_t from, std::uint8_t to)
{
    __try {
        auto* const byte = static_cast<volatile std::uint8_t*>(at);
        if (*byte != from) {
            return false;
        }
        *byte = to;
        return true;
    } __except (accessFilter(GetExceptionCode())) {
        return false;
    }
}

long long nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

bool setBoolAbility(std::byte* layered, int index, int wanted)
{
    std::byte* const slot = abilities::slotOf(layered, index);
    if (slot == nullptr) {
        return false;
    }

    int value = 0;
    if (!abilities::readInt(slot + abilities::kValueOffset, value)) {
        return false;
    }
    if (value == wanted) {
        return true;
    }

    if (wanted != 0) {
        int type = 0;
        if (abilities::readInt(slot + abilities::kTypeOffset, type)
            && type != abilities::kTypeBool) {
            abilities::writeInt(slot + abilities::kTypeOffset, abilities::kTypeBool);
        }
    }
    abilities::writeInt(slot + abilities::kValueOffset, wanted);
    return false;
}

}

CreativeNoClip& CreativeNoClip::instance()
{
    static CreativeNoClip module;
    return module;
}

bool CreativeNoClip::available() const
{
    return Scanner::instance().found(Target::AbilitiesAccess);
}

MenuItem CreativeNoClip::buildMenu()
{
    std::vector<MenuItem> children;
    children.push_back(enabledItem());
    children.push_back(toggleKeyItem());
    children.push_back(menu::toggle(
        L"Only while flying", [this] { return m_onlyWhileFlying.load(std::memory_order_relaxed); },
        [this] { m_onlyWhileFlying.store(!m_onlyWhileFlying.load(std::memory_order_relaxed), std::memory_order_relaxed); }));
    MenuItem item = menu::submenu(name(), std::move(children));
    item.available = [this] { return available(); };
    item.isOn = [this] { return enabled(); };
    return item;
}

void CreativeNoClip::loadConfig(const nlohmann::json& section)
{
    Module::loadConfig(section);
    m_onlyWhileFlying.store(Config::getBool(section, "onlyWhileFlying", true), std::memory_order_relaxed);
}

void CreativeNoClip::saveConfig(nlohmann::json& section) const
{
    Module::saveConfig(section);
    section["onlyWhileFlying"] = m_onlyWhileFlying.load(std::memory_order_relaxed);
}

bool CreativeNoClip::readBoolAbility(std::byte* layered, int index)
{
    std::byte* const slot = abilities::slotOf(layered, index);
    if (slot == nullptr) {
        return false;
    }
    int value = 0;
    if (!abilities::readInt(slot + abilities::kValueOffset, value)) {
        return false;
    }
    return value != 0;
}

void CreativeNoClip::onAbilitiesAccess(void* context)
{
    const bool active = m_active.load(std::memory_order_relaxed);
    const bool restoring = m_restorePending.load(std::memory_order_relaxed);
    if (!active && !restoring) {
        return;
    }

    std::byte* const layered = abilities::fromContext(context);
    if (!abilities::looksValid(layered)) {
        return;
    }
    m_ledger.observeWorld(DebugScreen::worldEntry());

    const bool canFly = active && readBoolAbility(layered, abilities::kMayFly);
    const bool forceFlying = canFly && !m_onlyWhileFlying.load(std::memory_order_relaxed);
    const bool allowed = canFly && (forceFlying || readBoolAbility(layered, abilities::kFlying));
    const bool wasClipping = m_clipping.exchange(allowed, std::memory_order_relaxed);
    if (allowed && !wasClipping) {
        m_noClipFromMs.store(nowMs() + kFlyingLeadMs, std::memory_order_relaxed);
    }

    if (forceFlying) {
        setBoolAbility(layered, abilities::kFlying, 1);
    }

    if (active) {
        m_ledger.note(layered);
        const bool noClipReady =
            allowed && nowMs() >= m_noClipFromMs.load(std::memory_order_relaxed);
        setBoolAbility(layered, abilities::kNoClip, noClipReady ? 1 : 0);
        if (!m_active.load(std::memory_order_seq_cst)) {
            m_ledger.markDirty(layered);
        }
        return;
    }

    if (m_ledger.needsRestore(layered)
        && setBoolAbility(layered, abilities::kNoClip, 0)) {
        m_ledger.markClean(layered);
    }

    if (m_ledger.allClean() && m_restorePending.exchange(false, std::memory_order_relaxed)) {
        log().info(L"CreativeNoClip: restored (no-clip is off again)");
    }
}

bool CreativeNoClip::beforePoseDecision(void* room)
{
    if (!m_clipping.load(std::memory_order_relaxed) || room == nullptr) {
        return false;
    }
    if (!swapByte(static_cast<std::byte*>(room) + kRoomStanding, 0, 1)) {
        return false;
    }
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed)) {
        log().info(L"CreativeNoClip: told the pose decision there is standing room (no auto sneak / crawl)");
    }
    return true;
}

void CreativeNoClip::afterPoseDecision(void* room)
{
    swapByte(static_cast<std::byte*>(room) + kRoomStanding, 1, 0);
}

void CreativeNoClip::onEnabledChanged(bool enabled)
{
    m_active.store(enabled, std::memory_order_seq_cst);
    if (!enabled) {
        m_clipping.store(false, std::memory_order_relaxed);
    }

    if (enabled) {
        m_restorePending.store(false, std::memory_order_relaxed);
        m_ledger.clearDirty();

        m_noClipFromMs.store(nowMs() + kFlyingLeadMs, std::memory_order_relaxed);
        if (!m_reported) {
            m_reported = true;
            log().info(L"CreativeNoClip: takes effect in creative and spectator only");
        }
        return;
    }

    m_reported = false;
    m_ledger.markAllDirty();
    m_restorePending.store(true, std::memory_order_relaxed);
}

void CreativeNoClip::shutdown()
{
    if (!m_active.load(std::memory_order_relaxed)
        && !m_restorePending.load(std::memory_order_relaxed)) {
        return;
    }

    m_active.store(false, std::memory_order_seq_cst);
    m_ledger.markAllDirty();
    m_restorePending.store(true, std::memory_order_relaxed);

    constexpr int kGiveUpMs = 2000;
    for (int waited = 0; waited < kGiveUpMs; waited += 10) {
        if (!m_restorePending.load(std::memory_order_relaxed)) {
            return;
        }
        Sleep(10);
    }
    log().warn(L"CreativeNoClip: gave up waiting for the restore (the game is not ticking)");
}

}
