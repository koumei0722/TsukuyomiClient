#pragma once

#include <array>

#include "modules/Module.h"

namespace tsukuyomi {

class JavaUI : public Module {
public:
    static JavaUI& instance();

    const wchar_t* name() const override { return L"JavaUI"; }
    bool available() const override;
    bool writeBlocked() const override;
    void applyWriteBlock() override;

    MenuItem buildMenu() override;
    void loadConfig(const nlohmann::json& section) override;
    void saveConfig(nlohmann::json& section) const override;

    void onScansReady() override;
    void shutdown() override;

protected:
    void onUpdate() override;
    void onEnabledChanged(bool enabled) override;

private:
    JavaUI();

    struct Part {
        Module* module;
        const wchar_t* label;
        const char* commandName;
    };
    std::array<Part, 4> m_parts;
};

}
