#include "modules/Zoom.h"

#include "config/Config.h"
#include "core/Logger.h"
#include "core/Paths.h"
#include "input/Foreground.h"
#include "input/GameButtons.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"

#include <TlHelp32.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <system_error>
#include <cstring>
#include <format>
#include <string>

namespace tsukuyomi {

Zoom& Zoom::instance()
{
    static Zoom module;
    return module;
}

bool Zoom::available() const
{
    return Scanner::instance().found(Target::CameraUpdate);
}

void Zoom::onScansReady()
{
    m_wheelLeftButton = GameButtons::instance().watchButton(gamebuttonlogic::button::inventoryLeft);
    m_wheelRightButton = GameButtons::instance().watchButton(gamebuttonlogic::button::inventoryRight);

    if (std::byte* const base = Scanner::instance().address(Target::CameraUpdate);
        base != nullptr) {
        const auto* const b = reinterpret_cast<const unsigned char*>(base + kWriteFov);
        if (memory::isReadable(base + kWriteFov, kWriteFovSize) && b[0] == 0xF3 && b[1] == 0x0F
            && b[2] == 0x11) {
            m_patchFov = makeNopPatch(base + kWriteFov, kWriteFovSize, "Zoom.Fov");
        } else {
            log().warn(L"Zoom: the fov write is not a movss, so it is left alone (zoom may not "
                       L"work)");
        }
    }

    if (std::byte* const base = Scanner::instance().address(Target::CameraFovStore);
        base != nullptr) {
        const auto* const b = reinterpret_cast<const unsigned char*>(base + kWriteFov2);
        if (memory::isReadable(base + kWriteFov2, kWriteFov2Size) && b[0] == 0xF3 && b[1] == 0x41
            && b[2] == 0x0F && b[3] == 0x11) {
            m_patchFov2 = makeNopPatch(base + kWriteFov2, kWriteFov2Size, "Zoom.Fov");
        } else {
            log().warn(L"Zoom: the second fov write is not a movss, so it is left alone");
        }
    }
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
    const bool was = m_zooming.exchange(down, std::memory_order_acq_rel);
    const auto& buttons = GameButtons::instance();
    const std::uint64_t left = buttons.buttonPressSeq(m_wheelLeftButton);
    const std::uint64_t right = buttons.buttonPressSeq(m_wheelRightButton);
    const int wheel = down && was
        ? gamebuttonlogic::wheelNotches(left, m_wheelLeftSeen, right, m_wheelRightSeen) : 0;
    m_wheelLeftSeen = left;
    m_wheelRightSeen = right;
    if (down != was) {
        if (down) {
            m_baseFovReady = false;
            m_patchFov.apply();
            m_patchFov2.apply();
        } else {
            m_restoreFov.store(true, std::memory_order_release);
        }
        log().info(L"Zoom: {} (factor {:.2f})", down ? L"started" : L"stopped", m_factor);
    }

    if (wheel != 0) {
        const float before = m_factor;
        m_factor = std::clamp(m_factor + kFactorStep * static_cast<float>(wheel), kMinFactor,
                              kMaxFactor);
        if (m_factor != before) {
            log().info(L"Zoom: factor {:.2f} -> {:.2f}", before, m_factor);
        }
    }
}

void Zoom::onEnabledChanged(bool enabled)
{
    if (!enabled) {
        m_zooming.store(false, std::memory_order_release);
    }
}

static bool looksLikeCamera(const std::byte* at, std::ptrdiff_t fovOffset)
{
    float v[4]{};
    if (!memory::copyGuarded(at + fovOffset - 4, v, sizeof(v))) {
        return false;
    }
    const float aspect = v[0];
    const float fov = v[1];
    const float nearPlane = v[2];
    const float farPlane = v[3];
    return std::isfinite(aspect) && aspect > 0.2f && aspect < 8.0f
           && std::isfinite(fov) && fov > 0.05f && fov < 3.2f
           && std::isfinite(nearPlane) && nearPlane > 0.0f && nearPlane < 10.0f
           && std::isfinite(farPlane) && farPlane > 16.0f && farPlane < 1.0e7f;
}

void Zoom::onCameraWrite(void* cameraBase, void* source)
{
    if (cameraBase == nullptr) {
        return;
    }

    auto* const base = static_cast<std::byte*>(cameraBase);

    if (m_restoreFov.exchange(false, std::memory_order_acq_rel)) {
        m_baseFovReady = false;
        m_patchFov.restore();
        m_patchFov2.restore();
        return;
    }

    if (!zooming()) {
        return;
    }

    const float factor = std::clamp(m_factor, kMinFactor, kMaxFactor);
    int wrote = 0;
    std::byte* const targets[] = {static_cast<std::byte*>(source), base};
    bool captured = false;
    for (std::size_t slot = 0; slot < 2; ++slot) {
        std::byte* const target = targets[slot];
        if (target == nullptr) {
            continue;
        }
        if (!looksLikeCamera(target, kFovOffsets[0])) {
            continue;
        }
        float current = 0.0f;
        if (!memory::copyGuarded(target + kFovOffsets[0], &current, sizeof(current))) {
            continue;
        }
        if (!std::isfinite(current) || current < kFovMin || current > kFovMax) {
            continue;
        }
        if (!m_baseFovReady) {
            m_baseFov[slot] = current;
            captured = true;
        }
        const float original =
            (std::isfinite(m_baseFov[slot]) && m_baseFov[slot] >= kFovMin
             && m_baseFov[slot] <= kFovMax)
                ? m_baseFov[slot]
                : current;

        const float shrunk = 2.0f * std::atan(std::tan(original * 0.5f) / factor);
        if (!std::isfinite(shrunk) || shrunk <= 0.0f) {
            continue;
        }
        if (!memory::writeGuarded(target + kFovOffsets[0], &shrunk, sizeof(shrunk))) {
            continue;
        }
        ++wrote;
    }
    if (captured && wrote != 0) {
        m_baseFovReady = true;
    }
}

MenuItem Zoom::buildMenu()
{
    std::vector<MenuItem> children;
    children.push_back(menu::back());
    children.push_back(enabledItem());
    children.push_back(toggleKeyItem());
    children.push_back(menu::keybind(
        L"Zoom key", [this] { return m_zoomKey.combo(); },
        [this](std::vector<int> combo) {
            m_zoomKey.set(std::move(combo));
            log().info(L"Zoom: zoom key set to {}", m_zoomKey.name());
        },
        {}));
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

    m_patchFov.restore();
    m_patchFov2.restore();
}

}
