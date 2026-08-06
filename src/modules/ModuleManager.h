#pragma once

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
    void onScansReady();
    void update();
    void shutdown();

    std::vector<MenuItem> buildMenuItems();

private:
    ModuleManager() = default;

    std::vector<Module*> m_modules;
};

}
