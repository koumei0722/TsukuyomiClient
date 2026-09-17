#include "modules/FlySpeed.h"

#include "config/Config.h"
#include "core/Logger.h"
#include "game/Abilities.h"
#include "input/Foreground.h"
#include "memory/Scanner.h"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>
#include <vector>

namespace tsukuyomi {

namespace {

long long nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

}

FlySpeed& FlySpeed::instance()
{
    static FlySpeed module;
    return module;
}

bool FlySpeed::available() const
{
    return Scanner::instance().found(Target::AbilitiesAccess);
}

bool FlySpeed::applyOne(std::byte* layered, int index, float wanted)
{
    std::byte* const slot = abilities::slotOf(layered, index);
    if (slot == nullptr) {
        return false;
    }

    float value = 0.0f;
    if (!abilities::readFloat(slot + abilities::kValueOffset, value)) {
        return false;
    }
    if (std::fabs(value - wanted) <= kEpsilon) {
        return true;
    }

    int type = 0;
    if (abilities::readInt(slot + abilities::kTypeOffset, type)
        && type != abilities::kTypeFloat) {
        abilities::writeInt(slot + abilities::kTypeOffset, abilities::kTypeFloat);
    }
    abilities::writeFloat(slot + abilities::kValueOffset, wanted);
    return false;
}

void FlySpeed::onAbilitiesAccess(void* context)
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

    if (active) {
        m_ledger.note(layered);
        applyOne(layered, abilities::kFlySpeed, m_horizontal.load(std::memory_order_relaxed));
        applyOne(layered, abilities::kVerticalFlySpeed,
                 m_vertical.load(std::memory_order_relaxed));
        return;
    }

    if (m_ledger.needsRestore(layered)) {
        const bool okHorizontal =
            applyOne(layered, abilities::kFlySpeed, abilities::kDefaultFlySpeed);
        const bool okVertical =
            applyOne(layered, abilities::kVerticalFlySpeed, abilities::kDefaultVerticalFlySpeed);
        if (okHorizontal && okVertical) {
            m_ledger.markClean(layered);
        }
    }

    if (m_ledger.allClean() && m_restorePending.exchange(false, std::memory_order_relaxed)) {
        log().info(L"FlySpeed: restored the vanilla fly speed");
    }
}

void FlySpeed::onEnabledChanged(bool enabled)
{
    m_active.store(enabled, std::memory_order_relaxed);

    if (enabled) {
        m_restorePending.store(false, std::memory_order_relaxed);
        m_ledger.clearDirty();
        if (!m_reported) {
            m_reported = true;
            log().info(L"FlySpeed: horizontal {:g} / vertical {:g}",
                       m_horizontal.load(std::memory_order_relaxed),
                       m_vertical.load(std::memory_order_relaxed));
        }
        return;
    }

    m_reported = false;
    m_ledger.markAllDirty();
    m_restorePending.store(true, std::memory_order_relaxed);
}

void FlySpeed::shutdown()
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
    log().warn(L"FlySpeed: gave up waiting for the restore (the game is not ticking)");
}

MenuItem FlySpeed::buildMenu()
{
    std::vector<MenuItem> children;
    children.push_back(menu::back());
    children.push_back(enabledItem());
    children.push_back(toggleKeyItem());
    children.push_back(menu::number(
        L"Horizontal", [this] { return m_horizontal.load(std::memory_order_relaxed); },
        [this](float value) {
            const float clamped = std::clamp(value, kMinSpeed, kMaxHorizontal);
            m_horizontal.store(clamped, std::memory_order_relaxed);
            log().info(L"FlySpeed: horizontal set to {:g} (default {:g})", clamped,
                       abilities::kDefaultFlySpeed);
        },
        false, kMinSpeed, kMaxHorizontal));
    children.push_back(menu::number(
        L"Vertical", [this] { return m_vertical.load(std::memory_order_relaxed); },
        [this](float value) {
            const float clamped = std::clamp(value, kMinSpeed, kMaxVertical);
            m_vertical.store(clamped, std::memory_order_relaxed);
            log().info(L"FlySpeed: vertical set to {:g} (default {:g})", clamped,
                       abilities::kDefaultVerticalFlySpeed);
        },
        false, kMinSpeed, kMaxVertical));

    MenuItem item = menu::submenu(name(), std::move(children));
    item.available = [this] { return available(); };
    item.isOn = [this] { return enabled(); };
    return item;
}

void FlySpeed::loadConfig(const nlohmann::json& section)
{
    Module::loadConfig(section);

    m_horizontal.store(
        std::clamp(Config::getFloat(section, "horizontal", abilities::kDefaultFlySpeed), kMinSpeed,
                   kMaxHorizontal),
        std::memory_order_relaxed);
    m_vertical.store(
        std::clamp(Config::getFloat(section, "vertical", abilities::kDefaultVerticalFlySpeed),
                   kMinSpeed, kMaxVertical),
        std::memory_order_relaxed);
}

void FlySpeed::saveConfig(nlohmann::json& section) const
{
    Module::saveConfig(section);
    section["horizontal"] = m_horizontal.load(std::memory_order_relaxed);
    section["vertical"] = m_vertical.load(std::memory_order_relaxed);
}

}
