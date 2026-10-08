#pragma once

#include <atomic>
#include <array>
#include <cstdint>

#include "game/AppleSkinMath.h"
#include "modules/Module.h"

namespace tsukuyomi {

class AppleSkin : public Module {
public:
    static AppleSkin& instance();

    const wchar_t* name() const override { return L"AppleSkin"; }
    bool available() const override { return m_ready.load(std::memory_order_relaxed); }
    void onScansReady() override;
    MenuItem buildMenu() override;
    void loadConfig(const nlohmann::json& section) override;
    void saveConfig(nlohmann::json& section) const override;
    void shutdown() override;

    void onPlayerViewUpdate();
    void onHungerRendererUpdate(void* self);
    void onHeartRendererUpdate(void* self);
    void onItemHoverText(const void* stack, void* out);
    void onHoverRender(void* self, void* ctx, void* client, void* owner);
    bool foodStats(int& hunger, float& saturation, float& exhaustion) const;

private:
    AppleSkin() = default;
    void onEnabledChanged(bool value) override;
    static void onHudCreated(void* ctrl);
    void prepareHud();
    bool readFood(const void* stack, appleskin::Food& food);
    bool hudActive() const;

    std::atomic<bool> m_ready{false};
    int m_getFoodSlot = -1;
    const void* m_hungerAttribute = nullptr;
    const void* m_saturationAttribute = nullptr;
    const void* m_exhaustionAttribute = nullptr;
    const void* m_healthAttribute = nullptr;
    void* m_getEffect = nullptr;
    std::ptrdiff_t m_hungerIcons = -1;
    std::ptrdiff_t m_heartIcons = -1;

    int m_levelOffset = -1, m_difficultySlot = -1, m_rulesSlot = -1;
    std::atomic<bool> m_on{false}, m_stopping{false}, m_statsValid{false}, m_hasFood{false};
    std::atomic<bool> m_hudReady{false}, m_tooltipBroken{false};
    std::array<std::atomic<bool>, 7> m_settings{true, true, true, true, true, true, true};
    std::atomic<float> m_maxAlpha{0.65f};
    std::array<std::atomic<std::uint8_t>, 10> m_hunger{}, m_saturation{}, m_gain{}, m_health{};
    std::atomic<int> m_heartCount{0}, m_foodHunger{0};
    std::atomic<float> m_foodSaturation{0}, m_foodExhaustion{0}, m_exhaustionRatio{0};
    appleskin::Flash m_flash;
    std::uint64_t m_flashTick = 0;
    bool m_previousShift = false, m_shiftKnown = false;
    std::atomic<int> m_reserveRows{2}, m_reserveSpaces{12};
    std::atomic<bool> m_loggedEffects{false}, m_loggedFoodFault{false};
    bool m_loggedReserve = false, m_loggedDraw = false, m_loggedBusyQueue = false;
};

}
