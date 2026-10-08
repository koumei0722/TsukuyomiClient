#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "modules/Module.h"

namespace tsukuyomi {

class ModuleManager {
public:
    static ModuleManager& instance();

    void registerModule(Module* module);
    const std::vector<Module*>& modules() const { return m_modules; }

    void loadConfig();
    void saveConfig();
    void applyWriteBlocks();
    void onScansReady();
    void update();
    void shutdown();

    std::vector<MenuItem> buildMenuItems();

    void postToggleNotice(const wchar_t* moduleName, bool enabled);
    void postNotice(std::string text);
    void pumpToggleNotices();

private:
    ModuleManager() = default;

    std::vector<Module*> m_modules;

    std::mutex m_noticeMutex;
    std::vector<std::string> m_notices;
};

}
