#include "modules/Zoom.h"

#include "config/Config.h"
#include "core/Logger.h"
#include "core/Paths.h"
#include "input/Foreground.h"
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

namespace {

int accessViolationFilter(unsigned long code)
{
    return (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR)
               ? EXCEPTION_EXECUTE_HANDLER
               : EXCEPTION_CONTINUE_SEARCH;
}

std::size_t copyFloatsGuarded(const std::byte* at, float* out, std::size_t count)
{
    std::size_t done = 0;
    __try {
        for (; done < count; ++done) {
            std::memcpy(&out[done], at + done * sizeof(float), sizeof(float));
        }
    } __except (accessViolationFilter(GetExceptionCode())) {
    }
    return done;
}

HMODULE currentModule()
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                           | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&currentModule), &module);
    return module;
}

}

Zoom* Zoom::s_hookOwner = nullptr;

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
    if (std::byte* const base = Scanner::instance().address(Target::CameraUpdate);
        base != nullptr) {
        const auto* const b = reinterpret_cast<const unsigned char*>(base + kWriteFov);
        if (memory::isReadable(base + kWriteFov, kWriteFovSize) && b[0] == 0xF3 && b[1] == 0x0F
            && b[2] == 0x11) {
            m_patchFov = makeNopPatch(base + kWriteFov, kWriteFovSize);
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
            m_patchFov2 = makeNopPatch(base + kWriteFov2, kWriteFov2Size);
        } else {
            log().warn(L"Zoom: the second fov write is not a movss, so it is left alone");
        }
    }

    if (const std::byte* const fn = Scanner::instance().address(Target::HotbarSelectTick);
        fn != nullptr) {
        std::size_t size = memory::functionSize(fn);
        for (std::size_t at = 0x10; size == 0 && at + 3 < kHotbarFnScan; ++at) {
            if (!memory::isReadable(fn + at, 3)) {
                break;
            }
            const auto* const b = reinterpret_cast<const unsigned char*>(fn + at);
            if (b[0] == 0xCC && b[1] == 0xCC && b[2] == 0xCC) {
                size = at;
                break;
            }
        }
        if (size != 0) {
            m_hotbarFn = fn;
            m_hotbarFnSize = size;
        } else {
            log().warn(L"Zoom: could not find the end of the hotbar scroll function, so slot "
                       L"changes during zoom cannot be blocked");
        }
    } else {
        log().warn(L"Zoom: the hotbar scroll function was not found, so slot changes during "
                   L"zoom cannot be blocked");
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
    const auto* const at = static_cast<const std::byte*>(returnAddress);
    if (m_hotbarFn != nullptr && (at < m_hotbarFn || at >= m_hotbarFn + m_hotbarFnSize)) {
        m_hotbarOutside.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

void Zoom::onUpdate()
{
    const bool down = !m_zoomKey.empty() && enabled() && available() && m_zoomKey.isDown()
                      && input::isInGameplay();
    const bool was = m_zooming.exchange(down, std::memory_order_acq_rel);
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

    if (enabled() && available() && !m_zoomKey.empty()) {
        installMouseHook();
    } else {
        removeMouseHook();
    }

    const int wheel = m_wheel.exchange(0, std::memory_order_acq_rel);
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
        removeMouseHook();
    }
}

void Zoom::installMouseHook()
{
    if (m_mouseHook != nullptr) {
        return;
    }
    s_hookOwner = this;
    m_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, &Zoom::mouseHookProc, currentModule(), 0);
    if (m_mouseHook == nullptr) {
        s_hookOwner = nullptr;
        log().warn(L"Zoom: could not grab the mouse (error {}); the factor cannot be changed "
                   L"with the wheel",
                   GetLastError());
    }
}

void Zoom::removeMouseHook()
{
    if (m_mouseHook != nullptr) {
        UnhookWindowsHookEx(m_mouseHook);
        m_mouseHook = nullptr;
    }
    s_hookOwner = nullptr;
}

LRESULT CALLBACK Zoom::mouseHookProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && s_hookOwner != nullptr && wParam == WM_MOUSEWHEEL) {
        s_hookOwner->m_wheelAt.store(GetTickCount64(), std::memory_order_release);
        s_hookOwner->m_wheelSeen.fetch_add(1, std::memory_order_relaxed);

        if (s_hookOwner->zooming()) {
            const auto* const info = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
            const int delta = GET_WHEEL_DELTA_WPARAM(info->mouseData);
            if (delta != 0) {
                s_hookOwner->m_wheel.fetch_add(delta > 0 ? 1 : -1, std::memory_order_acq_rel);
            }
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

static bool looksLikeCamera(const std::byte* at, std::ptrdiff_t fovOffset)
{
    float v[4]{};
    if (copyFloatsGuarded(at + fovOffset - 4, v, 4) != 4) {
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
        if (!memory::isReadable(target + kFovOffsets[0], sizeof(float))) {
            continue;
        }
        std::memcpy(&current, target + kFovOffsets[0], sizeof(current));
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
        if (!memory::isWritable(target + kFovOffsets[0], sizeof(float))) {
            continue;
        }
        *reinterpret_cast<float*>(target + kFovOffsets[0]) = shrunk;
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
    removeMouseHook();
    m_zooming.store(false, std::memory_order_release);

    m_patchFov.restore();
    m_patchFov2.restore();
}

}
