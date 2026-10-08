#include "core/Client.h"

#include "game/UiSound.h"

#include "config/Config.h"
#include "config/WriteSwitches.h"
#include "core/FreezeWatch.h"
#include "core/Logger.h"
#include "core/Notice.h"
#include "core/Paths.h"
#include "core/Version.h"
#include "game/UiProbe.h"
#include "game/OreUiPatch.h"
#include "game/SettingsCommand.h"
#include "hooks/Detours.h"
#include "game/BlockRegistry.h"
#include "game/AllowDuplicateKeys.h"
#include "game/ExtendedEnchantLevel.h"
#include "game/ExtendedFov.h"
#include "game/ExtendedStructureSize.h"
#include "game/GameData.h"
#include "hooks/HookManager.h"
#include "input/Foreground.h"
#include "input/GameButtons.h"
#include "memory/Scanner.h"
#include "modules/AntiEffect.h"
#include "modules/AutoTool.h"
#include "modules/CreativeNoClip.h"
#include "modules/FastBlockBreak.h"
#include "modules/FastBlockPlacement.h"
#include "modules/FastInventory.h"
#include "modules/FastUseItem.h"
#include "modules/FlySpeed.h"
#include "modules/FreeCamera.h"
#include "modules/Fullbright.h"
#include "modules/HandRestock.h"
#include "modules/ModuleManager.h"
#include "game/ItemStackRequest.h"
#include "modules/NoBlockAnimation.h"
#include "modules/NoRender.h"
#include "modules/OffhandSwap.h"
#include "modules/Scaffold.h"
#include "modules/Schematica.h"
#include "modules/InventoryHUD.h"
#include "modules/ArmorHUD.h"
#include "modules/JavaUI.h"
#include "modules/DeathLogger.h"
#include "modules/AppleSkin.h"
#include "modules/ShulkerPreview.h"
#include "modules/ToggleSneakSprint.h"
#include "modules/EffectTimer.h"
#include "modules/Zoom.h"
#include "modules/ItemScroller.h"
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

void Client::run()
{
    startup();
    mainLoop();
    shutdown();
}

void Client::registerModules()
{
    ModuleManager::instance().registerModule(&FreeCamera::instance());
    ModuleManager::instance().registerModule(&FastBlockPlacement::instance());
    ModuleManager::instance().registerModule(&FastBlockBreak::instance());
    ModuleManager::instance().registerModule(&AntiEffect::instance());
    ModuleManager::instance().registerModule(&AutoTool::instance());
    ModuleManager::instance().registerModule(&Scaffold::instance());
    ModuleManager::instance().registerModule(&CreativeNoClip::instance());
    ModuleManager::instance().registerModule(&FlySpeed::instance());
    ModuleManager::instance().registerModule(&FastUseItem::instance());
    ModuleManager::instance().registerModule(&HandRestock::instance());
    ModuleManager::instance().registerModule(&OffhandSwap::instance());
    ModuleManager::instance().registerModule(&Schematica::instance());

    ModuleManager::instance().registerModule(&FastInventory::instance());
    ModuleManager::instance().registerModule(&Fullbright::instance());
    ModuleManager::instance().registerModule(&NoRender::instance());
    ModuleManager::instance().registerModule(&Zoom::instance());
    ModuleManager::instance().registerModule(&ItemScroller::instance());
    ModuleManager::instance().registerModule(&ShulkerPreview::instance());
    ModuleManager::instance().registerModule(&InventoryHUD::instance());
    ModuleManager::instance().registerModule(&JavaUI::instance());
    ModuleManager::instance().registerModule(&DeathLogger::instance());
    ModuleManager::instance().registerModule(&AppleSkin::instance());
    ModuleManager::instance().registerModule(&ArmorHUD::instance());
    ModuleManager::instance().registerModule(&ToggleSneakSprint::instance());
    ModuleManager::instance().registerModule(&EffectTimer::instance());
    ModuleManager::instance().registerModule(&NoBlockAnimation::instance());
}

