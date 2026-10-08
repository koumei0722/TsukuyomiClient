#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "game/Abilities.h"
#include "modules/Module.h"

namespace tsukuyomi {

class CreativeNoClip : public Module {
public:
    static CreativeNoClip& instance();

    const wchar_t* name() const override { return L"CreativeNoClip"; }
    bool available() const override;

    MenuItem buildMenu() override;
    void loadConfig(const nlohmann::json& section) override;
    void saveConfig(nlohmann::json& section) const override;
    void shutdown() override;

    void onAbilitiesAccess(void* context);

    bool beforePoseDecision(void* room);
    void afterPoseDecision(void* room);

protected:
    void onEnabledChanged(bool enabled) override;

    bool persistEnabled() const override { return false; }

private:
    CreativeNoClip() = default;

    static constexpr long long kFlyingLeadMs = 250;

    static bool readBoolAbility(std::byte* layered, int index);

    std::atomic<bool> m_onlyWhileFlying{true};

    std::atomic<bool> m_active{false};
    std::atomic<bool> m_restorePending{false};

    abilities::RestoreLedger m_ledger;

    std::atomic<long long> m_noClipFromMs{0};

    std::atomic<bool> m_clipping{false};

    bool m_reported = false;
};

}
