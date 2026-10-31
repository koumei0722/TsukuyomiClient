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

    float scaleProjection(float tangent) const;

    bool zooming() const { return m_zooming.load(std::memory_order_acquire); }

    bool suppressHotbar(const void* returnAddress) const;

protected:
    void onUpdate() override;
    void onEnabledChanged(bool enabled) override;

private:
    Zoom() = default;

    Patch m_projection[2];

    static constexpr float kMinFactor = 1.25f;
    static constexpr float kMaxFactor = 30.0f;
    static constexpr float kDefaultFactor = 4.0f;

    static constexpr float kFactorStep = 0.5f;

    Hotkey m_zoomKey;

    float m_factor = kDefaultFactor;

    std::atomic<float> m_activeFactor{kDefaultFactor};

    std::atomic<bool> m_zooming{false};

    int m_wheelLeftButton = -1;
    int m_wheelRightButton = -1;
    std::uint64_t m_wheelLeftSeen = 0;
    std::uint64_t m_wheelRightSeen = 0;
};

}
