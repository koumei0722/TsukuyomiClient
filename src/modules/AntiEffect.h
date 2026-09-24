#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "modules/Module.h"

namespace tsukuyomi {

class AntiEffect : public Module {
public:
    static AntiEffect& instance();

    const wchar_t* name() const override { return L"AntiEffect"; }
    bool available() const override;

    MenuItem buildMenu() override;
    void loadConfig(const nlohmann::json& section) override;
    void saveConfig(nlohmann::json& section) const override;

    void* onGetEffect(int effectId, void* original);

    enum class Effect : int {
        Darkness = 0,
        Blindness,
        Nausea,
        Count,
    };
    static constexpr std::size_t kEffectCount = static_cast<std::size_t>(Effect::Count);

    static constexpr std::int32_t kDarknessEffectId = 30;
    static constexpr std::int32_t kBlindnessEffectId = 15;
    static constexpr std::int32_t kNauseaEffectId = 9;

    bool hides(std::int32_t effectId) const;

private:
    AntiEffect() = default;

    std::atomic<bool> m_hide[kEffectCount] = {true, true, true};

    std::atomic<bool> m_toldHidden[kEffectCount] = {};
};

}
