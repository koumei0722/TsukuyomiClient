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

void setBoolAbility(std::byte* layered, int index, int wanted)
{
    std::byte* const slot = abilities::slotOf(layered, index);
    if (slot == nullptr) {
        return;
    }

    int value = 0;
    if (!abilities::readInt(slot + abilities::kValueOffset, value)) {
        return;
    }
    if (value == wanted) {
        return;
    }

    if (wanted != 0) {

        int type = 0;
        if (abilities::readInt(slot + abilities::kTypeOffset, type)
            && type != abilities::kTypeBool) {
            abilities::writeInt(slot + abilities::kTypeOffset, abilities::kTypeBool);
        }
    }
    abilities::writeInt(slot + abilities::kValueOffset, wanted);
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
    if (!active && nowMs() >= m_restoreUntilMs.load(std::memory_order_relaxed)) {
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

    const bool noClipReady =
        allowed && nowMs() >= m_noClipFromMs.load(std::memory_order_relaxed);
    setBoolAbility(layered, abilities::kNoClip, noClipReady ? 1 : 0);
}

void CreativeNoClip::beginRestoreWindow()
{

    m_restoreUntilMs.store(nowMs() + kRestoreWindowMs, std::memory_order_relaxed);
}

void CreativeNoClip::onEnabledChanged(bool enabled)
{
    m_active.store(enabled, std::memory_order_relaxed);

    if (enabled) {
        m_restoreUntilMs.store(0, std::memory_order_relaxed);

        m_noClipFromMs.store(nowMs() + kFlyingLeadMs, std::memory_order_relaxed);
        if (!m_reported) {
            m_reported = true;
            log().info(L"CreativeNoClip: takes effect in creative and spectator only");
        }
        return;
    }

    m_reported = false;
    beginRestoreWindow();
}

void CreativeNoClip::shutdown()
{

    if (!m_active.load(std::memory_order_relaxed)
        && nowMs() >= m_restoreUntilMs.load(std::memory_order_relaxed)) {
        return;
    }

    m_active.store(false, std::memory_order_relaxed);
    beginRestoreWindow();
    Sleep(static_cast<DWORD>(kRestoreWindowMs) + 50);
}

}
