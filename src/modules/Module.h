#pragma once

#include <nlohmann/json.hpp>

#include "input/Hotkey.h"
#include "ui/Menu.h"

namespace tsukuyomi {

class Module {
public:
    virtual ~Module() = default;

    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;

    virtual const wchar_t* name() const = 0;

    virtual bool available() const { return true; }

    bool enabled() const { return m_enabled; }
    void setEnabled(bool value);
    void toggle() { setEnabled(!m_enabled); }

    virtual MenuItem buildMenu();

    virtual void loadConfig(const nlohmann::json& section);
    virtual void saveConfig(nlohmann::json& section) const;

    virtual void onScansReady() {}

    void update();

    virtual void shutdown() {}

    const Hotkey& toggleKey() const { return m_toggleKey; }

protected:
    Module() = default;

    virtual void onEnabledChanged(bool ) {}

    virtual void onUpdate() {}

    virtual bool persistEnabled() const { return true; }

    MenuItem enabledItem();
    MenuItem toggleKeyItem();

private:
    bool m_enabled = false;

    Hotkey m_toggleKey;
};

}
