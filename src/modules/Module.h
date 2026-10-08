#pragma once

#include <atomic>
#include <nlohmann/json.hpp>

#include <utility>
#include <vector>

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

    bool enabled() const
    {
        return m_enabled.load(std::memory_order_acquire) && !m_writeBlocked
               && (m_parent == nullptr || m_parent->enabled());
    }
    bool ownEnabled() const { return m_enabled.load(std::memory_order_acquire); }
    void setEnabled(bool value);
    void toggle() { setEnabled(!m_enabled.load(std::memory_order_acquire)); }
    void toggleByKey();

    virtual bool writeBlocked() const;
    virtual void applyWriteBlock();
    bool isWriteBlocked() const { return m_writeBlocked; }

    virtual MenuItem buildMenu();

    virtual void loadConfig(const nlohmann::json& section);
    virtual void saveConfig(nlohmann::json& section) const;

    virtual void onScansReady() {}

    void update();

    virtual void shutdown() {}

    const Hotkey& toggleKey() const { return m_toggleKey; }

    void setParent(Module* parent) { m_parent = parent; }
    void parentEnabledChanged();

protected:
    Module() = default;

    virtual void onEnabledChanged(bool ) {}

    virtual void onUpdate() {}

    virtual bool persistEnabled() const { return true; }

    MenuItem enabledItem();
    MenuItem toggleKeyItem();

private:
    std::atomic<bool> m_enabled{false};
    bool m_writeBlocked = false;
    Module* m_parent = nullptr;

    Hotkey m_toggleKey;
};

}
