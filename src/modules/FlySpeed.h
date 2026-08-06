#pragma once

#include <atomic>
#include <cstddef>

#include "modules/Module.h"

namespace tsukuyomi {

class FlySpeed : public Module {
public:
    static FlySpeed& instance();

    const wchar_t* name() const override { return L"FlySpeed"; }
    bool available() const override;

    MenuItem buildMenu() override;
    void loadConfig(const nlohmann::json& section) override;
    void saveConfig(nlohmann::json& section) const override;

    void shutdown() override;

    void onAbilitiesAccess(void* context);

protected:
    void onEnabledChanged(bool enabled) override;

private:
    FlySpeed() = default;

    static constexpr long long kRestoreWindowMs = 300;

    static constexpr float kMinSpeed = 0.0f;
    static constexpr float kMaxHorizontal = 2.0f;
    static constexpr float kMaxVertical = 20.0f;

    static constexpr float kEpsilon = 1.0e-6f;

    void beginRestoreWindow();

    static void applyOne(std::byte* layered, int index, float wanted);

    std::atomic<float> m_horizontal{0.05f};
    std::atomic<float> m_vertical{1.0f};

    std::atomic<bool> m_active{false};
    std::atomic<long long> m_restoreUntilMs{0};

    bool m_reported = false;
};

}
