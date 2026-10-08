#include "modules/JavaUI.h"

#include "core/Strings.h"
#include "modules/DebugScreen.h"
#include "modules/InventoryEffects.h"
#include "modules/OffhandSlot.h"
#include "modules/PlayerList.h"

#include <string>
#include <utility>
#include <vector>

namespace tsukuyomi {

namespace {

constexpr int kMenuOrder[] = {3, 2, 0, 1};

}

JavaUI& JavaUI::instance()
{
    static JavaUI module;
    return module;
}

JavaUI::JavaUI()
    : m_parts{{
          {&PlayerList::instance(), L"Player list", "player_list"},
          {&DebugScreen::instance(), L"Debug screen", "debug_screen"},
          {&InventoryEffects::instance(), L"Inventory effects", "inventory_effects"},
          {&OffhandSlot::instance(), L"Offhand slot", "offhand_slot"},
      }}
{
    for (const Part& part : m_parts) {
        part.module->setParent(this);
    }
}

bool JavaUI::available() const
{
    for (const Part& part : m_parts) {
        if (part.module->available()) {
            return true;
        }
    }
    return false;
}

bool JavaUI::writeBlocked() const
{
    for (const Part& part : m_parts) {
        if (!part.module->isWriteBlocked()) {
            return false;
        }
    }
    return true;
}

void JavaUI::applyWriteBlock()
{
    for (const Part& part : m_parts) {
        part.module->applyWriteBlock();
    }
    Module::applyWriteBlock();
}

MenuItem JavaUI::buildMenu()
{
    std::vector<MenuItem> children;
    children.push_back(enabledItem());
    children.push_back(toggleKeyItem());

    for (const int index : kMenuOrder) {
        const Part& part = m_parts[static_cast<size_t>(index)];
        Module* const module = part.module;
        MenuItem sub = module->buildMenu();
        for (MenuItem& row : sub.children) {
            const std::wstring label = row.labelText();
            if ((row.kind == MenuItemKind::Keybind && label == L"Toggle key")) {
                continue;
            }
            if (row.kind == MenuItemKind::Toggle && label == L"Enabled") {
                const std::wstring title = part.label;
                row.label = [title] { return title; };
                row.commandName = part.commandName;
                row.isOn = [module] { return module->ownEnabled(); };
            }
            row.available = [module, inner = std::move(row.available)] {
                return module->available() && (!inner || inner());
            };
            row.hidden = row.hidden || module->isWriteBlocked();
            children.push_back(std::move(row));
        }
    }

    MenuItem item = menu::submenu(name(), std::move(children));
    item.available = [this] { return available(); };
    item.isOn = [this] { return enabled(); };
    return item;
}

void JavaUI::loadConfig(const nlohmann::json& section)
{
    Module::loadConfig(section);

    for (const Part& part : m_parts) {
        const std::string key = toUtf8(part.module->name());
        const auto it = section.find(key);
        if (it != section.end() && it->is_object()) {
            part.module->loadConfig(*it);
        } else {
            const nlohmann::json defaults = {{"enabled", true}};
            part.module->loadConfig(defaults);
        }
    }
}

void JavaUI::saveConfig(nlohmann::json& section) const
{
    Module::saveConfig(section);
    for (const Part& part : m_parts) {
        nlohmann::json& sub = section[toUtf8(part.module->name())];
        sub = nlohmann::json::object();
        part.module->saveConfig(sub);
    }
}

void JavaUI::onScansReady()
{
    for (const Part& part : m_parts) {
        part.module->onScansReady();
    }
}

void JavaUI::onUpdate()
{
    for (const Part& part : m_parts) {
        part.module->update();
    }
}

void JavaUI::onEnabledChanged(bool)
{
    for (const Part& part : m_parts) {
        part.module->parentEnabledChanged();
    }
}

void JavaUI::shutdown()
{
    for (auto it = m_parts.rbegin(); it != m_parts.rend(); ++it) {
        it->module->shutdown();
    }
}

}
