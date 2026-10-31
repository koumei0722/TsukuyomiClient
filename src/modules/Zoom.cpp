#include "modules/Zoom.h"

#include "config/Config.h"
#include "core/Logger.h"
#include "game/ZoomLogic.h"
#include "input/Foreground.h"
#include "input/GameButtons.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"

#include <algorithm>

namespace tsukuyomi {

Zoom& Zoom::instance()
{
    static Zoom module;
    return module;
}

namespace {
using TangentFn = float (*)(float);
TangentFn g_projectionTangent = nullptr;

float projectionTangent(float halfFov)
{
    const float tangent = g_projectionTangent(halfFov);
    return Zoom::instance().scaleProjection(tangent);
}
}

bool Zoom::available() const
{
    return m_projection[0].applied() && m_projection[1].applied();
}

void Zoom::onScansReady()
{
    m_wheelLeftButton = GameButtons::instance().watchButton(gamebuttonlogic::button::inventoryLeft);
    m_wheelRightButton = GameButtons::instance().watchButton(gamebuttonlogic::button::inventoryRight);
    const auto& scanner = Scanner::instance();
    const Target targets[] = {Target::CameraProjectionTangent, Target::CameraProjectionTangentAlt};
    const void* cell = nullptr;
    for (std::size_t i = 0; i < 2; ++i) {
        auto* const site = scanner.address(targets[i]);
        if (site == nullptr) return;
        const void* current = memory::ripTarget(site + 13, 2);
        if (current == nullptr || (cell != nullptr && current != cell)) return;
        cell = current;
        m_projection[i] = makeIndirectCallPatch(site + 13,
            reinterpret_cast<const void*>(&projectionTangent), "Zoom.Fov");
        if (!m_projection[i].valid()) return;
    }
    if (!memory::copyGuarded(cell, &g_projectionTangent, sizeof(g_projectionTangent))
        || g_projectionTangent == nullptr) return;
    Patch* const patches[] = {&m_projection[0], &m_projection[1]};
    if (!Patch::setAll(patches, true)) {
        Patch::setAll(patches, false);
        log().warn(L"Zoom: the projection calls could not be redirected");
    }
}

float Zoom::scaleProjection(float tangent) const
{
    if (!zooming()) return tangent;
    return zoomlogic::scaleProjection(tangent,
        std::clamp(m_activeFactor.load(std::memory_order_acquire), kMinFactor, kMaxFactor));
}

bool Zoom::suppressHotbar(const void* returnAddress) const
{
    if (!zooming()) {
        return false;
    }
    if (returnAddress == nullptr) {
        return false;
    }
    return true;
}

void Zoom::onUpdate()
{
    const bool down = !m_zoomKey.empty() && enabled() && available() && m_zoomKey.isDown()
                      && input::isInGameplay();
    if (down && !m_zooming.load(std::memory_order_acquire)) {
        m_activeFactor.store(std::clamp(m_factor, kMinFactor, kMaxFactor), std::memory_order_release);
    }
    const bool was = m_zooming.exchange(down, std::memory_order_acq_rel);
    const auto& buttons = GameButtons::instance();
    const std::uint64_t left = buttons.buttonPressSeq(m_wheelLeftButton);
    const std::uint64_t right = buttons.buttonPressSeq(m_wheelRightButton);
    const int wheel = down && was
        ? gamebuttonlogic::wheelNotches(left, m_wheelLeftSeen, right, m_wheelRightSeen) : 0;
    m_wheelLeftSeen = left;
    m_wheelRightSeen = right;
    if (down != was) {
        log().info(L"Zoom: {} (factor {:.2f})", down ? L"started" : L"stopped",
                   m_activeFactor.load(std::memory_order_acquire));
    }

    if (wheel != 0) {
        const float before = m_activeFactor.load(std::memory_order_acquire);
        const float after = std::clamp(before + kFactorStep * static_cast<float>(wheel), kMinFactor,
                                       kMaxFactor);
        if (after != before) {
            m_activeFactor.store(after, std::memory_order_release);
            log().info(L"Zoom: factor {:.2f} -> {:.2f}", before, after);
        }
    }
}

void Zoom::onEnabledChanged(bool enabled)
{
    if (!enabled) {
        if (m_zooming.exchange(false, std::memory_order_acq_rel)) {
            log().info(L"Zoom: stopped (module switched off)");
        }
    }
}

MenuItem Zoom::buildMenu()
{
    std::vector<MenuItem> children;
    children.push_back(enabledItem());
    children.push_back(toggleKeyItem());
    children.push_back(menu::keybind(
        L"Zoom key", [this] { return m_zoomKey.combo(); },
        [this](std::vector<int> combo) {
            m_zoomKey.set(std::move(combo));
            log().info(L"Zoom: zoom key set to {}", m_zoomKey.name());
        },
        {}));
    bindPad(children.back(), m_zoomKey);
    children.push_back(menu::number(
        L"Factor", [this] { return m_factor; },
        [this](float value) { m_factor = std::clamp(value, kMinFactor, kMaxFactor); }, false,
        kMinFactor, kMaxFactor));

    MenuItem item = menu::submenu(name(), std::move(children));
    item.available = [this] { return available(); };
    item.isOn = [this] { return enabled(); };
    return item;
}

void Zoom::loadConfig(const nlohmann::json& section)
{
    Module::loadConfig(section);

    std::vector<int> combo;
    if (const auto it = section.find("zoomKeys"); it != section.end() && it->is_array()) {
        for (const auto& value : *it) {
            if (value.is_number_integer()) {
                combo.push_back(value.get<int>());
            }
        }
    }
    m_zoomKey.set(std::move(combo));

    m_factor = std::clamp(Config::getFloat(section, "factor", kDefaultFactor), kMinFactor,
                          kMaxFactor);
}

void Zoom::saveConfig(nlohmann::json& section) const
{
    Module::saveConfig(section);
    section["zoomKeys"] = m_zoomKey.combo();
    section["factor"] = m_factor;
}

void Zoom::shutdown()
{
    m_zooming.store(false, std::memory_order_release);
    Patch* const patches[] = {&m_projection[0], &m_projection[1]};
    Patch::setAll(patches, false);
}

}
