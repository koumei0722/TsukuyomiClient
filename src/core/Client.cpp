#include "core/Client.h"

#include "game/UiSound.h"

#include "config/Config.h"
#include "core/Logger.h"
#include "core/Paths.h"
#include "core/Version.h"
#include "game/UiProbe.h"
#include "hooks/Detours.h"
#include "game/BlockRegistry.h"
#include "hooks/HookManager.h"
#include "input/Foreground.h"
#include "memory/Scanner.h"
#include "modules/AntiDarkness.h"
#include "modules/AutoTool.h"
#include "modules/CreativeNoClip.h"
#include "modules/FastBlockPlacement.h"
#include "modules/FastInventory.h"
#include "modules/FastRightClick.h"
#include "modules/FlySpeed.h"
#include "modules/FreeCamera.h"
#include "modules/Fullbright.h"
#include "modules/GameModeSwitch.h"
#include "modules/HandRestock.h"
#include "modules/ModuleManager.h"
#include "modules/NoRender.h"
#include "modules/OffhandSwap.h"
#include "modules/Scaffold.h"
#include "modules/Schematica.h"
#include "modules/Zoom.h"
#include "render/BoxRenderer.h"
#include "render/DiffAtlas.h"
#include "render/FrameTrace.h"
#include "render/GhostLayer.h"
#include "render/Overlay.h"
#include "render/WorldMesh.h"

#include <chrono>
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
    ModuleManager::instance().registerModule(&HandRestock::instance());
    ModuleManager::instance().registerModule(&OffhandSwap::instance());
    ModuleManager::instance().registerModule(&Schematica::instance());

    ModuleManager::instance().registerModule(&FastInventory::instance());
    ModuleManager::instance().registerModule(&Fullbright::instance());
    ModuleManager::instance().registerModule(&NoRender::instance());
    ModuleManager::instance().registerModule(&Zoom::instance());
}

void Client::startup()
{
    log().info(L"Tsukuyomi {} loaded", TSUKUYOMI_VERSION_W);
    log().info(L"Config and log directory: {}", paths::dataDir().wstring());

    hooks::beModelsOn();

    Config::instance().load();

    HookManager::instance().initialize();

    Scanner::instance().scanAll();

    registerModules();
    ModuleManager::instance().loadConfig();
    loadHotkeys();

    uiprobe::installCrashProbe();

    UiSound::instance().onScansReady();

    hooks::installAll();

    ModuleManager::instance().onScansReady();

    log().info(L"Settings are in Minecraft's settings screen under Tsukuyomi (END to unload)");
}

void Client::loadHotkeys()
{
    m_unloadKey.set({VK_END});

    Config::instance().eraseSection("console");
}

void Client::saveHotkeys()
{
}

namespace {

void pumpThreadMessages()
{
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

}

void Client::mainLoop()
{
    while (!unloadRequested()) {
        pumpThreadMessages();

        if (!m_lateHooksDone) {
            m_lateHooksDone = hooks::installLate();
        }

        frametrace::pump();
        worldmesh::report();
        atlas::report();

        ModuleManager::instance().update();
        uiprobe::pumpSettingsToggle();

        if (uiprobe::takeSettingsDirty()) {
            m_settingsSaveAt = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            m_settingsSavePending = true;
        }
        if (m_settingsSavePending && std::chrono::steady_clock::now() >= m_settingsSaveAt) {
            m_settingsSavePending = false;
            ModuleManager::instance().saveConfig();
            if (Config::instance().save()) {
                log().info(L"Saved the values changed in the settings screen");
            }
        }

        const bool unload = m_unloadKey.triggered();
        if (unload && input::isGameForeground()) {
            requestUnload();
        }

        Sleep(10);
    }
}

void Client::shutdown()
{
    log().info(L"Shutting down");

    log().setNotifier(nullptr);

    ModuleManager::instance().shutdown();

    blocks::waitUntilIdle();

    render::shutdownOverlay();

    ghost::shutdownGhostLayer();
    boxes::shutdownDepth();
    frametrace::shutdown();
    worldmesh::shutdown();
    atlas::shutdown();

    uiprobe::restoreKeyRows();

    hooks::restoreMaterialBlend();

    HookManager::instance().shutdown();

    uiprobe::removeCrashProbe();

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
