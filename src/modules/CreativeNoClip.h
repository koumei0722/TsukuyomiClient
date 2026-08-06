#pragma once

#include <atomic>
#include <cstddef>

#include "modules/Module.h"

namespace tsukuyomi {

class CreativeNoClip : public Module {
public:
    static CreativeNoClip& instance();

    const wchar_t* name() const override { return L"CreativeNoClip"; }
    bool available() const override;

    void shutdown() override;

    void onAbilitiesAccess(void* context);

protected:
    void onEnabledChanged(bool enabled) override;

    bool persistEnabled() const override { return false; }

private:
    CreativeNoClip() = default;

    static constexpr long long kRestoreWindowMs = 300;

    static constexpr long long kFlyingLeadMs = 250;

    static bool mayFly(std::byte* layered);

    void beginRestoreWindow();

    std::atomic<bool> m_active{false};
    std::atomic<long long> m_restoreUntilMs{0};

    std::atomic<long long> m_noClipFromMs{0};

    bool m_reported = false;
};

}
