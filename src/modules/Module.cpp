#include "modules/Module.h"

#include "config/Config.h"
#include "core/Logger.h"
#include "input/Foreground.h"

#include <utility>
#include <vector>

namespace tsukuyomi {

void Module::setEnabled(bool value)
{
    if (m_enabled == value) {
        return;
    }

    if (value && !available()) {
        log().warn(L"{} is not available on this version", name());
        return;
    }

    m_enabled = value;
    onEnabledChanged(m_enabled);

    if (m_enabled) {
        log().success(L"{} enabled", name());
    } else {
        log().info(L"{} disabled", name());
    }
}

void Module::update()
{

    const bool toggleRequested = m_toggleKey.triggered();
    if (toggleRequested && input::isGameForeground()) {
        toggle();
    }

    onUpdate();
}

MenuItem Module::enabledItem()
{
    return menu::toggle(
        L"Enabled", [this] { return enabled(); }, [this] { toggle(); });
}

MenuItem Module::toggleKeyItem()
{
    return menu::keybind(
        L"Toggle key", [this] { return m_toggleKey.combo(); },
        [this](std::vector<int> combo) {
            m_toggleKey.set(std::move(combo));
            log().info(L"{}: toggle key set to {}", name(), m_toggleKey.name());
        });
}

MenuItem Module::buildMenu()
{
    std::vector<MenuItem> children;
    children.push_back(menu::back());
    children.push_back(enabledItem());
    children.push_back(toggleKeyItem());

    MenuItem item = menu::submenu(name(), std::move(children));
    item.available = [this] { return available(); };
    item.isOn = [this] { return enabled(); };
    return item;
}

void Module::loadConfig(const nlohmann::json& section)
{
    if (persistEnabled()) {
        const bool wanted = Config::getBool(section, "enabled", false);
        if (wanted) {

            m_enabled = true;
        }
    }

    std::vector<int> combo;
    if (const auto it = section.find("keys"); it != section.end() && it->is_array()) {
        for (const auto& value : *it) {
            if (value.is_number_integer()) {
                combo.push_back(value.get<int>());
            }
        }
    }
    m_toggleKey.set(std::move(combo));
}

void Module::saveConfig(nlohmann::json& section) const
{
    if (persistEnabled()) {
        section["enabled"] = m_enabled;
    }
    section["keys"] = m_toggleKey.combo();
}

}
