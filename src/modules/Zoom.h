#pragma once

#include <Windows.h>

#include <atomic>
#include <cstddef>
#include <vector>

#include "input/Hotkey.h"
#include "memory/Patch.h"
#include "modules/Module.h"

namespace tsukuyomi {

class Zoom : public Module {
public:
    static Zoom& instance();

    const wchar_t* name() const override { return L"Zoom"; }
    bool available() const override;

    MenuItem buildMenu() override;
    void loadConfig(const nlohmann::json& section) override;
    void saveConfig(nlohmann::json& section) const override;

    void onScansReady() override;
    void shutdown() override;

    void onCameraWrite(void* cameraBase, void* source);

    bool zooming() const { return m_zooming.load(std::memory_order_acquire); }

    bool wheelFresh(unsigned long long ms = 400) const
    {
        const unsigned long long at = m_wheelAt.load(std::memory_order_acquire);
        return at != 0 && (GetTickCount64() - at) <= ms;
    }

    unsigned long long wheelSeen() const
    {
        return m_wheelSeen.load(std::memory_order_relaxed);
    }

    bool suppressHotbar(const void* returnAddress) const;

protected:
    void onUpdate() override;
    void onEnabledChanged(bool enabled) override;

    bool persistEnabled() const override { return true; }

private:
    Zoom() = default;

    void installMouseHook();
    void removeMouseHook();
    static LRESULT CALLBACK mouseHookProc(int code, WPARAM wParam, LPARAM lParam);

    static constexpr std::ptrdiff_t kFovOffsets[] = {0x50, 0x170};

    static constexpr float kFovMin = 0.1f;
    static constexpr float kFovMax = 3.2f;

    static constexpr std::ptrdiff_t kWriteFov = 0x57;
    static constexpr std::size_t kWriteFovSize = 5;

    Patch m_patchFov;

    static constexpr std::ptrdiff_t kWriteFov2 = 0x16;
    static constexpr std::size_t kWriteFov2Size = 7;
    Patch m_patchFov2;

    float m_baseFov[2]{};
    bool m_baseFovReady = false;

    std::atomic<bool> m_restoreFov{false};

    static constexpr float kMinFactor = 1.25f;
    static constexpr float kMaxFactor = 10.0f;
    static constexpr float kDefaultFactor = 4.0f;

    static constexpr float kFactorStep = 0.5f;

    Hotkey m_zoomKey;

    float m_factor = kDefaultFactor;

    std::atomic<bool> m_zooming{false};

    static Zoom* s_hookOwner;
    HHOOK m_mouseHook = nullptr;

    std::atomic<int> m_wheel{0};

    std::atomic<unsigned long long> m_wheelAt{0};
    std::atomic<unsigned long long> m_wheelSeen{0};

    const std::byte* m_hotbarFn = nullptr;
    std::size_t m_hotbarFnSize = 0;
    static constexpr std::size_t kHotbarFnScan = 0x4000;

    mutable std::atomic<bool> m_hotbarLogged{false};
    mutable std::atomic<unsigned long long> m_hotbarOutside{0};

};

}