void Client::startup()
{
    m_startupComplete = false;
    log().info(L"Tsukuyomi {} loaded", TSUKUYOMI_VERSION_W);
    log().info(L"Config and log directory: {}", paths::dataDir().wstring());

    Config::instance().load();
    writes::finishStartup();
    if (const wchar_t* const why = oreui::patchFailure(); why != nullptr) {
        notice::failOnce("OreUiPatch.failed",
                         std::wstring(L"OreUiPatch: the settings bundle could not be patched (") + why
                             + L"); key rows are not shown in the settings screen (use /tk set to change keys)",
                         "Key rows are not shown in the settings screen: the settings screen could not be patched (use /tk set to change keys)");
    }

    HookManager::instance().initialize();

    Scanner::instance().scanAll();

    GameData::instance().onScansReady();

    registerModules();
    ModuleManager::instance().loadConfig();
    ModuleManager::instance().applyWriteBlocks();
    loadHotkeys();

    UiSound::instance().onScansReady();

    GameButtons::instance().install();

    allowduplicatekeys::install();
    extendedenchantlevel::install();

    hooks::installAll();

    ModuleManager::instance().onScansReady();
    settingscommand::registerCommand();
    settingscommand::loadPadKeys();

    extendedfov::start();

    extendedstructuresize::start();

    freezewatch::start();

    m_startupComplete = true;
    log().info(L"Settings are in Minecraft's settings screen under Tsukuyomi (END to unload)");
}

void Client::loadHotkeys()
{
    m_unloadKey.useLegacyOnly();
    m_unloadKey.set({VK_END});
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
    m_settingsCheckAt = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!unloadRequested()) {
        pumpThreadMessages();

        if (!m_lateHooksDone) {
            m_lateHooksDone = hooks::installLate();
        }

        worldmesh::report();

        ModuleManager::instance().update();
        extendedfov::update();
        uiprobe::pumpSettingsToggle();

        if (uiprobe::takeSettingsDirty()) {
            m_settingsSaveAt = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            m_settingsSavePending = true;
        }
        if (m_settingsSavePending && std::chrono::steady_clock::now() >= m_settingsSaveAt) {
            m_settingsSavePending = false;
            ModuleManager::instance().saveConfig();
            settingscommand::savePadKeys();
            if (m_startupComplete) Config::instance().pruneUnclaimed();
            if (Config::instance().save()) {
                log().info(L"Saved the changed settings (settings screen / toggle key)");
            }
        }
        if (!m_settingsSavePending && std::chrono::steady_clock::now() >= m_settingsCheckAt) {
            m_settingsCheckAt = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            ModuleManager::instance().saveConfig();
            settingscommand::savePadKeys();
            if (m_startupComplete) Config::instance().pruneUnclaimed();
            if (Config::instance().saveIfChanged()) {
                log().info(L"Saved the settings that changed outside the settings screen");
            }
        }

        const bool unload = m_unloadKey.triggered();
        if (unload && input::isGameForeground()) {
            if (input::isInGameplay()) {
                requestUnload();
            } else {
                log().warn(L"END was ignored: close every screen (back to the world) before unloading");
                ModuleManager::instance().postNotice("\xC2\xA7" "b[Tsukuyomi]" "\xC2\xA7" "r Close the screen before pressing END");
            }
        }

        Sleep(10);
    }
}

void Client::shutdown()
{
    log().info(L"Shutting down");

    freezewatch::stop();
    extendedfov::shutdown();
    extendedstructuresize::shutdown();

    hooks::freezeHookGroups();

    ModuleManager::instance().shutdown();

    ItemStackRequest::instance().waitPendingClose(4000);

    blocks::waitUntilIdle();

    render::shutdownOverlay();

    worldmesh::shutdown();

    uiprobe::restoreKeyRows();

    GameButtons::instance().shutdown();
    HookManager::instance().shutdown();
    uiprobe::removeCrashWatch();

    ModuleManager::instance().saveConfig();

    settingscommand::savePadKeys();
    if (m_startupComplete) Config::instance().pruneUnclaimed();
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
