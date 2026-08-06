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

void FlySpeed::applyOne(std::byte* layered, int index, float wanted)
{
    std::byte* const slot = abilities::slotOf(layered, index);
    if (slot == nullptr) {
        return;
    }

    float value = 0.0f;
    if (!abilities::readFloat(slot + abilities::kValueOffset, value)) {
        return;
    }
    if (std::fabs(value - wanted) <= kEpsilon) {
        return;
    }

    int type = 0;
    if (abilities::readInt(slot + abilities::kTypeOffset, type)
        && type != abilities::kTypeFloat) {
        abilities::writeInt(slot + abilities::kTypeOffset, abilities::kTypeFloat);
    }
    abilities::writeFloat(slot + abilities::kValueOffset, wanted);
}

void FlySpeed::onAbilitiesAccess(void* context)
{

    const bool active = m_active.load(std::memory_order_relaxed);
    if (!active && nowMs() >= m_restoreUntilMs.load(std::memory_order_relaxed)) {
        return;
    }

    std::byte* const layered = abilities::fromContext(context);
    if (!abilities::looksValid(layered)) {
        return;
    }

    const float horizontal =
        active ? m_horizontal.load(std::memory_order_relaxed) : abilities::kDefaultFlySpeed;
    const float vertical = active ? m_vertical.load(std::memory_order_relaxed)
                                  : abilities::kDefaultVerticalFlySpeed;

    applyOne(layered, abilities::kFlySpeed, horizontal);
    applyOne(layered, abilities::kVerticalFlySpeed, vertical);
}

void FlySpeed::beginRestoreWindow()
{

    m_restoreUntilMs.store(nowMs() + kRestoreWindowMs, std::memory_order_relaxed);
}

void FlySpeed::onEnabledChanged(bool enabled)
{
    m_active.store(enabled, std::memory_order_relaxed);

    if (enabled) {
        m_restoreUntilMs.store(0, std::memory_order_relaxed);
        if (!m_reported) {
            m_reported = true;
            log().info(L"FlySpeed: horizontal {:g} / vertical {:g}",
                       m_horizontal.load(std::memory_order_relaxed),
                       m_vertical.load(std::memory_order_relaxed));
        }
        return;
    }

    m_reported = false;
    beginRestoreWindow();
}

void FlySpeed::shutdown()
{

    if (!m_active.load(std::memory_order_relaxed)
        && nowMs() >= m_restoreUntilMs.load(std::memory_order_relaxed)) {
        return;
    }

    m_active.store(false, std::memory_order_relaxed);
    beginRestoreWindow();
    Sleep(static_cast<DWORD>(kRestoreWindowMs) + 50);
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
        false));
    children.push_back(menu::number(
        L"Vertical", [this] { return m_vertical.load(std::memory_order_relaxed); },
        [this](float value) {
            const float clamped = std::clamp(value, kMinSpeed, kMaxVertical);
            m_vertical.store(clamped, std::memory_order_relaxed);
            log().info(L"FlySpeed: vertical set to {:g} (default {:g})", clamped,
                       abilities::kDefaultVerticalFlySpeed);
        },
        false));

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
