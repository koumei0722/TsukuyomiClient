#include "modules/ModuleManager.h"

#include "config/Config.h"
#include "core/Strings.h"
#include "game/ClientChat.h"

#include <algorithm>
#include <utility>

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
        nlohmann::json& section = Config::instance().section(key);
        nlohmann::json fresh = nlohmann::json::object();
        if (const auto pad = section.find("padKeys"); pad != section.end()) {
            fresh["padKeys"] = *pad;
        }
        module->saveConfig(fresh);
        section = std::move(fresh);
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

void ModuleManager::postToggleNotice(const wchar_t* moduleName, bool enabled)
{
    std::string text = "\xC2\xA7" "b[Tsukuyomi]" "\xC2\xA7" "r " + toUtf8(moduleName)
        + (enabled ? " \xC2\xA7" "aON" : " \xC2\xA7" "cOFF");
    postNotice(std::move(text));
}

void ModuleManager::postNotice(std::string text)
{
    const std::lock_guard<std::mutex> lock(m_noticeMutex);
    if (m_notices.size() < 16) {
        m_notices.push_back(std::move(text));
    }
}

void ModuleManager::pumpToggleNotices()
{
    std::vector<std::string> notices;
    {
        const std::lock_guard<std::mutex> lock(m_noticeMutex);
        if (m_notices.empty()) {
            return;
        }
        notices.swap(m_notices);
    }
    for (const std::string& text : notices) {
        clientchat::printLocal(text);
    }
}

void ModuleManager::shutdown()
{
    for (auto it = m_modules.rbegin(); it != m_modules.rend(); ++it) {
        (*it)->shutdown();
    }
}

void ModuleManager::applyWriteBlocks()
{
    for (Module* module : m_modules) {
        module->applyWriteBlock();
    }
}

std::vector<MenuItem> ModuleManager::buildMenuItems()
{
    std::vector<MenuItem> items;
    items.reserve(m_modules.size());
    for (Module* module : m_modules) {
        items.push_back(module->buildMenu());
        if (module->isWriteBlocked()) {
            items.back().hidden = true;
        }
    }
    return items;
}

}
