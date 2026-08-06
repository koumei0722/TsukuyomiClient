#include "modules/ModuleManager.h"

#include "config/Config.h"
#include "core/Logger.h"
#include "core/Strings.h"

#include <algorithm>

namespace tsukuyomi {

ModuleManager& ModuleManager::instance()
{
    static ModuleManager manager;
    return manager;
}

void ModuleManager::registerModule(Module* module)
{
    if (module == nullptr) {
        return;
    }
    if (std::find(m_modules.begin(), m_modules.end(), module) != m_modules.end()) {
        return;
    }
    m_modules.push_back(module);
}

void ModuleManager::loadConfig()
{
    for (Module* module : m_modules) {
        const std::string key = toUtf8(module->name());
        module->loadConfig(Config::instance().section(key));
    }
}

void ModuleManager::saveConfig()
{
    for (const Module* module : m_modules) {
        const std::string key = toUtf8(module->name());
        module->saveConfig(Config::instance().section(key));
    }
}

void ModuleManager::onScansReady()
{
    for (Module* module : m_modules) {
        module->onScansReady();
    }
}

void ModuleManager::update()
{
    for (Module* module : m_modules) {
        module->update();
    }
}

void ModuleManager::shutdown()
{

    for (auto it = m_modules.rbegin(); it != m_modules.rend(); ++it) {
        (*it)->shutdown();
    }
}

std::vector<MenuItem> ModuleManager::buildMenuItems()
{
    std::vector<MenuItem> items;
    items.reserve(m_modules.size());
    for (Module* module : m_modules) {
        items.push_back(module->buildMenu());
    }
    return items;
}

}
