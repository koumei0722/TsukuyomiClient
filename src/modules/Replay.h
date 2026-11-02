#pragma once
#include "modules/Module.h"
#include "replay/ReplayCapture.h"
#include <mutex>

namespace tsukuyomi {
class Replay final : public Module {
public:
    static Replay& instance();
    const wchar_t* name() const override { return L"Replay"; }
    bool writeBlocked() const override;
    void onPlayerViewUpdate();
    bool wantsBlockWriteHook() const { return enabled(); }
    void shutdown() override;
protected:
    bool persistEnabled() const override { return false; }
    void onEnabledChanged(bool enabled) override;
    void onUpdate() override;
private:
    Replay() = default;
    std::mutex mutex_;
    replay::ReplayCapture capture_;
};
}
