#pragma once

#include <atomic>

#include "input/ToggleInputLogic.h"
#include "modules/Module.h"

namespace tsukuyomi {

class ToggleSneakSprint : public Module {
public:
    static ToggleSneakSprint& instance();

    const wchar_t* name() const override { return L"ToggleSneakSprint"; }
    bool available() const override { return m_keyCallReturn.load(std::memory_order_acquire) != nullptr; }

    MenuItem buildMenu() override;
    void loadConfig(const nlohmann::json& section) override;
    void saveConfig(nlohmann::json& section) const override;

    void setKeyCallReturn(const void* returnAddress);

    void onInputGatherBefore(void* out, const void* returnAddress);

private:
    ToggleSneakSprint() = default;

    std::atomic<const void*> m_keyCallReturn{nullptr};
    std::atomic<bool> m_sneak{true};
    std::atomic<bool> m_sprint{true};

    toggleinput::ToggleInput m_state;
};

}
