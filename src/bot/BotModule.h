#pragma once
#include "modules/Module.h"

namespace tsukuyomi {
class Bot final : public Module {
public:
    static Bot& instance();
    const wchar_t* name() const override { return L"Bot"; }
    bool available() const override;
    MenuItem buildMenu() override;
    void loadConfig(const nlohmann::json& section) override;
    void onScansReady() override;
    void onPlayerViewUpdate();
    void shutdown() override;
private:
    Bot() = default;
    bool enabledWasInConfig_ = false;
};
}
