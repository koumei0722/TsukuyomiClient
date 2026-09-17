#include "modules/CreativeNoClip.h"

#include "core/Logger.h"
#include "game/Abilities.h"
#include "memory/Scanner.h"

#include <Windows.h>

#include <chrono>

namespace tsukuyomi {

namespace {

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

bool CreativeNoClip::mayFly(std::byte* layered)
{
    std::byte* const slot = abilities::slotOf(layered, abilities::kMayFly);
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

    const bool allowed = active && mayFly(layered);

    if (allowed) {
        setBoolAbility(layered, abilities::kFlying, 1);
    }

    if (active) {
        m_ledger.note(layered);
        const bool noClipReady =
            allowed && nowMs() >= m_noClipFromMs.load(std::memory_order_relaxed);
        setBoolAbility(layered, abilities::kNoClip, noClipReady ? 1 : 0);
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

void CreativeNoClip::onEnabledChanged(bool enabled)
{
    m_active.store(enabled, std::memory_order_relaxed);

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

    m_active.store(false, std::memory_order_relaxed);
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
