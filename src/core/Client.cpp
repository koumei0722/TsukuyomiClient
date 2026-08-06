#include "core/Client.h"

#include "config/Config.h"
#include "core/Logger.h"
#include "core/Paths.h"
#include "core/Version.h"
#include "hooks/Detours.h"
#include "hooks/HookManager.h"
#include "input/Foreground.h"
#include "memory/Scanner.h"
#include "modules/AntiDarkness.h"
#include "modules/AutoTool.h"
#include "modules/CreativeNoClip.h"
#include "modules/FastBlockPlacement.h"
#include "modules/FastRightClick.h"
#include "modules/FlySpeed.h"
#include "modules/FreeCamera.h"
#include "modules/GameModeSwitch.h"
#include "modules/HandRestock.h"
#include "modules/ModuleManager.h"
#include "modules/OffhandSwap.h"
#include "modules/Scaffold.h"
#include "render/Overlay.h"

#include <utility>

namespace tsukuyomi {

Client& Client::instance()
{
    static Client client;
    return client;
}

void Client::run(HMODULE self)
{
    m_self = self;

    startup();
    mainLoop();
    shutdown();
}

void Client::registerModules()
{

    ModuleManager::instance().registerModule(&FreeCamera::instance());
    ModuleManager::instance().registerModule(&FastBlockPlacement::instance());
    ModuleManager::instance().registerModule(&AntiDarkness::instance());
    ModuleManager::instance().registerModule(&AutoTool::instance());
    ModuleManager::instance().registerModule(&Scaffold::instance());
    ModuleManager::instance().registerModule(&CreativeNoClip::instance());
    ModuleManager::instance().registerModule(&FlySpeed::instance());
    ModuleManager::instance().registerModule(&FastRightClick::instance());
    ModuleManager::instance().registerModule(&GameModeSwitch::instance());
    ModuleManager::instance().registerModule(&HandRestock::instance());
    ModuleManager::instance().registerModule(&OffhandSwap::instance());
}

void Client::startup()
{
    log().info(L"Tsukuyomi {} loaded", TSUKUYOMI_VERSION_W);
    log().info(L"Config and log directory: {}", paths::dataDir().wstring());

    Config::instance().load();

    HookManager::instance().initialize();

    Scanner::instance().scanAll();

    registerModules();
    ModuleManager::instance().loadConfig();
    loadHotkeys();

    hooks::installAll();

    ModuleManager::instance().onScansReady();

    if (!m_console.create()) {
        log().error(L"Could not open the console. Hotkeys still work");
        return;
    }

    m_console.menu().setRoot(buildRootMenu());

    if (m_consoleKey.empty()) {
        log().warn(L"No console key is set. Press INSERT to show the console");
    } else {
        log().info(L"Press {} to toggle the console", m_consoleKey.name());
    }
}

void Client::loadHotkeys()
{
    const nlohmann::json& section = Config::instance().section("console");

    std::vector<int> combo;
    if (const auto it = section.find("keys"); it != section.end() && it->is_array()) {
        for (const auto& value : *it) {
            if (value.is_number_integer()) {
                combo.push_back(value.get<int>());
            }
        }
    }
    m_consoleKey.set(std::move(combo));

    m_console.setReservedKeys(m_consoleKey.combo());

    m_forceShowKey.set({VK_INSERT});
    m_unloadKey.set({VK_END});

    m_console.setLogVisible(Config::getBool(section, "log", false));

    const auto x = section.find("ox");
    const auto y = section.find("oy");
    if (x != section.end() && x->is_number() && y != section.end() && y->is_number()) {
        m_console.setOrigin(x->get<float>(), y->get<float>());
    }
}

void Client::saveHotkeys()
{
    nlohmann::json& section = Config::instance().section("console");
    section["keys"] = m_consoleKey.combo();
    section["log"] = m_console.logVisible();

    if (m_console.hasOrigin()) {
        section["ox"] = m_console.originX();
        section["oy"] = m_console.originY();
    }

    section.erase("x");
    section.erase("y");
    section.erase("overlay");
}

MenuItem Client::buildRootMenu()
{
    std::vector<MenuItem> items = ModuleManager::instance().buildMenuItems();

    items.push_back(menu::submenu(
        L"Settings",
        {
            menu::back(),
            menu::keybind(
                L"Console key", [this] { return m_consoleKey.combo(); },
                [this](std::vector<int> combo) {
                    m_consoleKey.set(std::move(combo));

                    m_console.setReservedKeys(m_consoleKey.combo());
                    log().info(L"Console key set to {}", m_consoleKey.name());
                }),
            menu::toggle(
                L"Show log", [this] { return m_console.logVisible(); },
                [this] { m_console.setLogVisible(!m_console.logVisible()); }),
            menu::action(L"Save settings", [this] {
                saveHotkeys();
                ModuleManager::instance().saveConfig();
                if (Config::instance().save()) {
                    log().success(L"Settings saved");
                }
            }),
        }));

    items.push_back(menu::action(L"Unload (END)", [this] { requestUnload(); }));

    return menu::submenu(L"Tsukuyomi", std::move(items));
}

void Client::mainLoop()
{
    while (!unloadRequested()) {
        m_console.pump();
        ModuleManager::instance().update();

        const bool toggleConsole = m_consoleKey.triggered();
        const bool forceShow = m_forceShowKey.triggered();
        const bool unload = m_unloadKey.triggered();

        if (input::isGameForeground()) {
            if (toggleConsole) {
                m_console.toggle();
            }
            if (forceShow) {
                m_console.show();
            }
            if (unload) {
                requestUnload();
            }
        }

        Sleep(10);
    }
}

void Client::shutdown()
{
    log().info(L"Shutting down");

    log().setNotifier(nullptr);

    ModuleManager::instance().shutdown();
    m_console.destroy();
    render::shutdownOverlay();
    HookManager::instance().shutdown();

    saveHotkeys();
    ModuleManager::instance().saveConfig();
    Config::instance().save();
}

void Client::requestUnload()
{
    m_unloadRequested.store(true, std::memory_order_relaxed);
}

bool Client::unloadRequested() const
{
    return m_unloadRequested.load(std::memory_order_relaxed);
}

}
