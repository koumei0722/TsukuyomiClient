#pragma once
#include "modules/Module.h"

namespace tsukuyomi {
class PlayerInventory final : public Module {
public:
    static PlayerInventory& instance();
    const wchar_t* name() const override { return L"PlayerInventory"; }
    bool available() const override;
    void onScansReady() override;
protected:
    void onEnabledChanged(bool enabled) override;
private:
    PlayerInventory() = default;
};
}
