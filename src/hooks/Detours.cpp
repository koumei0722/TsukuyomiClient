#include "game/SchematicWorld.h"
#include "hooks/Detours.h"

#include "config/WriteSwitches.h"

#include "core/FreezeWatch.h"
#include "core/Logger.h"
#include "core/Notice.h"
#include "game/BlockRegistry.h"
#include "game/BlockWrite.h"
#include "game/InventoryResync.h"
#include "game/ChatCommand.h"
#include "game/AvailableCommandsPatch.h"
#include "game/ContainerUi.h"
#include "game/DebugScreenWorld.h"
#include "game/FmodStats.h"
#include "game/TradeUi.h"
#include "game/GameData.h"
#include "game/WorldIdentity.h"
#include "game/SessionTickLayout.h"
#include "game/UiSound.h"
#include "game/InventoryScreen.h"
#include "game/ItemStackRequest.h"
#include "game/UiProbe.h"
#include "game/OreUiScript.h"
#include "game/CustomGuiScale.h"
#include "game/CraftAmount.h"
#include "input/GameButtons.h"
#include "hooks/HookManager.h"
#include "input/GameInput.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"
#include "modules/ModuleManager.h"
#include "modules/DebugScreen.h"
#include "modules/DebugKeys.h"
#include "game/GameModeState.h"
#include "game/UiReload.h"
#include "game/GameModeWheel.h"
#include "game/PlayerContext.h"
#include "modules/AngleSnap.h"
#include "modules/AntiEffect.h"
#include "modules/AutoTool.h"
#include "modules/CreativeNoClip.h"
#include "modules/FastBlockBreak.h"
#include "modules/FastBlockPlacement.h"
#include "modules/FastUseItem.h"
#include "game/FlySpeedCommand.h"
#include "modules/HandRestock.h"
#include "modules/OffhandSlot.h"
#include "modules/PlayerList.h"
#include "modules/DeathLogger.h"
#include "bot/BotHooks.h"
#include "bot/BotModule.h"
#include "modules/AppleSkin.h"
#include "modules/EffectTimer.h"
#include "modules/ArmorHUD.h"
#include "modules/OffhandSwap.h"
#include "modules/FastInventory.h"
#include "modules/ItemScroller.h"
#include "modules/FreeCamera.h"
#include "modules/Fullbright.h"
#include "modules/NoRender.h"
#include "modules/OverworldSky.h"
#include "modules/ToggleSneakSprint.h"
#include "modules/Zoom.h"
#include "modules/Scaffold.h"
#include "modules/Schematica.h"
#include "modules/FlexiblePlacement.h"
#include "modules/IgnoreBlock.h"
#include "modules/LightOverlay.h"
#include "modules/LocatorBar.h"
#include "modules/NameTags.h"
#include "modules/Waypoints.h"
#include "game/LookPlacement.h"
#include "game/Rejoin.h"
#include "modules/ShulkerPreview.h"
#include "render/BoxRenderer.h"
#include "render/Overlay.h"
#include "render/WorldMesh.h"

#include <Windows.h>

#include <intrin.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace tsukuyomi {

namespace hooks {
static void refreshHookGroups();
}

std::atomic<unsigned long long> g_lastLeadingViewAt{0};

using SendCommandFn = void(__fastcall*)(void*, std::int32_t*, const void*, std::int32_t);
SendCommandFn g_sendCommandRequest = nullptr;
availablecommands::LoadPacketFn g_commandRegistryLoadPacket = nullptr;
void __fastcall detourCommandRegistryLoadPacket(void* registry, const void* packet)
{
    availablecommands::loadWithTk(g_commandRegistryLoadPacket, registry, packet);
}
using CommandAutoCompleteFn = void*(__fastcall*)(void* registry, void* out, const void* origin, const void* text,
                                                  std::uint32_t cursor);
using CommandAutoCompleteFilterFn = bool(__fastcall*)(void* registry, const void* suggestion, const void* originFlag,
                                                      bool cheatsEnabled, bool playerFlag);
CommandAutoCompleteFn g_commandAutoComplete = nullptr;
CommandAutoCompleteFilterFn g_commandAutoCompleteFilter = nullptr;
void* __fastcall detourCommandAutoComplete(void* registry, void* out, const void* origin, const void* text,
                                           std::uint32_t cursor)
{
    availablecommands::ensureTk(g_commandRegistryLoadPacket, registry);
    chatcommand::noteCompletionText(text);
    return g_commandAutoComplete(registry, out, origin, text, cursor);
}
bool __fastcall detourCommandAutoCompleteFilter(void* registry, const void* suggestion, const void* originFlag,
                                                bool cheatsEnabled, bool playerFlag)
{
    const bool keep = g_commandAutoCompleteFilter(registry, suggestion, originFlag, cheatsEnabled, playerFlag);
    if (!chatcommand::completingTsukuyomiArguments()) {
        return keep;
    }
    return chatcommand::allowTsukuyomiSuggestion(suggestion);
}

void __fastcall detourSendCommandRequest(void* sender, std::int32_t* out,
                                         const void* command, std::int32_t flags)
{
    if (chatcommand::intercept(command, out)) return;
    if (g_sendCommandRequest != nullptr) g_sendCommandRequest(sender, out, command, flags);
}

extern "C" {

void tsukuyomiCameraTrampolineEntry();
void* tsukuyomiCameraTrampoline = nullptr;

void tsukuyomiPlayerViewTrampolineEntry();
void* tsukuyomiPlayerViewTrampoline = nullptr;

void tsukuyomiPacketSendTrampolineEntry();
void* tsukuyomiPacketSendTrampoline = nullptr;

void tsukuyomiSessionTickFirstEntry();
void tsukuyomiSessionTickCatchupEntry();
void* tsukuyomiSessionTickFirstTrampoline = nullptr;
void* tsukuyomiSessionTickCatchupTrampoline = nullptr;

void tsukuyomiCameraHook(void* cameraBase, void* source, void* frame)
{
    FreeCamera::instance().onCameraWrite(cameraBase, frame);

    boxes::noteCamera(cameraBase, source);
}

using ViewVectorFn = void*(__fastcall*)(void* actor, float* out, float partial);
ViewVectorFn g_viewVector = nullptr;

void* __fastcall detourViewVector(void* actor, float* out, float partial)
{
    void* const result = (g_viewVector != nullptr) ? g_viewVector(actor, out, partial) : nullptr;
    if (!FreeCamera::instance().active() && !GameData::instance().hasLivePlayer()) {
        GameData::instance().adoptPlayerFromEntity(actor);
    }
    if (FreeCamera::instance().freezesAim() && GameData::instance().isPlayerEntity(actor)) {
        FreeCamera::instance().freezeViewVector(out);
    }
    return result;
}

using CameraDirectLookTurnFn = void(__fastcall*)(void*, void*, float*, const float*);
CameraDirectLookTurnFn g_cameraDirectLookTurn = nullptr;

void __fastcall detourCameraDirectLookTurn(void* rcx, void* rdx, float* look, const float* delta)
{
    AngleSnap& snap = AngleSnap::instance();
    float before[3]{};
    const bool snapping = snap.active() && look != nullptr && memory::copyGuarded(look, before, sizeof(before));
    g_cameraDirectLookTurn(rcx, rdx, look, delta);
    if (snapping) {
        snap.afterTurn(look, before);
    }
}

using CameraOrbitTurnFn = void(__fastcall*)(void*, void*, float*, const float*);
CameraOrbitTurnFn g_cameraOrbitTurn = nullptr;

void __fastcall detourCameraOrbitTurn(void* rcx, void* rdx, float* orbit, const float* delta)
{
    AngleSnap& snap = AngleSnap::instance();
    float before[2]{};
    const bool snapping =
        snap.active() && orbit != nullptr && memory::copyGuarded(orbit + 0x34 / sizeof(float), before, sizeof(before));
    g_cameraOrbitTurn(rcx, rdx, orbit, delta);
    if (snapping) {
        snap.afterOrbitTurn(orbit, before);
    }
}

using DismountButtonFn = void(__fastcall*)(void*);
DismountButtonFn g_dismountButton = nullptr;

void __fastcall detourDismountButton(void* client)
{
    if (FreeCamera::instance().blocksDismount()) {
        return;
    }
    g_dismountButton(client);
}

using HotbarScrollButtonFn = void(__fastcall*)(void*, int);
HotbarScrollButtonFn g_hotbarScrollButton = nullptr;

void __fastcall detourHotbarScrollButton(void* client, int direction)
{
    if (FreeCamera::instance().blocksHotbarScroll()) {
        return;
    }
    g_hotbarScrollButton(client, direction);
}

using HudHotbarScrollFn = std::uint32_t(__fastcall*)(void*, void*);
HudHotbarScrollFn g_hudHotbarScrollNext = nullptr;
HudHotbarScrollFn g_hudHotbarScrollPrev = nullptr;

std::uint32_t __fastcall detourHudHotbarScrollNext(void* self, void* event)
{
    if (FreeCamera::instance().blocksHotbarScroll()) {
        return 0;
    }
    return g_hudHotbarScrollNext(self, event);
}

std::uint32_t __fastcall detourHudHotbarScrollPrev(void* self, void* event)
{
    if (FreeCamera::instance().blocksHotbarScroll()) {
        return 0;
    }
    return g_hudHotbarScrollPrev(self, event);
}

namespace {
constexpr unsigned kAdoptAfter = 32;
void* g_viewLeader = nullptr;
unsigned g_viewMisses = 0;

bool isLeadingView(void* viewBase)
{
    if (viewBase != g_viewLeader) {
        if (++g_viewMisses <= kAdoptAfter) {
            return false;
        }
        g_viewLeader = viewBase;
    }
    g_viewMisses = 0;
    return true;
}
}

void tsukuyomiPlayerViewHook(void* viewBase)
{
    if (!isLeadingView(viewBase)) {
        return;
    }
    const freezewatch::Scope freezeScope{"PlayerView (game thread)"};
    hooks::refreshHookGroups();

    blocks::setSimThread(GetCurrentThreadId());

    constexpr size_t kViewSize = sizeof(float) * 5;
    float fields[5];
    if (!memory::copyGuarded(viewBase, fields, kViewSize)) {
        return;
    }

    PlayerView view;
    view.x = fields[0];
    view.y = fields[1];
    view.z = fields[2];
    view.pitch = fields[3];
    view.yaw = fields[4];

    const bool finite = std::isfinite(view.x) && std::isfinite(view.y) && std::isfinite(view.z)
                        && std::isfinite(view.pitch) && std::isfinite(view.yaw);
    if (!finite) {
        return;
    }

    constexpr float kWorldLimit = 3.0e7f;
    if (std::fabs(view.x) > kWorldLimit || std::fabs(view.y) > kWorldLimit
        || std::fabs(view.z) > kWorldLimit) {
        return;
    }

    GameData::instance().setPlayerView(view);
    g_lastLeadingViewAt.store(GetTickCount64(), std::memory_order_relaxed);

    static std::atomic<bool> announced{false};
    if (!announced.exchange(true)) {
        log().info(L"Entered a world (view updates are arriving)");
    }

    AutoTool::instance().onPlayerViewUpdate();

    GameModeState::instance().onPlayerView(GetTickCount64());

    UiSound::instance().pump();
    ModuleManager::instance().pumpToggleNotices();

    ItemStackRequest::instance().onFrame();
    InventoryResync::instance().onPlayerViewUpdate();

    chatcommand::pump();
    GameButtons::instance().pumpNotice();
    uireload::pump();
    notice::pump();
    HandRestock::instance().onPlayerViewUpdate();

    OffhandSwap::instance().onPlayerViewUpdate();

    ItemScroller::instance().onPlayerViewUpdate();

    static std::uint64_t placementGeneration = 0;
    const auto generation = blockwrite::regionGeneration();
    if (generation != placementGeneration) { placementGeneration = generation; lookplace::cancel(); }
    lookplace::pump();
    Schematica::instance().onPlayerViewUpdate();
    FlexiblePlacement::instance().onPlayerViewUpdate();
    IgnoreBlock::instance().onPlayerViewUpdate();
    LightOverlay::instance().onPlayerViewUpdate();
    LocatorBar::instance().onPlayerViewUpdate();
    Waypoints::instance().onPlayerViewUpdate();
    FreeCamera::instance().onPlayerViewUpdate();

    DebugScreen::instance().onPlayerViewUpdate();
    PlayerList::instance().onPlayerViewUpdate();
    DeathLogger::instance().onPlayerViewUpdate();
    Bot::instance().onPlayerViewUpdate();
    AppleSkin::instance().onPlayerViewUpdate();
    DebugKeys::instance().onPlayerViewUpdate();
    NameTags::instance().onPlayerViewUpdate();
    ArmorHUD::instance().onPlayerViewUpdate();

    uiprobe::pumpMenuSelection();

    uiprobe::pumpControlsKeybind();
}

void zeroAuthInputMove(void* packet)
{
    constexpr std::size_t kMoveAt = 0x080;
    constexpr std::size_t kFlagsAt = 0x014;

    auto* const base = static_cast<std::byte*>(packet);
    constexpr float kZeroMove[2]{};
    memory::writeGuarded(base + kMoveAt, kZeroMove, sizeof(kZeroMove));
    constexpr std::uint32_t kZeroFlags = 0;
    memory::writeGuarded(base + 0x088, &kZeroFlags, sizeof(kZeroFlags));
    memory::writeGuarded(base + kFlagsAt, &kZeroFlags, sizeof(kZeroFlags));
}

int tsukuyomiOnPacketSend(void* packet, void* sender)
{
    if (hooks::dryRunActive()) { hooks::noteDryPacket(); return 1; }
    bot::onPacketSend(packet, sender);
    lookplace::onPacketSend(packet);
    if (FreeCamera::instance().active()
        && ItemStackRequest::packetId(packet) == 0x90) {
        zeroAuthInputMove(packet);
    }

    ItemStackRequest::instance().observePacket(packet);
    return 0;
}

}

}

namespace tsukuyomi::hooks {

namespace {

using GetDestroySpeedFn = float(__fastcall*)(void*, void*, void*, void*);
using SetSelectedSlotFn = void(__fastcall*)(void*, void*, void*, void*);
using BuildBlockFn = bool(__fastcall*)(void*, void*, unsigned char, unsigned char, bool);
using MobSwingFn = bool(__fastcall*)(void*, unsigned char, unsigned char);
using AbilitiesAccessFn = std::byte*(__fastcall*)(void*);
using PoseDecisionFn = void(__fastcall*)(void*, void*, void*, void*, void*, std::uint64_t*, void*, void*,
                                          void*, void*);

using UseItemFn = int(__fastcall*)(void*, void*, int);

using UseItemTransactionFn = int(__fastcall*)(void*, void*, int);

using SetGameModeFn = void(__fastcall*)(void*, int, int);

using NotifyInventoryOpenFn = void(__fastcall*)(void*, int);

using MoveInputHandlerFn = void*(__fastcall*)(void*, void*, void*, void*);

using GetActorEffectFn = void*(__fastcall*)(void*, int);

using OpenInventoryScreenFn = void(__fastcall*)(void*);

using ScreenContextFn = void*(__fastcall*)(void*);

using ContainerOpenHandleFn = void(__fastcall*)(void*, void*, void*, void*);

using InventoryContentReadFn = void*(__fastcall*)(void*, void*, void*, void*);

using HandleItemStackResponseFn = void(__fastcall*)(void*, void*, void*, void*);
using CraftFromOutputFn = void*(__fastcall*)(void*, void*, void*, void*, void*);

using InventoryHoveredSlotFn = void*(__fastcall*)(void*, void*);

using UiDefLookupFn = void*(__fastcall*)(void*, const void*, const void*);

using SettingsGroupResolveFn = void*(__fastcall*)(void*, void*, const void*, void*);
using SettingsGroupRegisterFn = void*(__fastcall*)(void*, const void*, void*, void*);

using OpenHowToPlayScreenFn = void(__fastcall*)(void*);

using SettingsProviderCallFn = void*(__fastcall*)(void*, void*, void*, void*);

using SettingsFindComponentFn = void*(__fastcall*)(void*, void*, const void*, void*);

using GameAllocateFn = void*(__fastcall*)(void*, size_t);

using OreKeyRowsBuildFn = void*(__fastcall*)(void*, void*, void*);

using I18nGetFn = void*(__fastcall*)(void*, void*, const void*, void*);

using UiEventDispatchFn = int(__fastcall*)(void*, const void*, void*, void*);

GetDestroySpeedFn g_getDestroySpeed = nullptr;
SetSelectedSlotFn g_setSelectedSlot = nullptr;
BuildBlockFn g_buildBlock = nullptr;
std::uint8_t g_keptClickFlag = 0;
std::uint8_t g_keptClickPoint = 0;
MobSwingFn g_mobSwing = nullptr;
thread_local bool t_suppressSwing = false;
thread_local bool t_modBuild = false;
using GameModeContinueDestroyBlockFn = bool(__fastcall*)(void*, const void*, std::uint8_t,
                                                          const void*, bool*);
using GameModeDestroyBlockFn = bool(__fastcall*)(void*, const void*, std::uint8_t);
GameModeContinueDestroyBlockFn g_gameModeContinueDestroyBlock = nullptr;
GameModeDestroyBlockFn g_gameModeDestroyBlock = nullptr;
AbilitiesAccessFn g_abilitiesAccess = nullptr;
PoseDecisionFn g_poseDecision = nullptr;
UseItemFn g_useItem = nullptr;
UseItemTransactionFn g_useItemTransaction = nullptr;
SetGameModeFn g_setGameMode = nullptr;
NotifyInventoryOpenFn g_notifyInventoryOpen = nullptr;
MoveInputHandlerFn g_moveInputHandler = nullptr;
using InputGatherFn = void*(__fastcall*)(void*, void*, void*, void*);
InputGatherFn g_inputGather = nullptr;
using MoveIntentFn = void*(__fastcall*)(void*, void*, void*, void*, std::uint64_t, void*);
MoveIntentFn g_moveIntent = nullptr;
GetActorEffectFn g_getActorEffect = nullptr;
OpenInventoryScreenFn g_openInventoryScreen = nullptr;
ContainerOpenHandleFn g_containerOpenHandle = nullptr;
InventoryContentReadFn g_inventoryContentRead = nullptr;
HandleItemStackResponseFn g_handleItemStackResponse = nullptr;
CraftFromOutputFn g_craftFromOutput = nullptr;
InventoryHoveredSlotFn g_inventoryHoveredSlot = nullptr;
containerui::SmHandleFn g_containerSm = nullptr;
using ContainerScreenDtorFn = void*(__fastcall*)(void*);
ContainerScreenDtorFn g_containerScreenDtor = nullptr;
using ContainerScreenTickFn = std::uint32_t(__fastcall*)(void*);
ContainerScreenTickFn g_containerScreenTick = nullptr;
using ContainerScreenCtorFn = void*(__fastcall*)(void*, void*, void*, void*);
ContainerScreenCtorFn g_containerScreenCtor = nullptr;
using HudScreenCtorFn = void*(__fastcall*)(void*, void*, void*, void*);
HudScreenCtorFn g_hudScreenCtor = nullptr;

using RenderCurrentFrameFn = void(__fastcall*)(void* self, float delta);
RenderCurrentFrameFn g_renderCurrentFrame = nullptr;

using ServerLevelTickFn = void(__fastcall*)(void* level);
ServerLevelTickFn g_serverLevelTick = nullptr;
const void* g_sessionTickReturns[2]{};
bool g_sessionTicksReady = false;
thread_local int g_sessionTickBranch = 0;
thread_local LONGLONG g_sessionTickStart = 0;

using PacketCheckSizeFn = std::uint64_t(__fastcall*)(void* self, void* a2, void* a3,
                                                     unsigned int receiverIsServer);
PacketCheckSizeFn g_packetCheckSize = nullptr;

using NetworkSendFn = std::uint64_t(__fastcall*)(void* self, void* a2, void* a3, unsigned int a4);
NetworkSendFn g_networkSend = nullptr;
using StartGameHandleFn = void(__fastcall*)(void*, const void*, const void*);
StartGameHandleFn g_startGameHandle = nullptr;
using LevelChunkTickFn = std::uintptr_t(__fastcall*)(void*, void*, void*, void*);
LevelChunkTickFn g_levelChunkTick = nullptr;
using ContainerGetItemFn = const void*(__fastcall*)(void*, const void*, int);
ContainerGetItemFn g_containerGetItem = nullptr;
using HudCollResolveFn = bool(__fastcall*)(void*, void*, std::uintptr_t, std::uintptr_t, void*, std::uintptr_t, void*,
                                           void*);
HudCollResolveFn g_hudCollResolve = nullptr;
HudCollResolveFn g_hudCollBase = nullptr;
using TradeInvokeFn = int(__fastcall*)(void*, void* const*);
TradeInvokeFn g_tradeHoverInvoke = nullptr;
TradeInvokeFn g_tradeSecondaryInvoke = nullptr;
using TradeCurrentTierFn = int(__fastcall*)(void*);
TradeCurrentTierFn g_tradeCurrentTier = nullptr;
using TradeSelParseFn = void*(__fastcall*)(void*, const void*);
using TradeTierCountFn = int(__fastcall*)(void*);
using TradeTierIntFn = int(__fastcall*)(void*, const int*);
using TradeTierBoolFn = bool(__fastcall*)(void*, const int*);
using TradeTierNameFn = void*(__fastcall*)(void*, void*, const int*);
TradeSelParseFn g_tradeSelParse = nullptr;
TradeTierCountFn g_tradeSelectorTotal = nullptr;
TradeTierIntFn g_tradeTierTotal = nullptr;
TradeTierBoolFn g_tradeTierVisible = nullptr;
TradeTierBoolFn g_tradeTierUnlocked = nullptr;
TradeTierNameFn g_tradeTierName = nullptr;
UiDefLookupFn g_uiDefLookup = nullptr;
UiEventDispatchFn g_uiEventDispatch = nullptr;
SettingsGroupResolveFn g_settingsGroupResolve = nullptr;
SettingsGroupRegisterFn g_settingsGroupRegister = nullptr;
OpenHowToPlayScreenFn g_openHowToPlayScreen = nullptr;

std::atomic<void*> g_clientInstance{nullptr};
SettingsProviderCallFn g_settingsProviderCall = nullptr;
SettingsFindComponentFn g_settingsFindComponent = nullptr;
void** g_gameAllocatorGlobal = nullptr;
std::size_t g_gameAllocateSlot = 0;
using FogSettingsFetchFn = void*(__fastcall*)(void*, void*, void*, void*);
FogSettingsFetchFn g_fogSettingsFetch = nullptr;
FogSettingsFetchFn g_fogColorFetch = nullptr;
using LegacyParticleInsertFn = void(__fastcall*)(void* frameBuilder, void* description);
LegacyParticleInsertFn g_legacyParticleInsert = nullptr;
using LegacyParticleRenderFn = void*(__fastcall*)(void* engine, void* out, void* a3, void* a4, void* a5,
                                                  float a6);
LegacyParticleRenderFn g_legacyParticleRender = nullptr;
using FmodSystemUpdateFn = int(__fastcall*)(void* system);
FmodSystemUpdateFn g_fmodSystemUpdate = nullptr;
using ItemHoverTextBuildFn = void*(__fastcall*)(const void* stack, void* out, void* level, bool flag);
ItemHoverTextBuildFn g_itemHoverTextBuild = nullptr;
using HoverRendererRenderFn = void(__fastcall*)(void* self, void* ctx, void* client, void* owner, int pass);
HoverRendererRenderFn g_hoverRendererRender = nullptr;
HoverRendererRenderFn g_mobEffectsRendererRender = nullptr;
LocatorBar::RenderFn g_locatorBarRendererRender = nullptr;
LocatorBar::IconFn g_locatorBarIcon = nullptr;
NameTags::BuildTagFn g_nameTagBuild = nullptr;
using MobEffectsLayoutFn = void(__fastcall*)(void* layout);
MobEffectsLayoutFn g_mobEffectsLayout = nullptr;
using HudRendererUpdateFn = std::uintptr_t(__fastcall*)(void* self, void* a, void* b, void* c);
HudRendererUpdateFn g_hungerRendererUpdate = nullptr;
HudRendererUpdateFn g_heartRendererUpdate = nullptr;
using AttackCoreFn = bool(__fastcall*)(void* gameMode, void* target, bool direct, const void* hitPos);
AttackCoreFn g_attackCore = nullptr;
using SendComplexTxFn = void(__fastcall*)(void* player, void** transaction);
SendComplexTxFn g_sendComplexTx = nullptr;
std::uintptr_t g_legacyParticleSubmitBegin = 0;
std::uintptr_t g_legacyParticleSubmitEnd = 0;
OreKeyRowsBuildFn g_oreKeyRowsBuild = nullptr;
I18nGetFn g_i18nGet = nullptr;
void* g_i18nSelf = nullptr;
using I18nGetWithParamsFn = void*(__fastcall*)(void*, void*, const void*, const void*, const void*);
I18nGetWithParamsFn g_i18nGetWithParams = nullptr;

float __fastcall detourGetDestroySpeed(void* rcx, void* rdx, void* r8, void* r9)
{
    return AutoTool::instance().onGetDestroySpeed(rcx, rdx, r8, r9);
}

void __fastcall detourSetSelectedSlot(void* rcx, void* rdx, void* r8, void* r9)
{
    const void* const returnAddress = _ReturnAddress();

    if (Zoom::instance().suppressHotbar(returnAddress)) {
        return;
    }
    if (FastBlockPlacement::instance().suppressHotbarSlot(static_cast<int>(reinterpret_cast<std::uintptr_t>(rdx) & 0xFFFFFFFFu))) {
        return;
    }

    HandRestock::instance().onSetSelectedSlot(rcx);

    AutoTool::instance().onSetSelectedSlot(rcx, rdx, r8, r9);
}

void* __fastcall detourCraftFromOutput(void* ctrl, void* out, void* collection, void* amount, void* results)
{
    craftamount::apply(amount);
    return g_craftFromOutput(ctrl, out, collection, amount, results);
}

void __fastcall detourHandleItemStackResponse(void* rcx, void* rdx, void* r8, void* r9)
{
    ItemStackRequest::instance().onResponse(rdx);

    if (g_handleItemStackResponse != nullptr) {
        g_handleItemStackResponse(rcx, rdx, r8, r9);
    }
}

bool __fastcall detourBuildBlock(void* gameMode, void* blockPos, unsigned char face,
                                 unsigned char extra, bool simTick)
{
    GameData::instance().setGameMode(gameMode);
    if (t_modBuild) return hooks::callBuildBlock(gameMode, blockPos, face, extra, simTick);
    bool handled = false;
    const bool ignoreResult = IgnoreBlock::instance().onBuildBlock(blockPos, handled);
    if (handled) return ignoreResult;
    const bool result = Schematica::instance().onBuildBlock(gameMode, blockPos, face, extra, simTick, handled);
    if (handled) return result;

    const bool flexibleResult = FlexiblePlacement::instance().onBuildBlock(gameMode, blockPos, face, extra, simTick, handled);
    if (handled) return flexibleResult;

    return FastBlockPlacement::instance().onBuildBlock(gameMode, blockPos, face, extra, simTick);
}

bool __fastcall detourMobSwing(void* mob, unsigned char source, unsigned char r8)
{
    if (t_suppressSwing) {
        return false;
    }
    return g_mobSwing != nullptr ? g_mobSwing(mob, source, r8) : false;
}

bool __fastcall detourGameModeContinueDestroyBlock(void* gameMode, const void* pos,
                                                   std::uint8_t face, const void* playerPos, bool* out)
{
    return FastBlockBreak::instance().onContinueDestroyBlock(gameMode, pos, face, playerPos, out);
}

bool __fastcall detourGameModeDestroyBlock(void* gameMode, const void* pos, std::uint8_t face)
{
    return FastBlockBreak::instance().onDestroyBlock(gameMode, pos, face);
}

std::byte* __fastcall detourAbilitiesAccess(void* actor)
{
    std::byte* const layered = g_abilitiesAccess != nullptr ? g_abilitiesAccess(actor) : nullptr;
    CreativeNoClip::instance().onAbilitiesAccess(layered);
    return layered;
}

void __fastcall detourPoseDecision(void* a1, void* input, void* gameType, void* room, void* flags,
                                   std::uint64_t* actions, void* a7, void* a8, void* a9, void* a10)
{
    const bool lent = CreativeNoClip::instance().beforePoseDecision(room);
    if (g_poseDecision != nullptr) {
        g_poseDecision(a1, input, gameType, room, flags, actions, a7, a8, a9, a10);
    }
    if (lent) {
        CreativeNoClip::instance().afterPoseDecision(room);
    }
}

int __fastcall detourUseItem(void* gameMode, void* itemStack, int extra)
{
    if (!t_modBuild && gameMode != nullptr) {
        void* owner = nullptr;
        if (memory::copyGuarded(static_cast<const std::byte*>(gameMode) + 8, &owner, sizeof(owner))
            && owner != nullptr && owner == GameData::instance().player()) {
            GameData::instance().setGameMode(gameMode);
        }
    }
    if (!t_modBuild) {
        bool handled = false;
        Schematica::instance().onUseItem(gameMode, handled);
        if (handled) return 0;
    }
    return FastUseItem::instance().onUseItem(gameMode, itemStack, extra);
}

int __fastcall detourUseItemTransaction(void* gameMode, void* itemStack, int extra)
{
    return FastUseItem::instance().onUseItemTransaction(gameMode, itemStack, extra);
}

using SubChunkSetBlockFn = void(__fastcall*)(void*, unsigned int, unsigned int, const void*);
SubChunkSetBlockFn g_subChunkSetBlock = nullptr;

void __fastcall detourSubChunkSetBlock(void* self, unsigned int layer, unsigned int index, const void* block)
{
    if (dryRunActive()) { noteDryWrite(self, layer, index, block); return; }
    if (schematicworld::active()) schematicworld::onSubChunkWrite(self, layer, index);
    if (g_subChunkSetBlock != nullptr) g_subChunkSetBlock(self, layer, index, block);
}

using BlockSourceGetBlockFn = const void*(__fastcall*)(void*, const void*);
BlockSourceGetBlockFn g_blockSourceGetBlock = nullptr;
using BlockSourceGetExtraFn = const void*(__fastcall*)(void*, const void*);
BlockSourceGetExtraFn g_blockSourceGetExtra = nullptr;

const void* __fastcall detourBlockSourceGetExtra(void* region, const void* pos)
{
    const void* out = nullptr;
    if (pos && schematicworld::answerForBuild(static_cast<const std::int32_t*>(pos), 1, out)) return out;
    return g_blockSourceGetExtra != nullptr ? g_blockSourceGetExtra(region, pos) : nullptr;
}
constexpr std::size_t kHitSize = 0x85;

using HitAssignFn = void*(__fastcall*)(void*, const void*);
HitAssignFn g_hitAssign = nullptr;

struct HitSeat {
    std::atomic<std::uintptr_t> site{0};
    std::atomic<void*> dst{nullptr};
    std::atomic<std::uint64_t> generation{0};
    unsigned char raw[kHitSize] = {};
};
HitSeat g_hitSeats[2];

void publishHitTo(HitSeat& seat, const unsigned char* p)
{
    seat.generation.fetch_add(1, std::memory_order_acq_rel);
    std::memcpy(seat.raw, p, kHitSize);
    seat.generation.fetch_add(1, std::memory_order_acq_rel);
}

HitSeat* seatForSite(std::uintptr_t site)
{
    for (HitSeat& seat : g_hitSeats) {
        const std::uintptr_t mine = seat.site.load(std::memory_order_relaxed);
        if (mine == site) {
            return &seat;
        }
        if (mine == 0) {
            seat.site.store(site, std::memory_order_relaxed);
            return &seat;
        }
    }
    return nullptr;
}

HitSeat& mainSeat()
{
    const std::uintptr_t a = g_hitSeats[0].site.load(std::memory_order_relaxed);
    const std::uintptr_t b = g_hitSeats[1].site.load(std::memory_order_relaxed);
    return (b != 0 && (a == 0 || b < a)) ? g_hitSeats[1] : g_hitSeats[0];
}

HitSeat& liquidSeat()
{
    return &mainSeat() == &g_hitSeats[0] ? g_hitSeats[1] : g_hitSeats[0];
}

void publishHit(void* returnAddress, void* dst, const unsigned char* p)
{
    HitSeat* const seat = seatForSite(reinterpret_cast<std::uintptr_t>(returnAddress));
    if (seat == nullptr) {
        return;
    }
    seat->dst.store(dst, std::memory_order_relaxed);
    publishHitTo(*seat, p);
}

bool readSeat(const HitSeat& seat, unsigned char* out)
{
    for (int attempt = 0; attempt < 4; ++attempt) {
        const std::uint64_t before = seat.generation.load(std::memory_order_acquire);
        if ((before & 1) != 0) {
            continue;
        }
        std::memcpy(out, seat.raw, kHitSize);
        if (seat.generation.load(std::memory_order_acquire) == before) {
            return before != 0;
        }
    }
    return false;
}

void* __fastcall detourHitAssign(void* dst, const void* src)
{
    void* const site = _ReturnAddress();
    void* const out = g_hitAssign != nullptr ? g_hitAssign(dst, src) : dst;
    if (dst != nullptr) publishHit(site, dst, static_cast<const unsigned char*>(dst));
    return out;
}

using HitPickFn = void(__fastcall*)(void*, void*, void*, void*);
HitPickFn g_hitPick = nullptr;
struct PickArgs { void* self; void* frame; void* a3; void* a4; };
thread_local const PickArgs* t_pickArgs = nullptr;

void __fastcall detourHitPick(void* self, void* frame, void* a3, void* a4)
{
    if (g_hitPick != nullptr) g_hitPick(self, frame, a3, a4);
    if (!FastBlockPlacement::instance().wantsPick()) return;
    const PickArgs args{self, frame, a3, a4};
    const PickArgs* const previous = t_pickArgs;
    t_pickArgs = &args;
    FastBlockPlacement::instance().onPicked();
    t_pickArgs = previous;
}

using HitMoveAssignFn = void*(__fastcall*)(void*, void*);
HitMoveAssignFn g_hitMoveAssign = nullptr;

void* __fastcall detourHitMoveAssign(void* dst, void* src)
{
    void* const out = g_hitMoveAssign != nullptr ? g_hitMoveAssign(dst, src) : dst;
    if (dst == nullptr) {
        return out;
    }
    for (HitSeat& seat : g_hitSeats) {
        if (seat.site.load(std::memory_order_relaxed) != 0 && seat.dst.load(std::memory_order_relaxed) == dst) {
            publishHitTo(seat, static_cast<unsigned char*>(dst));
            break;
        }
    }
    return out;
}

const void* __fastcall detourBlockSourceGetBlock(void* region, const void* pos)
{
    const void* out = nullptr;
    if (pos && schematicworld::answerForBuild(static_cast<const std::int32_t*>(pos), 0, out)) return out;
    return g_blockSourceGetBlock != nullptr ? g_blockSourceGetBlock(region, pos) : nullptr;
}

void __fastcall detourSetGameMode(void* self, int mode, int extra)
{
    GameModeState::instance().onSetGameMode(mode, GetTickCount64());
    PlayerContext::instance().onEntityContext(self);

    if (g_setGameMode != nullptr) {
        g_setGameMode(self, mode, extra);
    }
}

void __fastcall detourNotifyInventoryOpen(void* client, int which)
{
    ItemStackRequest::instance().onNotifyInventoryOpen(client);
    InventoryResync::instance().onInventoryOpening();

    if (g_notifyInventoryOpen != nullptr) {
        g_notifyInventoryOpen(client, which);
    }

    if (!ItemStackRequest::instance().suppressingInputReset()) {
        FastInventory::instance().onInventoryOpenSent(client);
    }
}

void noteSneakInput(void* out)
{
    if (out == nullptr) {
        return;
    }
    std::uint32_t bits = 0;
    __try {
        bits = *static_cast<const volatile std::uint32_t*>(out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    input::noteRawMoveBits(bits);
}

void* __fastcall detourInputGather(void* a1, void* out, void* a3, void* a4)
{
    FreeCamera::instance().onInputGatherBefore(out);
    ToggleSneakSprint::instance().onInputGatherBefore(out, _ReturnAddress());
    noteSneakInput(out);

    void* const result = (g_inputGather != nullptr) ? g_inputGather(a1, out, a3, a4) : nullptr;

    FreeCamera::instance().onInputGather(out, a1);
    return result;
}

void* __fastcall detourMoveIntent(void* out, void* input, void* a3, void* a4, std::uint64_t a5, void* a6)
{
    void* const result = (g_moveIntent != nullptr) ? g_moveIntent(out, input, a3, a4, a5, a6) : nullptr;
    FreeCamera::instance().onMoveIntent(out, input);
    return result;
}

void* __fastcall detourMoveInputHandler(void* client, void* a2, void* a3, void* a4)
{
    if (client != nullptr) {
        g_clientInstance.store(client, std::memory_order_relaxed);
    }
    if (ItemStackRequest::instance().suppressingInputReset()) {
        return nullptr;
    }
    void* const input =
        (g_moveInputHandler != nullptr) ? g_moveInputHandler(client, a2, a3, a4) : nullptr;

    if (input != nullptr) {
        FreeCamera::instance().onMoveInput(input);
    }
    return input;
}

void* __fastcall detourGetActorEffect(void* context, int effectId)
{
    void* const original = (g_getActorEffect != nullptr) ? g_getActorEffect(context, effectId)
                                                         : nullptr;
    void* const shown = AntiEffect::instance().onGetEffect(effectId, original);
    return Fullbright::instance().onGetEffect(effectId, shown, _ReturnAddress());
}

void __fastcall detourContainerOpenHandle(void* packet, void* result, void* callback,
                                          void* network)
{
    if (g_containerOpenHandle == nullptr) {
        return;
    }
    ItemStackRequest& requests = ItemStackRequest::instance();
    std::byte shell[ItemStackRequest::kOpenShellBytes];
    const bool watching = requests.suppressionPending() && requests.captureOpenShell(packet, shell);
    g_containerOpenHandle(packet, result, callback, network);
    if (watching) {
        requests.takeInventoryOpen(packet, result, shell);
    }
    InventoryResync::instance().onContainerOpened();
}

void* __fastcall detourInventoryContentRead(void* packet, void* out, void* stream, void* network)
{
    if (g_inventoryContentRead == nullptr) {
        return out;
    }
    void* const result = g_inventoryContentRead(packet, out, stream, network);
    OffhandSwap::instance().onInventoryContent(packet);
    return result;
}

void* __fastcall detourInventoryHoveredSlot(void* controller, void* out)
{
    InventoryScreen::instance().onController();

    return g_inventoryHoveredSlot != nullptr ? g_inventoryHoveredSlot(controller, out) : out;
}

void* __fastcall detourUiDefLookup(void* self, const void* space, const void* name)
{
    void* const value = (g_uiDefLookup != nullptr) ? g_uiDefLookup(self, space, name) : nullptr;
    if (void* const extended = uiprobe::extendDefinition(self, space, name, value)) {
        return extended;
    }
    if (void* const swapped = uiprobe::substituteOwnPage(self, space, name)) {
        return swapped;
    }
    return value;
}

void* __fastcall detourFogColorFetch(void* self, void* out, void* src, void* r9)
{
    void* const result = (g_fogColorFetch != nullptr) ? g_fogColorFetch(self, out, src, r9) : nullptr;
    OverworldSky::instance().adjustFogColor(self, out);
    return result;
}

void* __fastcall detourFogSettingsFetch(void* self, void* out, void* src, void* r9)
{
    void* const result =
        (g_fogSettingsFetch != nullptr) ? g_fogSettingsFetch(self, out, src, r9) : nullptr;
    OverworldSky::instance().adjustFog(self, out);
    if (out != nullptr && NoRender::instance().fogSuppressed()) {
        auto* const bytes = static_cast<unsigned char*>(out);
        if (bytes[0x1c] != 0) {
            constexpr float kStart = 0.999f;
            constexpr float kEnd = 1.0f;
            constexpr std::uint32_t kRenderDistanceType = 1;
            std::memcpy(bytes + 0x10, &kStart, sizeof(kStart));
            std::memcpy(bytes + 0x14, &kEnd, sizeof(kEnd));
            std::memcpy(bytes + 0x18, &kRenderDistanceType, sizeof(kRenderDistanceType));
        }
    }
    return result;
}

void* __fastcall detourItemHoverTextBuild(const void* stack, void* out, void* level, bool flag)
{
    void* const result = (g_itemHoverTextBuild != nullptr) ? g_itemHoverTextBuild(stack, out, level, flag) : out;
    DebugKeys::instance().onItemHoverText(stack, out);
    AppleSkin::instance().onItemHoverText(stack, out);
    return result;
}

void __fastcall detourHoverRendererRender(void* self, void* ctx, void* client, void* owner, int pass)
{
    if (g_hoverRendererRender != nullptr) {
        g_hoverRendererRender(self, ctx, client, owner, pass);
    }
    AppleSkin::instance().onHoverRender(self, ctx, client, owner);
}

void __fastcall detourMobEffectsRendererRender(void* self, void* ctx, void* client, void* owner, int pass)
{
    if (g_mobEffectsRendererRender != nullptr) {
        g_mobEffectsRendererRender(self, ctx, client, owner, pass);
    }
    EffectTimer::instance().onRendered(client, owner);
}

void __fastcall detourLocatorBarRendererRender(void* self, void* ctx, void* client, void* owner, int pass, void* area)
{
    LocatorBar::instance().onRender(g_locatorBarRendererRender, self, ctx, client, owner, pass, area);
}

void __fastcall detourLocatorBarIcon(void* a, void* mesh, void* texture, float x, float y, float u0, float v0, float u1,
                                     float v1, float w, float h, float scale, float alpha, std::uint32_t color,
                                     std::uint32_t tint, float s0, float s1)
{
    LocatorBar::instance().onIcon(g_locatorBarIcon, a, mesh, texture, x, y, u0, v0, u1, v1, w, h, scale, alpha, color,
                                  tint, s0, s1);
}

void* __fastcall detourNameTagBuild(void* a1, void* out, void* a3, void* a4, void* actor, const void* text,
                                    const float* position, float a8, const void* a9)
{
    return NameTags::instance().onBuildTag(g_nameTagBuild, a1, out, a3, a4, actor, text, position, a8, a9);
}

void __fastcall detourMobEffectsLayout(void* layout)
{
    EffectTimer::instance().beforeLayout(layout);
    if (g_mobEffectsLayout != nullptr) {
        g_mobEffectsLayout(layout);
    }
    EffectTimer::instance().afterLayout(layout);
}

std::uintptr_t __fastcall detourHungerRendererUpdate(void* self, void* a, void* b, void* c)
{
    const std::uintptr_t result = (g_hungerRendererUpdate != nullptr) ? g_hungerRendererUpdate(self, a, b, c) : 0;
    AppleSkin::instance().onHungerRendererUpdate(self);
    return result;
}

std::uintptr_t __fastcall detourHeartRendererUpdate(void* self, void* a, void* b, void* c)
{
    const std::uintptr_t result = (g_heartRendererUpdate != nullptr) ? g_heartRendererUpdate(self, a, b, c) : 0;
    AppleSkin::instance().onHeartRendererUpdate(self);
    return result;
}

bool __fastcall detourAttackCore(void* gameMode, void* target, bool direct, const void* hitPos)
{
    return AutoTool::instance().onAttack(gameMode, target, direct, hitPos);
}

void __fastcall detourSendComplexTx(void* player, void** transaction)
{
    if (AutoTool::instance().onSendTransaction(player, transaction)) {
        return;
    }
    if (g_sendComplexTx != nullptr) {
        g_sendComplexTx(player, transaction);
    }
}

void __fastcall detourLegacyParticleInsert(void* frameBuilder, void* description)
{
    if (NoRender::instance().particlesSuppressed()) {
        const auto back = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
        if (back >= g_legacyParticleSubmitBegin && back < g_legacyParticleSubmitEnd) {
            return;
        }
    }
    if (g_legacyParticleInsert != nullptr) {
        g_legacyParticleInsert(frameBuilder, description);
    }
}

namespace {

constexpr int kInsertShape[] = {
    0x55, 0x56, 0x57, 0x53, 0x48, 0x81, 0xEC, 0xC8, 0x00, 0x00, 0x00, 0x48, 0x8D, 0xAC, 0x24, 0x80, 0x00,
    0x00, 0x00, 0x48, 0xC7, 0x45, 0x40, 0xFE, 0xFF, 0xFF, 0xFF, 0x48, 0x89, 0xD7, 0x48, 0x89, 0xCE, 0x8B,
    0x05, -1,   -1,   -1,   -1,   0x8B, 0x0D, -1,   -1,   -1,   -1,   0x65, 0x48, 0x8B, 0x14, 0x25, 0x58,
    0x00, 0x00, 0x00, 0x48, 0x8B, 0x0C, 0xCA, 0x3B, 0x81, 0x04, 0x00, 0x00, 0x00, 0x7F, -1,   0x48, 0x8D,
    0x0D, -1,   -1,   -1,   -1,   0x48, 0x89, 0x4D, 0x20, 0xC6, 0x45, 0x28, 0x00, 0x48, 0x8D, 0x5D, 0x30,
    0x48, 0xC7, 0x45, 0x38, 0x00, 0x00, 0x00, 0x00, 0xC6, 0x45, 0xE8, 0x00, 0x4C, 0x8D, 0x45, 0xB0, 0x48,
    0x89, 0xDA, 0xE8, -1,   -1,   -1,   -1,   0x88, 0x45, 0x28, 0x48, 0x89, 0x7D, 0x10, 0xC6, 0x45, 0x18,
    0x02, 0x48, 0x8B, 0x06, 0x48, 0x8B, 0x80};

bool functionBounds(const std::byte* inside, std::uintptr_t& begin, std::uintptr_t& end)
{
    DWORD64 base = 0;
    const PRUNTIME_FUNCTION entry =
        RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(inside), &base, nullptr);
    if (entry == nullptr || base == 0 || entry->EndAddress <= entry->BeginAddress) {
        return false;
    }
    begin = static_cast<std::uintptr_t>(base + entry->BeginAddress);
    end = static_cast<std::uintptr_t>(base + entry->EndAddress);
    return true;
}

std::byte* directCallTarget(const std::byte* at)
{
    if (static_cast<unsigned char>(*at) != 0xE8) {
        return nullptr;
    }
    std::int32_t rel = 0;
    std::memcpy(&rel, at + 1, sizeof(rel));
    auto* const target = const_cast<std::byte*>(at) + 5 + rel;
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
    if (!memory::inGameModule(target) || !functionBounds(target, begin, end)
        || begin != reinterpret_cast<std::uintptr_t>(target)) {
        return nullptr;
    }
    return target;
}

bool hasInsertShape(const std::byte* fn)
{
    for (std::size_t i = 0; i < std::size(kInsertShape); ++i) {
        if (kInsertShape[i] >= 0 && static_cast<int>(static_cast<unsigned char>(fn[i])) != kInsertShape[i]) {
            return false;
        }
    }
    return true;
}

std::byte* resolveLegacyParticleInsert()
{
    std::byte* const anchor = Scanner::instance().address(Target::LegacyParticleRender);
    std::uintptr_t anchorBegin = 0;
    std::uintptr_t anchorEnd = 0;
    if (anchor == nullptr || !functionBounds(anchor, anchorBegin, anchorEnd)) {
        return nullptr;
    }
    std::byte* submit = nullptr;
    int calls = 0;
    auto* const anchorStop = reinterpret_cast<std::byte*>(anchorEnd) - 5;
    for (auto* at = reinterpret_cast<std::byte*>(anchorBegin); at <= anchorStop; ++at) {
        std::byte* const target = directCallTarget(at);
        if (target == nullptr) {
            continue;
        }
        if (submit != nullptr && target != submit) {
            log().warn(L"NoRender: the legacy particle submit calls do not agree; particles from the "
                       L"legacy engine will stay visible");
            return nullptr;
        }
        submit = target;
        ++calls;
        at += 4;
    }
    std::uintptr_t submitBegin = 0;
    std::uintptr_t submitEnd = 0;
    if (submit == nullptr || calls < 2 || !functionBounds(submit, submitBegin, submitEnd)) {
        log().warn(L"NoRender: the legacy particle submit was not found ({} call(s))", calls);
        return nullptr;
    }
    std::byte* insert = nullptr;
    auto* const submitStop = reinterpret_cast<std::byte*>(submitEnd) - 5;
    for (auto* at = reinterpret_cast<std::byte*>(submitBegin); at <= submitStop; ++at) {
        std::byte* const target = directCallTarget(at);
        if (target == nullptr || !hasInsertShape(target)) {
            continue;
        }
        if (insert != nullptr && target != insert) {
            log().warn(L"NoRender: more than one legacy particle insert was found; leaving it alone");
            return nullptr;
        }
        insert = target;
        at += 4;
    }
    if (insert == nullptr) {
        log().warn(L"NoRender: the legacy particle insert was not found");
        return nullptr;
    }
    g_legacyParticleSubmitBegin = submitBegin;
    g_legacyParticleSubmitEnd = submitEnd;
    const auto exe = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    log().info(L"NoRender: legacy particles are submitted by RVA {:#x} and inserted by RVA {:#x}",
               submitBegin - exe, reinterpret_cast<std::uintptr_t>(insert) - exe);
    return insert;
}

}

void* __fastcall detourOreKeyRowsBuild(void* out, void* rdx, void* r8)
{
    uiprobe::substituteKeyRows(rdx);
    void* const result =
        (g_oreKeyRowsBuild != nullptr) ? g_oreKeyRowsBuild(out, rdx, r8) : nullptr;
    return result;
}

void* __fastcall detourI18nGet(void* self, void* out, const void* key, void* r9)
{
    void* const result = (g_i18nGet != nullptr) ? g_i18nGet(self, out, key, r9) : nullptr;
    uiprobe::overrideTranslation(key, out);
    return result;
}

bool translateGuarded(void* out, const void* key, void* params)
{
    __try {
        g_i18nGet(g_i18nSelf, out, key, params);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool translateWithParamsGuarded(void* out, const void* key, const void* params)
{
    alignas(16) std::byte shared[16]{};
    __try {
        g_i18nGetWithParams(g_i18nSelf, out, key, params, shared);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool translateInto(void* out, const char* key)
{
    if (g_i18nGet == nullptr || g_i18nSelf == nullptr || out == nullptr || key == nullptr) {
        return false;
    }
    struct alignas(8) MsvcString {
        union {
            char buf[16];
            const char* ptr;
        };
        std::uint64_t size;
        std::uint64_t cap;
    } k{};
    const std::size_t len = std::strlen(key);
    if (len < 16) {
        std::memcpy(k.buf, key, len);
        k.cap = 15;
    } else {
        k.ptr = key;
        k.cap = len;
    }
    k.size = len;
    alignas(8) std::byte params[32]{};
    return translateGuarded(out, &k, params);
}

void* __fastcall detourSettingsGroupResolve(void* registry, void* out, const void* idView, void* r9)
{
    uiprobe::beforeSettingsGroupResolve(registry, idView);
    return g_settingsGroupResolve != nullptr ? g_settingsGroupResolve(registry, out, idView, r9) : nullptr;
}

void* __fastcall detourSettingsGroupRegister(void* registry, const void* idView, void* provider,
                                             void* r9)
{
    uiprobe::onSettingsGroupRegister(registry, idView, provider);
    void* const result = (g_settingsGroupRegister != nullptr)
                             ? g_settingsGroupRegister(registry, idView, provider, r9)
                             : nullptr;
    uiprobe::afterSettingsGroupRegister(registry, idView, provider);
    return result;
}

void* __fastcall detourSettingsProviderCall(void* self, void* out, void* r8, void* r9)
{
    void* const result =
        (g_settingsProviderCall != nullptr) ? g_settingsProviderCall(self, out, r8, r9) : nullptr;
    uiprobe::onSettingsProviderCall(self, out);
    return result;
}

void* __fastcall detourSettingsFindComponent(void* self, void* out, const void* idView, void* r9)
{
    void* const result = (g_settingsFindComponent != nullptr)
                             ? g_settingsFindComponent(self, out, idView, r9)
                             : nullptr;
    uiprobe::onSettingsFindComponent(self, out, idView);
    return result;
}

using UiSliderPublishFn = void(__fastcall*)(void*, float);
UiSliderPublishFn g_uiSliderPublish = nullptr;

using UiCtlBagFn = void*(__fastcall*)(void*);
UiCtlBagFn g_uiCtlBag = nullptr;

struct UiName {
    const char* data;
    unsigned long long size;
};
using UiBagSetFn = void(__fastcall*)(void*, const UiName*, const float*);
UiBagSetFn g_uiBagSet = nullptr;

void resolveCtlBag(void* publish)
{
    constexpr std::ptrdiff_t kCallAt = 0x31;
    if (publish == nullptr) {
        return;
    }
    const auto* at = reinterpret_cast<const unsigned char*>(publish) + kCallAt;
    if (!memory::isReadable(at, 5) || at[0] != 0xE8) {
        log().warn(L"Schematica: the backup getter was not found (+0x31 is not E8)");
        return;
    }
    std::int32_t rel = 0;
    std::memcpy(&rel, at + 1, sizeof(rel));
    const auto target = reinterpret_cast<std::uintptr_t>(at + 5) + static_cast<std::intptr_t>(rel);
    if (!memory::inGameModule(reinterpret_cast<const void*>(target))
        || !memory::isExecutable(reinterpret_cast<const void*>(target), 1)) {
        log().warn(L"Schematica: the backup getter jumps outside the module ({:#x})", target);
        return;
    }
    g_uiCtlBag = reinterpret_cast<UiCtlBagFn>(target);

    constexpr std::ptrdiff_t kSetAt = 0x58;
    const auto* const setAt = reinterpret_cast<const unsigned char*>(publish) + kSetAt;
    if (!memory::isReadable(setAt, 5) || setAt[0] != 0xE8) {
        log().warn(L"Schematica: the backup setter was not found (+0x58 is not E8)");
        return;
    }
    std::int32_t setRel = 0;
    std::memcpy(&setRel, setAt + 1, sizeof(setRel));
    const auto setTarget =
        reinterpret_cast<std::uintptr_t>(setAt + 5) + static_cast<std::intptr_t>(setRel);
    if (!memory::inGameModule(reinterpret_cast<const void*>(setTarget))
        || !memory::isExecutable(reinterpret_cast<const void*>(setTarget), 1)) {
        log().warn(L"Schematica: the backup setter jumps outside the module ({:#x})", setTarget);
        return;
    }
    g_uiBagSet = reinterpret_cast<UiBagSetFn>(setTarget);
}

void __fastcall detourUiSliderPublish(void* self, float value)
{
    if (g_uiCtlBag != nullptr && self != nullptr
        && memory::isReadable(reinterpret_cast<const char*>(self) + 8, 8)) {
        void* const owner =
            *reinterpret_cast<void* const*>(reinterpret_cast<const char*>(self) + 8);
        if (owner != nullptr) {
            uiprobe::onPageBag(g_uiCtlBag(owner));
        }
    }
    g_uiSliderPublish(self, value);
    float reseed = 0.0F;
    if (uiprobe::onSliderPublish(self, value, reseed)) {
        g_uiSliderPublish(self, reseed);
    }
}

using ScenePushFn = void(__fastcall*)(void* stack, void* scene, void* a3, void* a4);
ScenePushFn g_scenePush = nullptr;
const void* g_tradePushReturn = nullptr;

struct OffstackScreen {
    void* scene = nullptr;
    void* counts = nullptr;
    void* ctrl = nullptr;
    void* tick = nullptr;
    unsigned long thread = 0;
};
OffstackScreen g_offstack;

using CtrlTickFn = std::uint32_t(__fastcall*)(void*);
using SceneCountsReleaseFn = void(__fastcall*)(void*);

void* findControllerTick(void* ctrl, const std::uint8_t* base)
{
    if (ctrl == nullptr || base == nullptr) {
        return nullptr;
    }
    __try {
        const auto* const vt = *static_cast<void* const* const*>(ctrl);
        if (vt == nullptr || !memory::inGameModule(vt)) {
            return nullptr;
        }
        for (int i = 0; i < 16; ++i) {
            const auto* const fn = static_cast<const std::uint8_t*>(vt[i]);
            if (fn == nullptr || !memory::inGameModule(fn) || !memory::isReadable(fn, 0x40)) {
                continue;
            }
            if (fn == base) {
                return const_cast<std::uint8_t*>(fn);
            }
            for (int off = 0; off + 5 <= 0x40; ++off) {
                if (fn[off] != 0xE8) {
                    continue;
                }
                std::int32_t rel = 0;
                std::memcpy(&rel, fn + off + 1, sizeof(rel));
                if (fn + off + 5 + rel == base) {
                    return const_cast<std::uint8_t*>(fn);
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

bool readSceneGuarded(void* scene, void** object, void** counts)
{
    __try {
        *object = static_cast<void* const*>(scene)[0];
        *counts = static_cast<void* const*>(scene)[1];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool tickControllerGuarded(CtrlTickFn fn, void* ctrl)
{
    __try {
        fn(ctrl);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void releaseCountsGuarded(void* counts)
{
    if (counts == nullptr) {
        return;
    }
    __try {
        auto* const c = static_cast<std::uint8_t*>(counts);
        void* const* cvt = *reinterpret_cast<void* const* const*>(c);
        if (_InterlockedDecrement(reinterpret_cast<volatile long*>(c + 8)) == 0) {
            reinterpret_cast<SceneCountsReleaseFn>(cvt[0])(c);
            if (_InterlockedDecrement(reinterpret_cast<volatile long*>(c + 0xc)) == 0) {
                reinterpret_cast<SceneCountsReleaseFn>(cvt[1])(c);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool takeTradeScreen(void* scene)
{
    void* const ctrl = ItemScroller::instance().offstackTradeController();
    if (ctrl == nullptr) {
        return false;
    }
    void* object = nullptr;
    void* counts = nullptr;
    if (!readSceneGuarded(scene, &object, &counts) || object == nullptr) {
        return false;
    }
    void* const tick = findControllerTick(
        ctrl, reinterpret_cast<const std::uint8_t*>(Scanner::instance().address(Target::ContainerScreenTick)));
    if (tick == nullptr) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            log().warn(L"Hooks: could not find the tick of the trade screen controller; the auto trade is off");
        }
        return false;
    }
    g_offstack = OffstackScreen{object, counts, ctrl, tick, GetCurrentThreadId()};
    ItemScroller::instance().onTradeScreenHeldOffstack();
    static int said = 0;
    if (said < 4) {
        ++said;
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        log().info(L"Hooks: kept the trade screen off the scene stack (controller vtable slot at RVA {:#x}, thread {})",
                   reinterpret_cast<std::uintptr_t>(tick) - base, g_offstack.thread);
    }
    return true;
}

void __fastcall detourSceneStackPush(void* stack, void* scene, void* a3, void* a4)
{
    if (scene != nullptr && g_tradePushReturn != nullptr && _ReturnAddress() == g_tradePushReturn
        && g_offstack.scene == nullptr && takeTradeScreen(scene)) {
        return;
    }
    if (g_scenePush != nullptr) {
        g_scenePush(stack, scene, a3, a4);
    }
    if (g_offstack.scene != nullptr) {
        releaseOffstackScreen(L"another screen was pushed");
    }
}

std::uint32_t __fastcall detourContainerScreenTick(void* self)
{
    const freezewatch::Scope freezeScope{"ContainerScreenTick (UI thread)"};
    const std::uint32_t dirty = (g_containerScreenTick != nullptr) ? g_containerScreenTick(self) : 0;
    return dirty | containerui::onScreenTickHook(self);
}

int __fastcall detourTradeHoverInvoke(void* self, void* const* bag)
{
    if (bag != nullptr) {
        tradeui::onHoverInvoke(*bag);
    }
    return (g_tradeHoverInvoke != nullptr) ? g_tradeHoverInvoke(self, bag) : 0;
}

int __fastcall detourTradeSecondaryInvoke(void* self, void* const* bag)
{
    const int result = (g_tradeSecondaryInvoke != nullptr) ? g_tradeSecondaryInvoke(self, bag) : 0;
    if (self != nullptr && bag != nullptr) {
        void* ctrl = nullptr;
        std::memcpy(&ctrl, static_cast<const std::byte*>(self) + 8, sizeof(ctrl));
        tradeui::onSecondaryInvoke(ctrl, *bag);
    }
    return result;
}

void* __fastcall detourTradeSelParse(void* sel, const void* bag)
{
    void* const result = g_tradeSelParse(sel, bag);
    tradeui::afterSelParse(sel);
    return result;
}
int __fastcall detourTradeSelectorTotal(void* self)
{
    const int n = g_tradeSelectorTotal(self);
    return (n > 0 && tradeui::favoriteTierShown(self)) ? n + 1 : n;
}
int __fastcall detourTradeTierTotal(void* self, const int* tier)
{
    if (tier != nullptr && tradeui::favoriteTierShown(self)) {
        if (*tier == 0) {
            return tradeui::favoriteTierRows();
        }
        const int raw = *tier - 1;
        int count = 0;
        bool emptied = false;
        if (tradeui::restRows(raw, count, emptied)) {
            return count;
        }
        return g_tradeTierTotal(self, &raw);
    }
    return g_tradeTierTotal(self, tier);
}
bool __fastcall detourTradeTierVisible(void* self, const int* tier)
{
    if (tier != nullptr && tradeui::favoriteTierShown(self)) {
        if (*tier == 0) {
            return true;
        }
        const int raw = *tier - 1;
        int count = 0;
        bool emptied = false;
        if (tradeui::restRows(raw, count, emptied) && emptied) {
            return false;
        }
        return g_tradeTierVisible(self, &raw);
    }
    return g_tradeTierVisible(self, tier);
}
bool __fastcall detourTradeTierUnlocked(void* self, const int* tier)
{
    if (tier != nullptr && tradeui::favoriteTierShown(self)) {
        if (*tier == 0) {
            return true;
        }
        const int raw = *tier - 1;
        return g_tradeTierUnlocked(self, &raw);
    }
    return g_tradeTierUnlocked(self, tier);
}
void* __fastcall detourTradeTierName(void* ret, void* self, const int* tier)
{
    if (tier != nullptr && tradeui::favoriteTierShownAny()) {
        if (*tier == 0) {
            tradeui::writeFavoriteTierName(ret);
            return ret;
        }
        const int raw = *tier - 1;
        return g_tradeTierName(ret, self, &raw);
    }
    return g_tradeTierName(ret, self, tier);
}
int __fastcall detourTradeCurrentTier(void* owner)
{
    const int value = g_tradeCurrentTier(owner);
    return tradeui::overrideTier(owner, value, _ReturnAddress());
}

void* __fastcall detourContainerScreenCtor(void* self, void* a2, void* a3, void* a4)
{
    void* const result = (g_containerScreenCtor != nullptr) ? g_containerScreenCtor(self, a2, a3, a4) : self;
    containerui::onScreenConstructed(self);
    return result;
}

void* __fastcall detourHudScreenCtor(void* self, void* a2, void* a3, void* a4)
{
    void* const result = (g_hudScreenCtor != nullptr) ? g_hudScreenCtor(self, a2, a3, a4) : self;
    containerui::onHudConstructed(self);
    return result;
}

using SettingsGroupBuildFn = void*(__fastcall*)(void* self, void* out);
SettingsGroupBuildFn g_accessibilitySettingsBuild = nullptr;
SettingsGroupBuildFn g_videoSettingsBuild = nullptr;

void* __fastcall detourAccessibilitySettingsBuild(void* self, void* out)
{
    void* const result = g_accessibilitySettingsBuild(self, out);
    uiprobe::onGuiScaleGroupBuilt(out, 1);
    return result;
}

void* __fastcall detourVideoSettingsBuild(void* self, void* out)
{
    void* const result = g_videoSettingsBuild(self, out);
    uiprobe::onGuiScaleGroupBuilt(out, 0);
    return result;
}

void __fastcall detourRenderCurrentFrame(void* self, float delta)
{
    customguiscale::onFrame();
    DebugScreen::onFrameRendered();
    if (g_renderCurrentFrame != nullptr) {
        g_renderCurrentFrame(self, delta);
    }
}

void runClientTick()
{
    constexpr unsigned long long kViewFreshMs = 1000;
    const unsigned long long lastView = g_lastLeadingViewAt.load(std::memory_order_relaxed);
    if (lastView == 0 || GetTickCount64() - lastView > kViewFreshMs) {
        return;
    }
    const freezewatch::Scope freezeScope{"ClientTick (game thread)"};
    flyspeed::onClientTick();
    FastBlockPlacement::instance().onClientTick();
    Scaffold::instance().onClientTick();
}

void __fastcall detourServerLevelTick(void* level)
{
    const void* const caller = _ReturnAddress();
    if (g_sessionTicksReady && (caller == g_sessionTickReturns[0] || caller == g_sessionTickReturns[1])) {
        LARGE_INTEGER start{};
        QueryPerformanceCounter(&start);
        g_sessionTickStart = start.QuadPart;
        g_sessionTickBranch = caller == g_sessionTickReturns[0] ? 1 : 2;
    }
    if (g_serverLevelTick != nullptr) {
        __try {
            g_serverLevelTick(level);
        } __finally {
            if (AbnormalTermination()) g_sessionTickBranch = 0;
        }
    }
}

void finishSessionTick(void* session, int branch)
{
    if (!g_sessionTicksReady || g_sessionTickBranch != branch) return;
    g_sessionTickBranch = 0;
    LARGE_INTEGER end{};
    QueryPerformanceCounter(&end);
    const unsigned long thread = GetCurrentThreadId();
    const unsigned long clientThread = blocks::simThread();
    if (GameData::instance().onLevelTick(session, thread, clientThread)) {
        DebugScreen::onServerTick(end.QuadPart - g_sessionTickStart);
        flyspeed::onServerTick(session);
    }
    if (clientThread != 0 && thread == clientThread) {
        rejoin::noteClientSession(session);
        runClientTick();
    }
}

void installSessionTicks(HookManager& hooks, const Scanner& scanner)
{
    std::byte* const first = scanner.address(Target::ServerLevelTick);
    if (!writes::allowed("ServerLevelTick") || first == nullptr || !memory::isReadable(first, 0x280)) return;
    const ScanHit second = scanRange(std::span(first + 0x100, std::size_t{0x180}), kGameSessionCatchupTickShape);
    std::uint8_t a[0x4C]{}, b[0x4C]{};
    if (second.count != 1 || !memory::copyGuarded(first, a, sizeof(a))
        || !memory::copyGuarded(second.address, b, sizeof(b))
        || !sessiontick::siteLooksRight(a, false) || !sessiontick::siteLooksRight(b, true)) {
        log().warn(L"ServerLevelTick: the two inlined session ticks did not match; leaving them alone");
        return;
    }
    DWORD64 firstImage = 0, secondImage = 0;
    const auto* const firstEntry = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(first), &firstImage, nullptr);
    const auto* const secondEntry = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(second.address), &secondImage, nullptr);
    RUNTIME_FUNCTION entry{};
    constexpr std::uint8_t kFrame[] = {0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,
                                      0x48,0x81,0xEC,0x38,0x01,0,0};
    std::uint8_t frame[sizeof(kFrame)]{};
    if (firstEntry == nullptr || firstEntry != secondEntry || firstImage != secondImage
        || !memory::copyGuarded(firstEntry, &entry, sizeof(entry))
        || !memory::copyGuarded(reinterpret_cast<const void*>(firstImage + entry.BeginAddress), frame, sizeof(frame))
        || std::memcmp(frame, kFrame, sizeof(kFrame)) != 0) return;
    void* const levelTick = memory::ripTarget(first, 5);
    if (levelTick != memory::ripTarget(second.address, 5) || !memory::inGameModule(levelTick)
        || !memory::isExecutable(levelTick, 1)) return;
    g_sessionTickReturns[0] = first + sessiontick::kReturnOffset;
    g_sessionTickReturns[1] = second.address + sessiontick::kReturnOffset;
    bool ready = hooks.create(levelTick, &detourServerLevelTick, reinterpret_cast<void**>(&g_serverLevelTick), L"ServerLevelTick");
    ready &= hooks.create(first + sessiontick::kFinishOffset, &tsukuyomiSessionTickFirstEntry,
                          &tsukuyomiSessionTickFirstTrampoline, L"ServerLevelTick");
    ready &= hooks.create(second.address + sessiontick::kFinishOffset, &tsukuyomiSessionTickCatchupEntry,
                          &tsukuyomiSessionTickCatchupTrampoline, L"ServerLevelTick");
    g_sessionTicksReady = ready;
}

void* __fastcall detourLegacyParticleRender(void* engine, void* out, void* a3, void* a4, void* a5, float a6)
{
    if (DebugScreen::wantsParticles()) {
        int legacy = 0;
        int dataDriven = 0;
        if (debugworld::countLegacyParticles(engine, legacy)
            && debugworld::countDataDrivenParticles(engine, a5, dataDriven)) {
            DebugScreen::onParticles(legacy + dataDriven);
        }
    }
    return g_legacyParticleRender != nullptr ? g_legacyParticleRender(engine, out, a3, a4, a5, a6) : out;
}

int __fastcall detourFmodSystemUpdate(void* system)
{
    const int result = g_fmodSystemUpdate != nullptr ? g_fmodSystemUpdate(system) : 0;
    if (DebugScreen::wantsSounds() && DebugScreen::soundSampleDue()) {
        fmodstats::Counts counts{};
        if (fmodstats::sample(system, counts)) {
            DebugScreen::onSounds(counts.statics, counts.streams, counts.cap, counts.loadedSounds,
                                  counts.memoryBytes);
        }
    }
    return result;
}

bool copyStartGameString(const void* packet, std::size_t offset, char* out, std::size_t cap)
{
    if (cap == 0 || packet == nullptr || !memory::isReadable(static_cast<const std::uint8_t*>(packet) + offset, 32)) {
        return false;
    }
    __try {
        const auto* const field = static_cast<const std::uint8_t*>(packet) + offset;
        const std::size_t length = *reinterpret_cast<const std::size_t*>(field + 0x10);
        const std::size_t capacity = *reinterpret_cast<const std::size_t*>(field + 0x18);
        if (length >= cap || capacity < length || (capacity <= 15 && length > 15)) return false;
        const char* const source = capacity > 15 ? *reinterpret_cast<const char* const*>(field)
                                                 : reinterpret_cast<const char*>(field);
        if (source == nullptr || !memory::isReadable(source, length)) return false;
        for (std::size_t i = 0; i < length; ++i) out[i] = source[i];
        out[length] = '\0';
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __fastcall detourStartGameHandle(void* self, const void* identifier, const void* packet)
{
    char version[32]{};
    DebugScreen::onStartGame(copyStartGameString(packet, 0x648, version, sizeof(version)) ? version : nullptr);
    char levelId[256]{}, worldName[256]{};
    if (!copyStartGameString(packet, 0x598, levelId, sizeof(levelId))) levelId[0] = '\0';
    if (!copyStartGameString(packet, 0x5b8, worldName, sizeof(worldName))) worldName[0] = '\0';
    worldidentity::onStartGame(levelId, worldName);
    if (g_startGameHandle != nullptr) g_startGameHandle(self, identifier, packet);
}

std::uintptr_t __fastcall detourLevelChunkTick(void* chunk, void* a2, void* a3, void* a4)
{
    const unsigned long sim = blocks::simThread();
    const bool clientThread = sim == 0 || GetCurrentThreadId() == sim;
    if (!clientThread) {
        DebugScreen::onChunkTicked();
    }
    return g_levelChunkTick != nullptr ? g_levelChunkTick(chunk, a2, a3, a4) : 0;
}

std::uint64_t __fastcall detourPacketCheckSize(void* self, void* a2, void* a3,
                                               unsigned int receiverIsServer)
{
    if (receiverIsServer == 0) {
        DebugScreen::onPacketReceived();
    }
    return g_packetCheckSize != nullptr ? g_packetCheckSize(self, a2, a3, receiverIsServer) : 0;
}

std::uint64_t __fastcall detourNetworkSend(void* self, void* a2, void* a3, unsigned int a4)
{
    DebugScreen::onPacketSent();
    return g_networkSend != nullptr ? g_networkSend(self, a2, a3, a4) : 0;
}

const void* __fastcall detourContainerGetItem(void* mc, const void* collectionName, int index)
{
    if (mc != nullptr && mc == OffhandSlot::hudManager()) {
        if (const void* const own = OffhandSlot::offhandStackFor(collectionName, index)) {
            return own;
        }
    }
    if (mc != nullptr && mc == ArmorHUD::hudManager()) {
        if (const void* const own = ArmorHUD::stackFor(collectionName, index)) {
            return own;
        }
    }
    if (const void* const own = ShulkerPreview::stackFor(collectionName, index)) {
        return own;
    }
    return g_containerGetItem(mc, collectionName, index);
}

bool __fastcall detourHudCollResolve(void* self, void* coll, std::uintptr_t collHash, std::uintptr_t index, void* name,
                                     std::uintptr_t nameHash, void* out1, void* out2)
{
    containerui::onHudResolving(self);
    if (g_hudCollBase != nullptr && containerui::hudForwardsToBase(static_cast<std::uint32_t>(nameHash))) {
        return g_hudCollBase(self, coll, collHash, index, name, nameHash, out1, out2);
    }
    return g_hudCollResolve(self, coll, collHash, index, name, nameHash, out1, out2);
}

void* __fastcall detourContainerScreenDtor(void* self)
{
    containerui::onScreenDestroyed(self);
    return (g_containerScreenDtor != nullptr) ? g_containerScreenDtor(self) : self;
}

int __fastcall detourContainerSm(void* sm, std::uint32_t id, int state, const void* coll, int index)
{
    int result = 0;
    if (containerui::onSmHandle(sm, id, state, coll, index, result)) {
        return result;
    }
    return (g_containerSm != nullptr) ? g_containerSm(sm, id, state, coll, index) : 0;
}

int __fastcall detourUiEventDispatch(void* self, const void* event, void* r8, void* r9)
{
    uiprobe::onUiEvent(self, event);
    return (g_uiEventDispatch != nullptr) ? g_uiEventDispatch(self, event, r8, r9) : 0;
}

}

extern "C" void tsukuyomiSessionTickFinished(void* session, int branch)
{
    finishSessionTick(session, branch);
}

bool readHitResult(unsigned char out[kHitResultBytes])
{
    static_assert(kHitSize == kHitResultBytes);
    return out != nullptr && readSeat(mainSeat(), out);
}
void* hitResultAddress()
{
    return mainSeat().dst.load(std::memory_order_acquire);
}

bool repickHitResult()
{
    const PickArgs* const args = t_pickArgs;
    if (args == nullptr || g_hitPick == nullptr) return false;
    g_hitPick(args->self, args->frame, args->a3, args->a4);
    return true;
}

bool readLiquidHitResult(unsigned char out[kHitResultBytes])
{
    if (out == nullptr || g_hitSeats[0].site.load(std::memory_order_relaxed) == 0
        || g_hitSeats[1].site.load(std::memory_order_relaxed) == 0) {
        return false;
    }
    return readSeat(liquidSeat(), out);
}

namespace {

void* resolveI18nGet()
{
    void* const anchor = Scanner::instance().address(Target::I18nAnchor);
    if (anchor == nullptr) {
        return nullptr;
    }
    const auto at = reinterpret_cast<const unsigned char*>(anchor);
    if (!memory::isReadable(at, 21)) {
        return nullptr;
    }
    std::int32_t disp = 0;
    std::memcpy(&disp, at + 17, sizeof(disp));
    const auto global = reinterpret_cast<void* const*>(at + 21 + disp);
    if (!memory::isReadable(global, sizeof(void*))) {
        return nullptr;
    }
    const auto vtable = reinterpret_cast<void* const*>(*global);
    if (vtable == nullptr || !memory::isReadable(vtable, 0x88)) {
        return nullptr;
    }
    g_i18nSelf = const_cast<void*>(reinterpret_cast<const void*>(global));
    void* const get = vtable[0x80 / sizeof(void*)];
    constexpr unsigned char kCallParams[] = {0x4C, 0x8D, 0x4D, 0xC0, 0x48, 0x89, 0xF2, 0xE8};
    unsigned char head[0x90]{};
    if (memory::copyGuarded(get, head, sizeof(head))) {
        for (std::size_t i = 0; i + sizeof(kCallParams) + 4 <= sizeof(head); ++i) {
            if (std::memcmp(head + i, kCallParams, sizeof(kCallParams)) != 0) continue;
            void* const body = memory::ripTarget(static_cast<const std::byte*>(get) + i + 7, 1);
            if (memory::inGameModule(body) && memory::isExecutable(body, 1)) {
                g_i18nGetWithParams = reinterpret_cast<I18nGetWithParamsFn>(body);
            }
            break;
        }
    }
    return get;
}

void* blockSourceForVtable()
{
    return blockwrite::renderRegion();
}

void* resolveBlockSourceGetBlock()
{
    void* const region = blockSourceForVtable();
    if (region == nullptr || !memory::isReadable(region, sizeof(void*))) {
        return nullptr;
    }
    void* vtable = nullptr;
    std::memcpy(&vtable, region, sizeof(vtable));
    if (vtable == nullptr || !memory::isReadable(vtable, 0x18)) {
        return nullptr;
    }
    void* fn = nullptr;
    std::memcpy(&fn, static_cast<const unsigned char*>(vtable) + 0x10, sizeof(fn));
    return (memory::isExecutable(fn, 1) && memory::inGameModule(fn)) ? fn : nullptr;
}

void* resolveBlockSourceGetExtra()
{
    void* const region = blockSourceForVtable();
    if (region == nullptr || !memory::isReadable(region, sizeof(void*))) {
        return nullptr;
    }
    void* vtable = nullptr;
    std::memcpy(&vtable, region, sizeof(vtable));
    if (vtable == nullptr || !memory::isReadable(vtable, 0x28)) {
        return nullptr;
    }
    void* fn = nullptr;
    std::memcpy(&fn, static_cast<const unsigned char*>(vtable) + 0x20, sizeof(fn));
    return (memory::isExecutable(fn, 1) && memory::inGameModule(fn)) ? fn : nullptr;
}

using BeRenderLoopFn = void(__fastcall*)(void*, void*, unsigned int);
BeRenderLoopFn g_beRenderLoop = nullptr;

void __fastcall detourBeRenderLoop(void* renderer, void* ctx, unsigned int shadow)
{
    void* source = nullptr;
    if (renderer != nullptr) {
        auto* const at = static_cast<unsigned char*>(renderer) + 0x978;
        if (!memory::copyGuarded(at, &source, sizeof(source))) {
            source = nullptr;
        }
        blockwrite::noteRenderRegion(source);
    }
    if (g_beRenderLoop != nullptr) {
        g_beRenderLoop(renderer, ctx, shadow);
    }
}

bool g_lateI18nSettled = false;
bool g_lateBlockGetSettled = false;

}

bool installLate()
{
    HookManager& hooks = HookManager::instance();
    bool created = false;

    if (!g_lateI18nSettled) {
        if (void* const fn = resolveI18nGet(); fn != nullptr) {
            g_lateI18nSettled = true;
            created = hooks.create(fn, &detourI18nGet, reinterpret_cast<void**>(&g_i18nGet),
                                   L"I18nGet")
                      || created;
        }
    }

    if (!g_lateBlockGetSettled && !writes::allowed(std::string_view("BlockSourceGetBlock"))) {
        g_lateBlockGetSettled = true;
    }
    if (!g_lateBlockGetSettled) {
        bool got = false;
        if (void* const fn = resolveBlockSourceGetBlock(); fn != nullptr) {
            got = hooks.create(fn, &detourBlockSourceGetBlock,
                               reinterpret_cast<void**>(&g_blockSourceGetBlock),
                               L"BlockSourceGetBlock", HookGroup::Ghost);
            created = got || created;
            if (got) {
                const auto exe = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
                log().info(L"Hooks: BlockSource::getBlock at RVA {:#x} (from the {} BlockSource)",
                           reinterpret_cast<std::uintptr_t>(fn) - exe,
                           L"rendering");
            }
        }
        if (got) {
            if (void* const extra = resolveBlockSourceGetExtra(); extra != nullptr) {
                created = hooks.create(extra, &detourBlockSourceGetExtra,
                                       reinterpret_cast<void**>(&g_blockSourceGetExtra),
                                       L"BlockSourceGetExtra", HookGroup::Ghost)
                          || created;
            }
            g_lateBlockGetSettled = true;
        }
    }

    if (created) {
        hooks.applyQueued();
    }

    return g_lateI18nSettled && g_lateBlockGetSettled;
}

using LevelBuildDispatchFn = void*(__fastcall*)(void*, void*, void*, void*);
LevelBuildDispatchFn g_levelBuildDispatch = nullptr;

bool readBuilderStats(void* params, int& pending, int& freeBuffers)
{
    __try {
        const auto* const builder = *reinterpret_cast<const std::uint8_t* const*>(static_cast<std::uint8_t*>(params) + 8);
        if (builder == nullptr) {
            return false;
        }
        const std::int64_t limit = *reinterpret_cast<const std::int64_t*>(builder + 0x70);
        const std::int64_t nearBuilds = *reinterpret_cast<const std::int64_t*>(builder + 0x78);
        const std::int64_t farBuilds = *reinterpret_cast<const std::int64_t*>(builder + 0x80);
        const auto poolBegin = *reinterpret_cast<const std::uintptr_t*>(builder + 0x88);
        const auto poolEnd = *reinterpret_cast<const std::uintptr_t*>(builder + 0x90);
        if (limit < 0 || limit > 4096 || nearBuilds < 0 || farBuilds < 0 || nearBuilds + farBuilds > 4096 || poolEnd < poolBegin
            || (poolEnd - poolBegin) % 8 != 0 || (poolEnd - poolBegin) / 8 > 4096) {
            return false;
        }
        pending = static_cast<int>(nearBuilds + farBuilds);
        freeBuffers = static_cast<int>((poolEnd - poolBegin) / 8);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                   : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

void* __fastcall detourLevelBuildDispatch(void* ret, void* params, void* third, void* fourth)
{
    if (DebugScreen::wantsChunkRender()) {
        int pending = 0;
        int freeBuffers = 0;
        if (readBuilderStats(params, pending, freeBuffers)) {
            DebugScreen::onBuilderStats(pending, freeBuffers);
        }
    }
    return g_levelBuildDispatch != nullptr ? g_levelBuildDispatch(ret, params, third, fourth) : ret;
}

void installGhostChunkBuilder()
{
    HookManager::instance().create(Scanner::instance().address(Target::LevelBuildDispatch),
        &detourLevelBuildDispatch, reinterpret_cast<void**>(&g_levelBuildDispatch), L"LevelBuildDispatch");
}

std::atomic<bool> g_groupsFrozen{false};

void freezeHookGroups()
{
    g_groupsFrozen.store(true, std::memory_order_release);
}

static void refreshHookGroups()
{
    if (g_groupsFrozen.load(std::memory_order_acquire)) {
        return;
    }

    static bool now[static_cast<std::size_t>(HookGroup::Count)] = {};
    static unsigned long long offSince[static_cast<std::size_t>(HookGroup::Count)] = {};
    constexpr unsigned long long kOffDelayMs = 2000;

    const struct {
        HookGroup group;
        bool on;
    } wants[] = {
        {HookGroup::Ghost, Schematica::instance().wantsGhostHooks() || schematicworld::active()},
        {HookGroup::Fullbright, Fullbright::instance().enabled() || AntiEffect::instance().enabled()},
        {HookGroup::Ability,
         CreativeNoClip::instance().enabled()},
        {HookGroup::Tool, AutoTool::instance().enabled()},
        {HookGroup::Fog, NoRender::instance().fogSuppressed() || OverworldSky::instance().wantsFogHook()},
        {HookGroup::Particles, NoRender::instance().particlesSuppressed()},
    };
    const unsigned long long at = GetTickCount64();
    for (const auto& want : wants) {
        const auto index = static_cast<std::size_t>(want.group);
        bool& remembered = now[index];
        if (want.on == remembered) {
            offSince[index] = 0;
            continue;
        }
        if (want.on) {
            offSince[index] = 0;
            remembered = true;
            HookManager::instance().setGroupEnabled(want.group, true);
            continue;
        }
        if (offSince[index] == 0) {
            offSince[index] = at;
        } else if (at - offSince[index] >= kOffDelayMs) {
            offSince[index] = 0;
            remembered = false;
            HookManager::instance().setGroupEnabled(want.group, false);
        }
    }

}

void installAll()
{
    const Scanner& scanner = Scanner::instance();
    HookManager& hooks = HookManager::instance();
    if (customguiscale::prepareHooks()) {
        const bool compute = hooks.create(scanner.address(Target::GuiScaleCompute), &customguiscale::detourCompute,
            reinterpret_cast<void**>(&customguiscale::originalCompute), L"GuiScaleCompute");
        const bool list = hooks.create(customguiscale::providerFunction(3), &customguiscale::detourList,
            reinterpret_cast<void**>(&customguiscale::originalList), L"GuiScaleOptionList");
        const bool get = hooks.create(customguiscale::providerFunction(4), &customguiscale::detourGet,
            reinterpret_cast<void**>(&customguiscale::originalGet), L"GuiScaleOptionGet");
        const bool set = hooks.create(customguiscale::providerFunction(5), &customguiscale::detourSet,
            reinterpret_cast<void**>(&customguiscale::originalSet), L"GuiScaleOptionSet");
        const bool accessibility = hooks.create(scanner.address(Target::AccessibilitySettingsBuild),
            &detourAccessibilitySettingsBuild, reinterpret_cast<void**>(&g_accessibilitySettingsBuild),
            L"AccessibilitySettingsBuild");
        const bool video = hooks.create(scanner.address(Target::VideoSettingsBuild), &detourVideoSettingsBuild,
            reinterpret_cast<void**>(&g_videoSettingsBuild), L"VideoSettingsBuild");
        customguiscale::setHooksReady(compute && list && get && set && accessibility && video);
    }

    hooks.create(scanner.address(Target::CommandAutoComplete), &detourCommandAutoComplete,
                 reinterpret_cast<void**>(&g_commandAutoComplete), L"CommandAutoComplete");
    hooks.create(scanner.address(Target::CommandAutoCompleteFilter), &detourCommandAutoCompleteFilter,
                 reinterpret_cast<void**>(&g_commandAutoCompleteFilter), L"CommandAutoCompleteFilter");
    hooks.create(scanner.address(Target::SendCommandRequest), &detourSendCommandRequest,
                 reinterpret_cast<void**>(&g_sendCommandRequest), L"SendCommandRequest");
    hooks.create(scanner.address(Target::CommandRegistryLoadPacket), &detourCommandRegistryLoadPacket,
                 reinterpret_cast<void**>(&g_commandRegistryLoadPacket), L"CommandRegistryLoadPacket");

    hooks.create(scanner.address(Target::GetDestroySpeed), &detourGetDestroySpeed,
                 reinterpret_cast<void**>(&g_getDestroySpeed), L"GetDestroySpeed", HookGroup::Tool);

    hooks.create(scanner.address(Target::SetSelectedSlot), &detourSetSelectedSlot,
                 reinterpret_cast<void**>(&g_setSelectedSlot), L"SetSelectedSlot");

    hooks.create(scanner.address(Target::SubChunkSetBlock), &detourSubChunkSetBlock,
                 reinterpret_cast<void**>(&g_subChunkSetBlock), L"SubChunkSetBlock", HookGroup::Ghost);

    installGhostChunkBuilder();

    hooks.create(scanner.address(Target::HitResultAssign), &detourHitAssign,
                 reinterpret_cast<void**>(&g_hitAssign), L"HitResultAssign");
    hooks.create(scanner.address(Target::HitResultMoveAssign), &detourHitMoveAssign,
                 reinterpret_cast<void**>(&g_hitMoveAssign), L"HitResultMoveAssign");
    hooks.create(scanner.address(Target::HitResultPick), &detourHitPick,
                 reinterpret_cast<void**>(&g_hitPick), L"HitResultPick");

    hooks.create(scanner.address(Target::BeRenderLoop), &detourBeRenderLoop,
                 reinterpret_cast<void**>(&g_beRenderLoop), L"BeRenderLoop");

    hooks.create(scanner.address(Target::BuildBlock), &detourBuildBlock,
                 reinterpret_cast<void**>(&g_buildBlock), L"buildBlock");
    if (scanner.found(Target::BuildBlockKeptClickSite)) {
        const auto* const site = reinterpret_cast<const std::uint8_t*>(scanner.address(Target::BuildBlockKeptClickSite));
        std::uint8_t bytes[25]{};
        if (memory::copyGuarded(site, bytes, sizeof(bytes)) && bytes[17] == 0x80 && bytes[21] == 0x48 && bytes[23] == 0x47) {
            g_keptClickFlag = bytes[19]; g_keptClickPoint = bytes[24];
            log().info(L"buildBlock: kept click point at game mode +{:#x} (flag +{:#x})", g_keptClickPoint, g_keptClickFlag);
        } else {
            log().warn(L"buildBlock: the kept click point fields could not be read; placements while holding may reuse the first click");
        }
    } else {
        log().warn(L"buildBlock: BuildBlockKeptClickSite was not found; placements while holding may reuse the first click");
    }
    {
        std::int32_t readDisp = 0;
        std::int32_t writeDisp = 0;
        const auto* const site = reinterpret_cast<const std::uint8_t*>(scanner.address(Target::PlayerGameModeSite));
        if (site != nullptr && memory::copyGuarded(site + 3, &readDisp, sizeof(readDisp))
            && memory::copyGuarded(site + 17, &writeDisp, sizeof(writeDisp)) && readDisp == writeDisp && readDisp > 8
            && readDisp < 0x4000) {
            GameData::instance().setPlayerGameModeOffset(static_cast<std::size_t>(readDisp));
            log().info(L"buildBlock: the player's game mode is at player +{:#x}", readDisp);
        } else {
            notice::failOnce("Detours.playerGameMode",
                             L"buildBlock: the player's game mode field could not be read (PlayerGameModeSite); Scaffold, "
                             L"FastBlockPlacement and Easy Place do not place blocks",
                             "Scaffold, FastBlockPlacement and Easy Place are off: the player's game mode could not be found");
        }
    }
    hooks.create(scanner.address(Target::MobSwing), &detourMobSwing,
                 reinterpret_cast<void**>(&g_mobSwing), L"MobSwing");

    hooks.create(scanner.address(Target::GameModeContinueDestroyBlock), &detourGameModeContinueDestroyBlock,
                 reinterpret_cast<void**>(&g_gameModeContinueDestroyBlock),
                 L"GameModeContinueDestroyBlock", HookGroup::Always);
    hooks.create(scanner.address(Target::GameModeDestroyBlock), &detourGameModeDestroyBlock,
                 reinterpret_cast<void**>(&g_gameModeDestroyBlock),
                 L"GameModeDestroyBlock", HookGroup::Always);

    hooks.create(scanner.address(Target::ViewVector), &detourViewVector,
                 reinterpret_cast<void**>(&g_viewVector), L"ViewVector");

    hooks.create(scanner.address(Target::CameraDirectLookTurn), &detourCameraDirectLookTurn,
                 reinterpret_cast<void**>(&g_cameraDirectLookTurn), L"CameraDirectLookTurn");
    hooks.create(scanner.address(Target::CameraOrbitTurn), &detourCameraOrbitTurn,
                 reinterpret_cast<void**>(&g_cameraOrbitTurn), L"CameraOrbitTurn");
    hooks.create(scanner.address(Target::DismountButton), &detourDismountButton,
                 reinterpret_cast<void**>(&g_dismountButton), L"DismountButton");
    hooks.create(scanner.address(Target::HotbarScrollButton), &detourHotbarScrollButton,
                 reinterpret_cast<void**>(&g_hotbarScrollButton), L"HotbarScrollButton");
    hooks.create(scanner.address(Target::HudHotbarScrollNext), &detourHudHotbarScrollNext,
                 reinterpret_cast<void**>(&g_hudHotbarScrollNext), L"HudHotbarScrollNext");
    hooks.create(scanner.address(Target::HudHotbarScrollPrev), &detourHudHotbarScrollPrev,
                 reinterpret_cast<void**>(&g_hudHotbarScrollPrev), L"HudHotbarScrollPrev");

    if (const std::byte* const abilitySite = scanner.address(Target::AbilitiesAccess);
        abilitySite != nullptr && memory::isReadable(abilitySite, 8)) {
        hooks.create(directCallTarget(abilitySite + 3), &detourAbilitiesAccess,
                     reinterpret_cast<void**>(&g_abilitiesAccess), L"AbilitiesAccess", HookGroup::Ability);
    }
    hooks.create(scanner.address(Target::PoseDecision), &detourPoseDecision,
                 reinterpret_cast<void**>(&g_poseDecision), L"PoseDecision", HookGroup::Ability);

    hooks.create(scanner.address(Target::UseItem), &detourUseItem,
                 reinterpret_cast<void**>(&g_useItem), L"useItem");

    hooks.create(scanner.address(Target::UseItemTransaction), &detourUseItemTransaction,
                 reinterpret_cast<void**>(&g_useItemTransaction), L"useItemTransaction");

    hooks.create(scanner.address(Target::SetGameMode), &detourSetGameMode,
                 reinterpret_cast<void**>(&g_setGameMode), L"SetGameMode");

    hooks.create(scanner.address(Target::NotifyInventoryOpen), &detourNotifyInventoryOpen,
                 reinterpret_cast<void**>(&g_notifyInventoryOpen), L"NotifyInventoryOpen");

    hooks.create(scanner.address(Target::MoveInputHandler), &detourMoveInputHandler,
                 reinterpret_cast<void**>(&g_moveInputHandler), L"MoveInputHandler");

    if (hooks.create(scanner.address(Target::InputGather), &detourInputGather,
                     reinterpret_cast<void**>(&g_inputGather), L"InputGather")) {
        const auto* const site = reinterpret_cast<const std::uint8_t*>(scanner.address(Target::InputGatherKeyCallSite));
        const auto* const gather = reinterpret_cast<const std::uint8_t*>(scanner.address(Target::InputGather));
        const void* keyReturn = nullptr;
        if (site != nullptr && gather != nullptr && memory::isReadable(site, 0x16) && site[0x11] == 0xE8) {
            std::int32_t rel = 0;
            std::memcpy(&rel, site + 0x12, sizeof(rel));
            if (site + 0x16 + rel == gather) keyReturn = site + 0x16;
        }
        ToggleSneakSprint::instance().setKeyCallReturn(keyReturn);
        if (keyReturn == nullptr) {
            notice::failOnce("ToggleSneakSprint.keyCall",
                             L"Hooks: could not locate the raw key call of InputGather; ToggleSneakSprint is unavailable",
                             "ToggleSneakSprint is unavailable (the key input call was not found)");
        }
    }

    hooks.create(scanner.address(Target::MoveIntentFromInput), &detourMoveIntent,
                 reinterpret_cast<void**>(&g_moveIntent), L"MoveIntentFromInput");

    if (hooks.create(scanner.address(Target::GetActorEffect), &detourGetActorEffect,
                     reinterpret_cast<void**>(&g_getActorEffect), L"GetActorEffect", HookGroup::Fullbright)) {
        const auto* const site =
            reinterpret_cast<const std::uint8_t*>(scanner.address(Target::FogColorNightVisionSite));
        const auto* const effect = reinterpret_cast<const std::uint8_t*>(scanner.address(Target::GetActorEffect));
        const void* returns[2]{};
        int found = 0;
        if (site != nullptr && effect != nullptr && memory::isReadable(site, 0x40)) {
            for (int i = 0; i + 10 <= 0x40 && found < 2; ++i) {
                if (site[i] != 0xBA || site[i + 1] != 0x10 || site[i + 2] != 0 || site[i + 3] != 0 || site[i + 4] != 0
                    || site[i + 5] != 0xE8) {
                    continue;
                }
                std::int32_t rel = 0;
                std::memcpy(&rel, site + i + 6, sizeof(rel));
                if (site + i + 10 + rel == effect) returns[found++] = site + i + 10;
            }
        }
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        if (found == 2) {
            Fullbright::instance().setFogColorReturns(returns[0], returns[1]);
            log().info(L"Hooks: the fog color asks for night vision from RVA {:#x} / {:#x} (Fullbright leaves the sky alone)",
                       reinterpret_cast<std::uintptr_t>(returns[0]) - base,
                       reinterpret_cast<std::uintptr_t>(returns[1]) - base);
        } else {
            log().error(L"Hooks: could not locate the fog color night vision calls ({} of 2); Fullbright is unavailable",
                        found);
        }
    }

    g_openInventoryScreen =
        scanner.addressAs<OpenInventoryScreenFn>(Target::OpenInventoryScreen);

    hooks.create(scanner.address(Target::HandleItemStackResponse),
                 &detourHandleItemStackResponse,
                 reinterpret_cast<void**>(&g_handleItemStackResponse),
                 L"HandleItemStackResponse");
    if (hooks.create(scanner.address(Target::CraftFromOutput), &detourCraftFromOutput,
                     reinterpret_cast<void**>(&g_craftFromOutput), L"CraftFromOutput")) {
        craftamount::markAvailable();
    }

    if (hooks.create(scanner.address(Target::ContainerSmHandle), &detourContainerSm,
                     reinterpret_cast<void**>(&g_containerSm), L"ContainerSm")) {
        containerui::setSmOriginal(g_containerSm);
    }
    hooks.create(scanner.address(Target::ContainerScreenDtor), &detourContainerScreenDtor,
                 reinterpret_cast<void**>(&g_containerScreenDtor), L"ContainerScreenDtor");
    hooks.create(scanner.address(Target::ContainerScreenTick), &detourContainerScreenTick,
                 reinterpret_cast<void**>(&g_containerScreenTick), L"ContainerScreenTick");

    if (hooks.create(scanner.address(Target::SceneStackPush), &detourSceneStackPush,
                     reinterpret_cast<void**>(&g_scenePush), L"SceneStackPush")) {
        const auto* const site =
            reinterpret_cast<const std::uint8_t*>(scanner.address(Target::OpenTradingScreenPush));
        if (site != nullptr && memory::isReadable(site, 0x50)) {
            int found = 0;
            for (int i = 0; i + 6 <= 0x50; ++i) {
                if (site[i] != 0xFF || site[i + 1] != 0x15) {
                    continue;
                }
                if (++found == 2) {
                    g_tradePushReturn = site + i + 6;
                    break;
                }
            }
        }
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        if (g_tradePushReturn != nullptr) {
            log().info(L"Hooks: the trade screen push returns to RVA {:#x} (auto trade keeps it off the stack)",
                       reinterpret_cast<std::uintptr_t>(g_tradePushReturn) - base);
        } else {
            log().warn(L"Hooks: could not locate the trade screen push site; the auto trade is off");
        }
    }
    if (hooks.create(scanner.address(Target::ContainerScreenCtor), &detourContainerScreenCtor,
                     reinterpret_cast<void**>(&g_containerScreenCtor), L"ContainerScreenCtor")) {
        containerui::setCtorAddress(scanner.address(Target::ContainerScreenCtor));
    }
    if (hooks.create(scanner.address(Target::HudScreenCtor), &detourHudScreenCtor,
                     reinterpret_cast<void**>(&g_hudScreenCtor), L"HudScreenCtor")) {
        containerui::setHudCtorAddress(scanner.address(Target::HudScreenCtor));
    }
    hooks.create(scanner.address(Target::RenderCurrentFrame), &detourRenderCurrentFrame,
                 reinterpret_cast<void**>(&g_renderCurrentFrame), L"RenderCurrentFrame");
    installSessionTicks(hooks, scanner);
    hooks.create(scanner.address(Target::PacketCheckSize), &detourPacketCheckSize,
                 reinterpret_cast<void**>(&g_packetCheckSize), L"PacketCheckSize");
    hooks.create(scanner.address(Target::NetworkSend), &detourNetworkSend,
                 reinterpret_cast<void**>(&g_networkSend), L"NetworkSend");
    hooks.create(scanner.address(Target::StartGameHandle), &detourStartGameHandle,
                 reinterpret_cast<void**>(&g_startGameHandle), L"StartGameHandle");
    if (std::byte* const site = scanner.address(Target::LevelChunkTickCall);
        site != nullptr && memory::isReadable(site, 42) && site[37] == std::byte{0xE8}) {
        void* const target = memory::ripTarget(site, 38);
        if (memory::inGameModule(target) && memory::isExecutable(target, 1)) {
            if (hooks.create(target, &detourLevelChunkTick, reinterpret_cast<void**>(&g_levelChunkTick),
                             L"LevelChunkTick")) {
                DebugScreen::onChunkTickHookReady();
            }
        }
    }
    hooks.create(scanner.address(Target::LegacyParticleRender), &detourLegacyParticleRender,
                 reinterpret_cast<void**>(&g_legacyParticleRender), L"LegacyParticleRender");
    if (void* const fmodUpdate = fmodstats::updateAddress(); fmodUpdate != nullptr) {
        hooks.create(fmodUpdate, &detourFmodSystemUpdate, reinterpret_cast<void**>(&g_fmodSystemUpdate),
                     L"FmodSystemUpdate");
    } else {
        log().info(L"DebugScreen: fmod.dll (or one of its functions) was not found; Sounds will not be shown");
    }
    if (scanner.found(Target::HudGetItemSite) && scanner.found(Target::ContainerGetItem)
        && hooks.create(scanner.address(Target::ContainerGetItem), &detourContainerGetItem,
                        reinterpret_cast<void**>(&g_containerGetItem), L"ContainerGetItem")) {
        containerui::setHudGetItemSite(scanner.address(Target::HudGetItemSite),
                                       scanner.address(Target::ContainerGetItem));
    }
    if (scanner.found(Target::HudGetItemSite)) {
        void* resolver = nullptr;
        void* base = nullptr;
        if (containerui::locateHudCollectionResolver(scanner.address(Target::HudGetItemSite), resolver, base)) {
            g_hudCollBase = reinterpret_cast<HudCollResolveFn>(base);
            hooks.create(resolver, &detourHudCollResolve, reinterpret_cast<void**>(&g_hudCollResolve),
                         L"HudCollectionResolve");
        }
    }
    hooks.create(scanner.address(Target::TradeHoverInvoke), &detourTradeHoverInvoke,
                 reinterpret_cast<void**>(&g_tradeHoverInvoke), L"TradeHoverInvoke");
    hooks.create(scanner.address(Target::TradeSecondaryInvoke), &detourTradeSecondaryInvoke,
                 reinterpret_cast<void**>(&g_tradeSecondaryInvoke), L"TradeSecondaryInvoke");
    {
        bool all = hooks.create(scanner.address(Target::TradeSelParse), &detourTradeSelParse,
                                reinterpret_cast<void**>(&g_tradeSelParse), L"TradeSelParse");
        struct TierHook {
            tradeui::TierBinding which;
            void* detour;
            void** original;
            const wchar_t* name;
        };
        const TierHook tierHooks[] = {
            {tradeui::TierBinding::SelectorTotal, reinterpret_cast<void*>(&detourTradeSelectorTotal),
             reinterpret_cast<void**>(&g_tradeSelectorTotal), L"TradeSelectorTotal"},
            {tradeui::TierBinding::TierTotal, reinterpret_cast<void*>(&detourTradeTierTotal),
             reinterpret_cast<void**>(&g_tradeTierTotal), L"TradeTierTotal"},
            {tradeui::TierBinding::TierVisible, reinterpret_cast<void*>(&detourTradeTierVisible),
             reinterpret_cast<void**>(&g_tradeTierVisible), L"TradeTierVisible"},
            {tradeui::TierBinding::TierUnlocked, reinterpret_cast<void*>(&detourTradeTierUnlocked),
             reinterpret_cast<void**>(&g_tradeTierUnlocked), L"TradeTierUnlocked"},
            {tradeui::TierBinding::TierName, reinterpret_cast<void*>(&detourTradeTierName),
             reinterpret_cast<void**>(&g_tradeTierName), L"TradeTierName"},
        };
        for (const TierHook& h : tierHooks) {
            const void* const at = tradeui::resolveTierBinding(h.which);
            if (at == nullptr) {
                log().warn(L"Detours: the trade tier binding {} was not found in the trade screen constructor", h.name);
                all = false;
                continue;
            }
            all = hooks.create(const_cast<void*>(at), h.detour, h.original, h.name) && all;
        }
        tradeui::setFavoriteTierHooked(all);
        tradeui::setTranslator(&translateInto);
    }
    if (hooks.create(scanner.address(Target::TradeCurrentTier), &detourTradeCurrentTier,
                     reinterpret_cast<void**>(&g_tradeCurrentTier), L"TradeCurrentTier")) {
        tradeui::setCurrentTierOriginal(reinterpret_cast<const void*>(g_tradeCurrentTier));
    }

    hooks.create(scanner.address(Target::InventoryHoveredSlot), &detourInventoryHoveredSlot,
                 reinterpret_cast<void**>(&g_inventoryHoveredSlot), L"InventoryHoveredSlot");

    hooks.create(scanner.address(Target::UiDefLookup), &detourUiDefLookup,
                 reinterpret_cast<void**>(&g_uiDefLookup), L"UiDefLookup");

    hooks.create(scanner.address(Target::SettingsGroupResolve), &detourSettingsGroupResolve,
                 reinterpret_cast<void**>(&g_settingsGroupResolve), L"SettingsGroupResolve");
    hooks.create(scanner.address(Target::SettingsGroupRegister), &detourSettingsGroupRegister,
                 reinterpret_cast<void**>(&g_settingsGroupRegister), L"SettingsGroupRegister");
    g_openHowToPlayScreen = reinterpret_cast<OpenHowToPlayScreenFn>(
        scanner.address(Target::OpenHowToPlayScreen));

    hooks.create(scanner.address(Target::SettingsProviderCall), &detourSettingsProviderCall,
                 reinterpret_cast<void**>(&g_settingsProviderCall), L"SettingsProviderCall");

    hooks.create(scanner.address(Target::SettingsFindComponent), &detourSettingsFindComponent,
                 reinterpret_cast<void**>(&g_settingsFindComponent), L"SettingsFindComponent");

    const auto* allocatorSite = scanner.address(Target::GameAllocate);
    unsigned char allocatorCode[14]{};
    if (allocatorSite && memory::copyGuarded(allocatorSite, allocatorCode, sizeof(allocatorCode))
        && allocatorCode[0] == 0x48 && allocatorCode[1] == 0x8b && allocatorCode[2] == 0x0d
        && std::memcmp(allocatorCode + 7, "\x48\x8b\x01\x48\x8b\x40\x08", 7) == 0) {
        auto* global = static_cast<void**>(memory::ripTarget(allocatorSite, 3));
        if (memory::inGameModule(global) && memory::isReadable(global, sizeof(void*))) {
            g_gameAllocatorGlobal = global;
            g_gameAllocateSlot = allocatorCode[13];
        }
    }

    hooks.create(scanner.address(Target::UiEventDispatch), &detourUiEventDispatch,
                 reinterpret_cast<void**>(&g_uiEventDispatch), L"UiEventDispatch");

    hooks.create(scanner.address(Target::UiSliderPublish), &detourUiSliderPublish,
                 reinterpret_cast<void**>(&g_uiSliderPublish), L"UiSliderPublish");
    resolveCtlBag(scanner.address(Target::UiSliderPublish));

    hooks.create(scanner.address(Target::FogSettingsFetch), &detourFogSettingsFetch,
                 reinterpret_cast<void**>(&g_fogSettingsFetch), L"FogSettingsFetch", HookGroup::Fog);
    hooks.create(scanner.address(Target::FogColorFetch), &detourFogColorFetch,
                 reinterpret_cast<void**>(&g_fogColorFetch), L"FogColorFetch", HookGroup::Fog);
    hooks.create(scanner.address(Target::ItemHoverTextBuild), &detourItemHoverTextBuild,
                 reinterpret_cast<void**>(&g_itemHoverTextBuild), L"ItemHoverTextBuild");
    hooks.create(scanner.address(Target::HoverRendererRender), &detourHoverRendererRender,
                 reinterpret_cast<void**>(&g_hoverRendererRender), L"HoverRendererRender");
    hooks.create(scanner.address(Target::MobEffectsRendererRender), &detourMobEffectsRendererRender,
                 reinterpret_cast<void**>(&g_mobEffectsRendererRender), L"MobEffectsRendererRender");
    hooks.create(scanner.address(Target::MobEffectsLayout), &detourMobEffectsLayout,
                 reinterpret_cast<void**>(&g_mobEffectsLayout), L"MobEffectsLayout");
    hooks.create(scanner.address(Target::LocatorBarRendererRender), &detourLocatorBarRendererRender,
                 reinterpret_cast<void**>(&g_locatorBarRendererRender), L"LocatorBarRendererRender");
    if (const std::byte* const iconCall = scanner.address(Target::LocatorBarIconCall)) {
        hooks.create(memory::ripTarget(iconCall, 85), &detourLocatorBarIcon,
                     reinterpret_cast<void**>(&g_locatorBarIcon), L"LocatorBarIcon");
    }
    NameTags::instance().setBuildHooked(hooks.create(scanner.address(Target::NameTagBuild), &detourNameTagBuild,
                                                     reinterpret_cast<void**>(&g_nameTagBuild), L"NameTagBuild"));
    hooks.create(scanner.address(Target::HungerRendererUpdate), &detourHungerRendererUpdate,
                 reinterpret_cast<void**>(&g_hungerRendererUpdate), L"HungerRendererUpdate");
    hooks.create(scanner.address(Target::HeartRendererUpdate), &detourHeartRendererUpdate,
                 reinterpret_cast<void**>(&g_heartRendererUpdate), L"HeartRendererUpdate");
    hooks.create(scanner.address(Target::AttackCore), &detourAttackCore, reinterpret_cast<void**>(&g_attackCore),
                 L"AttackCore", HookGroup::Tool);
    hooks.create(scanner.address(Target::SendComplexTransaction), &detourSendComplexTx,
                 reinterpret_cast<void**>(&g_sendComplexTx), L"SendComplexTransaction", HookGroup::Tool);
    if (std::byte* const insert = resolveLegacyParticleInsert(); insert != nullptr) {
        hooks.create(insert, &detourLegacyParticleInsert, reinterpret_cast<void**>(&g_legacyParticleInsert),
                     L"LegacyParticleInsert", HookGroup::Particles);
    }

    hooks.create(scanner.address(Target::OreKeyRowsBuild), &detourOreKeyRowsBuild,
                 reinterpret_cast<void**>(&g_oreKeyRowsBuild), L"OreKeyRowsBuild");

    if (void* const advance = oreuiscript::advanceAddress(); advance != nullptr) {
        hooks.create(advance, oreuiscript::advanceDetour(), oreuiscript::advanceOriginal(), L"OreUiViewAdvance");
    }

    hooks.create(ItemStackRequest::findContainerOpenHandle(), &detourContainerOpenHandle,
                 reinterpret_cast<void**>(&g_containerOpenHandle), L"ContainerOpenHandle");

    hooks.create(ItemStackRequest::findInventoryContentReader(), &detourInventoryContentRead,
                 reinterpret_cast<void**>(&g_inventoryContentRead), L"InventoryContentRead");

    if (std::byte* const cameraBase = scanner.address(Target::CameraUpdate)) {
        hooks.create(cameraBase + FreeCamera::kTrampolineOffset,
                     reinterpret_cast<void*>(&tsukuyomiCameraTrampolineEntry),
                     &tsukuyomiCameraTrampoline, L"CameraUpdate");
    }

    hooks.create(scanner.address(Target::PlayerView),
                 reinterpret_cast<void*>(&tsukuyomiPlayerViewTrampolineEntry),
                 &tsukuyomiPlayerViewTrampoline, L"PlayerView");

    const bool packetSendHooked = hooks.create(scanner.address(Target::PacketSend),
                 reinterpret_cast<void*>(&tsukuyomiPacketSendTrampolineEntry),
                 &tsukuyomiPacketSendTrampoline, L"PacketSend");
    bot::installHooks(hooks, packetSendHooked);

    render::installOverlayHooks();

    worldmesh::installHooks();
    gamemodewheel::installHooks();

    uiprobe::installPublishPump();

    hooks.applyQueued();

    installLate();
}

float callGetDestroySpeed(void* rcx, void* rdx, void* r8, void* r9)
{
    return g_getDestroySpeed != nullptr ? g_getDestroySpeed(rcx, rdx, r8, r9) : 0.0f;
}

void callSetSelectedSlot(void* rcx, void* rdx, void* r8, void* r9)
{
    if (g_setSelectedSlot != nullptr) {
        g_setSelectedSlot(rcx, rdx, r8, r9);
    }
}

bool callAttackCore(void* gameMode, void* target, bool direct, const void* hitPos)
{
    return g_attackCore != nullptr && g_attackCore(gameMode, target, direct, hitPos);
}

bool attackHooksInstalled()
{
    return g_attackCore != nullptr && g_sendComplexTx != nullptr;
}

bool callSendComplexTx(void* player, void** transaction)
{
    if (g_sendComplexTx == nullptr) {
        return false;
    }
    g_sendComplexTx(player, transaction);
    return true;
}

int callUseItem(void* gameMode, void* itemStack, int extra)
{
    return g_useItem != nullptr ? g_useItem(gameMode, itemStack, extra) : 0;
}

int callUseItemTransaction(void* gameMode, void* itemStack, int extra)
{
    return g_useItemTransaction != nullptr ? g_useItemTransaction(gameMode, itemStack, extra) : 0;
}

namespace {

int gameModeFaultFilter(unsigned long code)
{
    return (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR)
               ? EXCEPTION_EXECUTE_HANDLER
               : EXCEPTION_CONTINUE_SEARCH;
}

bool callBuildBlockGuarded(BuildBlockFn fn, void* gameMode, void* blockPos, unsigned char face,
                           unsigned char extra, bool simTick, bool& out)
{
    __try {
        out = fn(gameMode, blockPos, face, extra, simTick);
        return true;
    } __except (gameModeFaultFilter(GetExceptionCode())) {
        return false;
    }
}

bool redirectBuildRaw(BuildBlockFn fn, void* gameMode, std::int32_t* pos, unsigned char face,
                      const float* click, unsigned char extra, bool simTick, void* hit, bool& out)
{
    auto* const fields = static_cast<unsigned char*>(hit) + 0x18;
    unsigned char saved[0x20]{};
    if (!memory::isWritable(fields, sizeof(saved)) || !memory::copyGuarded(fields, saved, sizeof(saved))) return false;
    unsigned char* kept = nullptr;
    unsigned char* keptFlag = nullptr;
    unsigned char keptSaved[12]{};
    unsigned char keptFlagSaved = 0;
    if (g_keptClickFlag != 0 && g_keptClickPoint != 0) {
        auto* const base = static_cast<unsigned char*>(gameMode);
        if (memory::isWritable(base + g_keptClickFlag, 1) && memory::isWritable(base + g_keptClickPoint, sizeof(keptSaved))
            && memory::copyGuarded(base + g_keptClickFlag, &keptFlagSaved, 1)
            && memory::copyGuarded(base + g_keptClickPoint, keptSaved, sizeof(keptSaved))) {
            kept = base + g_keptClickPoint; keptFlag = base + g_keptClickFlag;
        }
    }
    const bool previous = t_modBuild;
    __try {
        __try {
            t_modBuild = true;
            const int type = 0, side = face;
            float point[3]{};
            for (int a = 0; a < 3; ++a) point[a] = static_cast<float>(pos[a]) + click[a];
            std::memcpy(fields, &type, 4); std::memcpy(fields + 4, &side, 4);
            std::memcpy(fields + 8, pos, 12); std::memcpy(fields + 20, point, 12);
            if (kept) std::memcpy(kept, point, sizeof(point));
            out = fn(gameMode, pos, face, extra, simTick);
        } __finally {
            std::memcpy(fields, saved, sizeof(saved));
            if (kept) { std::memcpy(kept, keptSaved, sizeof(keptSaved)); *keptFlag = keptFlagSaved; }
            t_modBuild = previous;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { t_modBuild = previous; return false; }
}

}

bool callBuildBlock(void* gameMode, void* blockPos, unsigned char face, unsigned char extra,
                    bool simTick)
{
    if (g_buildBlock == nullptr || gameMode == nullptr) {
        return false;
    }

    bool placed = false;
    const bool previous = t_modBuild;
    t_modBuild = true;
    const bool called = callBuildBlockGuarded(g_buildBlock, gameMode, blockPos, face, extra, simTick, placed);
    t_modBuild = previous;
    if (called) {
        return placed;
    }

    log().error(L"buildBlock faulted for game mode {:#x}, dropping it",
                reinterpret_cast<std::uintptr_t>(gameMode));
    GameData::instance().setGameMode(nullptr);
    return false;
}

bool callOriginalBuildBlock(void* gameMode, std::int32_t blockPos[3], unsigned char face,
                            const float click[3], unsigned char extra, bool simTick)
{
    void* const hit = hitResultAddress();
    if (!g_buildBlock || !gameMode || !hit || !blockPos || !click || face > 5) return false;
    bool placed = false;
    if (!redirectBuildRaw(g_buildBlock, gameMode, blockPos, face, click, extra, simTick, hit, placed)) {
        notice::failOnce("EasyPlace.hitResult", L"Easy Place: redirecting the hit position or the placement call failed.",
                         "Easy Place: could not redirect the placement hit result.");
        return false;
    }
    return placed;
}

namespace {
std::atomic<unsigned long> g_dryThread{0};
DryRun* g_dry = nullptr;
}

bool dryRunActive()
{
    const auto thread = g_dryThread.load(std::memory_order_relaxed);
    return thread != 0 && thread == GetCurrentThreadId();
}

void noteDryPacket()
{
    if (g_dry) ++g_dry->m_dropped;
}

void noteDryWrite(void* subChunk, unsigned layer, unsigned index, const void* block)
{
    if (!g_dry) return;
    DryWrite write;
    write.subChunk = subChunk; write.layer = layer; write.index = index; write.block = block;
    write.before = blockwrite::readBlockOf(subChunk, layer, index);
    g_dry->m_writes.push_back(write);
    if (g_dry->m_apply && write.before && g_subChunkSetBlock) {
        g_subChunkSetBlock(subChunk, layer, index, block);
        blockwrite::noteLiveSubChunk(subChunk);
    }
}

DryRun::DryRun(bool apply) : m_apply(apply)
{
    unsigned long expected = 0;
    if (!g_dryThread.compare_exchange_strong(expected, GetCurrentThreadId())) return;
    g_dry = this; m_ok = true;
}

DryRun::~DryRun()
{
    if (!m_ok) return;
    if (m_apply && g_subChunkSetBlock)
        for (auto it = m_writes.rbegin(); it != m_writes.rend(); ++it)
            if (it->before) {
                g_subChunkSetBlock(it->subChunk, it->layer, it->index, it->before);
                blockwrite::noteLiveSubChunk(it->subChunk);
            }
    g_dry = nullptr;
    g_dryThread.store(0, std::memory_order_relaxed);
}

bool DryRun::click(void* gameMode, std::int32_t blockPos[3], unsigned char face, const float click[3], bool& used)
{
    used = false;
    if (!m_ok) return false;
    const bool previous = t_suppressSwing;
    t_suppressSwing = true;
    used = callOriginalBuildBlock(gameMode, blockPos, face, click, 0, false);
    t_suppressSwing = previous;
    return true;
}

bool callBuildBlockAimedWithoutSwing(void* gameMode, std::int32_t blockPos[3], unsigned char face,
                                     const float click[3], unsigned char extra, bool simTick)
{
    void* const hit = hitResultAddress();
    if (!g_buildBlock || !gameMode || !hit || !blockPos || !click || face > 5) return false;
    const bool previous = t_suppressSwing;
    t_suppressSwing = true;
    bool placed = false;
    const bool called = redirectBuildRaw(g_buildBlock, gameMode, blockPos, face, click, extra, simTick, hit, placed);
    t_suppressSwing = previous;
    if (called) return placed;

    log().error(L"buildBlock (aimed) faulted for game mode {:#x}, dropping it",
                reinterpret_cast<std::uintptr_t>(gameMode));
    GameData::instance().setGameMode(nullptr);
    return false;
}

void callNotifyInventoryOpen(void* client)
{
    if (g_notifyInventoryOpen != nullptr && client != nullptr) {
        g_notifyInventoryOpen(client, 0);
    }
}

void* callUiDefLookup(void* self, const void* space, const void* name)
{
    if (g_uiDefLookup == nullptr || self == nullptr) {
        return nullptr;
    }
    return g_uiDefLookup(self, space, name);
}

bool setUiBagNumber(void* holder, const char* name, unsigned long long size, float value)
{
    if (g_uiBagSet == nullptr || holder == nullptr || name == nullptr) {
        return false;
    }
    const UiName key{name, size};
    g_uiBagSet(holder, &key, &value);
    return true;
}

void* uiBagSetFunction()
{
    return reinterpret_cast<void*>(g_uiBagSet);
}

void* gameClientInstance() { return g_clientInstance.load(std::memory_order_relaxed); }

bool translate(void* out, const char* key) { return translateInto(out, key); }

bool translateWith(void* out, const void* key, const void* params)
{
    if (g_i18nGetWithParams == nullptr || g_i18nSelf == nullptr || out == nullptr || key == nullptr || params == nullptr) {
        return false;
    }
    return translateWithParamsGuarded(out, key, params);
}

namespace {

constexpr std::size_t kScreenContextSlot = 0x718;
constexpr std::size_t kScreenStackBegin = 0x60;
constexpr std::size_t kScreenStackEnd = 0x68;

InventoryOpenResult openInventoryScreenGuarded(OpenInventoryScreenFn open, void* client)
{
    __try {
        void** const vtable = *static_cast<void***>(client);
        if (vtable == nullptr
            || !memory::isReadable(vtable, kScreenContextSlot + sizeof(void*))) {
            return InventoryOpenResult::NotReady;
        }
        void* const slot = vtable[kScreenContextSlot / sizeof(void*)];
        if (slot == nullptr || !memory::isExecutable(slot, 1)) {
            return InventoryOpenResult::NotReady;
        }
        void* const context = reinterpret_cast<ScreenContextFn>(slot)(client);
        if (context == nullptr
            || !memory::isReadable(context, kScreenStackEnd + sizeof(void*))) {
            return InventoryOpenResult::NotReady;
        }
        auto* const bytes = static_cast<std::byte*>(context);
        if (*reinterpret_cast<void**>(bytes + kScreenStackBegin)
            != *reinterpret_cast<void**>(bytes + kScreenStackEnd)) {
            return InventoryOpenResult::NotReady;
        }
        open(context);
        return InventoryOpenResult::Opened;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return InventoryOpenResult::Faulted;
    }
}

}

InventoryOpenResult callOpenInventoryScreen(void* client)
{
    if (g_openInventoryScreen == nullptr || client == nullptr) {
        return InventoryOpenResult::NotReady;
    }
    return openInventoryScreenGuarded(g_openInventoryScreen, client);
}

bool callOpenHowToPlayScreen()
{
    void* const client = g_clientInstance.load(std::memory_order_relaxed);
    if (g_openHowToPlayScreen == nullptr || client == nullptr) {
        return false;
    }
    void* fake[2] = {nullptr, client};
    __try {
        g_openHowToPlayScreen(&fake[0]);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

namespace {
std::ptrdiff_t g_getSceneStackSlot = -1;
std::ptrdiff_t g_popScreenSlot = -1;

bool resolvePopSlots()
{
    if (g_getSceneStackSlot > 0 && g_popScreenSlot > 0) {
        return true;
    }
    const auto* open = reinterpret_cast<const std::uint8_t*>(Scanner::instance().address(Target::OpenHowToPlayScreen));
    const auto* pop = reinterpret_cast<const std::uint8_t*>(Scanner::instance().address(Target::ScenePopCall));
    if (open == nullptr || pop == nullptr || !memory::isReadable(open, 40) || !memory::isReadable(pop, 16)) {
        return false;
    }
    if (open[30] != 0x48 || open[31] != 0x8B || open[32] != 0x80 || pop[7] != 0x48 || pop[8] != 0x8B || pop[9] != 0x80) {
        return false;
    }
    std::int32_t a = 0;
    std::int32_t b = 0;
    std::memcpy(&a, open + 33, 4);
    std::memcpy(&b, pop + 10, 4);
    if (a <= 0 || a > 0x2000 || (a % 8) != 0 || b <= 0 || b > 0x400 || (b % 8) != 0) {
        return false;
    }
    g_getSceneStackSlot = a;
    g_popScreenSlot = b;
    return true;
}

struct alignas(8) SceneStackRef {
    void* alive = nullptr;
    void* counts = nullptr;
    void* stack = nullptr;
};

using GetSceneStackFn = SceneStackRef*(__fastcall*)(void* client, SceneStackRef* out);
using PopScreenFn = void(__fastcall*)(void* stack, int count);
using CountsReleaseFn = void(__fastcall*)(void* counts);

bool popTopScreenGuarded(void* client)
{
    __try {
        void* const* vt = *static_cast<void* const* const*>(client);
        const auto get = reinterpret_cast<GetSceneStackFn>(vt[g_getSceneStackSlot / 8]);
        SceneStackRef ref;
        get(client, &ref);
        bool ok = false;
        if (ref.alive != nullptr && *static_cast<const volatile std::uint8_t*>(ref.alive) != 0 && ref.stack != nullptr) {
            void* const* svt = *static_cast<void* const* const*>(ref.stack);
            reinterpret_cast<PopScreenFn>(svt[g_popScreenSlot / 8])(ref.stack, 1);
            ok = true;
        }
        if (ref.counts != nullptr) {
            auto* const c = static_cast<std::uint8_t*>(ref.counts);
            void* const* cvt = *reinterpret_cast<void* const* const*>(c);
            if (_InterlockedDecrement(reinterpret_cast<volatile long*>(c + 8)) == 0) {
                reinterpret_cast<CountsReleaseFn>(cvt[0])(c);
                if (_InterlockedDecrement(reinterpret_cast<volatile long*>(c + 0xc)) == 0) {
                    reinterpret_cast<CountsReleaseFn>(cvt[1])(c);
                }
            }
        }
        return ok;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
}

bool popTopScreen()
{
    void* const client = g_clientInstance.load(std::memory_order_relaxed);
    if (client == nullptr || !resolvePopSlots()) {
        return false;
    }
    const bool ok = popTopScreenGuarded(client);
    static int said = 0;
    if (said < 4) {
        ++said;
        log().info(L"Hooks: popped the top screen {} (scene stack slots +{:#x} / +{:#x})", ok ? L"ok" : L"FAILED",
                   g_getSceneStackSlot, g_popScreenSlot);
    }
    return ok;
}

bool offstackScreenHeld()
{
    return g_offstack.scene != nullptr;
}

bool tickOffstackScreen()
{
    if (g_offstack.scene == nullptr || g_offstack.tick == nullptr) {
        return false;
    }
    if (GetCurrentThreadId() != g_offstack.thread) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            log().warn(L"Hooks: the trade screen was kept on thread {} but the frame runs on thread {}; not ticking it",
                       g_offstack.thread, GetCurrentThreadId());
        }
        return true;
    }
    return tickControllerGuarded(reinterpret_cast<CtrlTickFn>(g_offstack.tick), g_offstack.ctrl);
}

void releaseOffstackScreen(const wchar_t* why)
{
    if (g_offstack.scene == nullptr && g_offstack.counts == nullptr) {
        return;
    }
    if (g_offstack.thread != 0 && GetCurrentThreadId() != g_offstack.thread) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            log().warn(L"Hooks: releasing the kept trade screen from thread {} (it was kept on thread {})",
                       GetCurrentThreadId(), g_offstack.thread);
        }
    }
    const OffstackScreen held = g_offstack;
    g_offstack = OffstackScreen{};
    ItemScroller::instance().onOffstackTradeReleased();
    releaseCountsGuarded(held.counts);
    static int said = 0;
    if (said < 8) {
        ++said;
        log().info(L"Hooks: released the trade screen that was never pushed — {}", why);
    }
}

bool callSettingsGroupRegister(void* registry, const void* idView, void* provider)
{
    if (g_settingsGroupRegister == nullptr || registry == nullptr || idView == nullptr
        || provider == nullptr) {
        return false;
    }
    __try {
        g_settingsGroupRegister(registry, idView, provider, nullptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool callClientRefreshScreenSize(void* client)
{
    using Fn = void(__fastcall*)(void*);
    const auto fn = Scanner::instance().addressAs<Fn>(Target::ClientRefreshScreenSize);
    if (fn == nullptr || client == nullptr) return false;
    __try {
        fn(client);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool executableGameFunction(const void* fn, std::atomic<const void*>& checked)
{
    if (fn == checked.load(std::memory_order_relaxed)) return true;
    if (!memory::inGameModule(fn) || !memory::isExecutable(fn, 1)) return false;
    checked.store(fn, std::memory_order_relaxed);
    return true;
}
static std::atomic<const void*> g_checkedAllocate{nullptr};
static std::atomic<const void*> g_checkedFree{nullptr};

void* callGameAllocate(size_t size)
{
    constexpr size_t kMaxGameAllocation = 64u << 20;
    if (g_gameAllocatorGlobal == nullptr || size == 0 || size > kMaxGameAllocation) {
        return nullptr;
    }
    __try {
        void* allocator = *g_gameAllocatorGlobal;
        if (!allocator) return nullptr;
        auto* vtable = *static_cast<unsigned char**>(allocator);
        if (!memory::inGameModule(vtable)) return nullptr;
        const auto fn = *reinterpret_cast<GameAllocateFn*>(vtable + g_gameAllocateSlot);
        if (!executableGameFunction(reinterpret_cast<void*>(fn), g_checkedAllocate)) return nullptr;
        return fn(allocator, size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool callGameFree(void* data)
{
    if (!data) return true;
    if (!g_gameAllocatorGlobal) return false;
    constexpr std::size_t kFreeSlot = 0x10;
    using FreeFn = void(__fastcall*)(void*, void*);
    __try {
        void* allocator = *g_gameAllocatorGlobal;
        if (!allocator) return false;
        auto* vtable = *static_cast<unsigned char**>(allocator);
        if (!memory::inGameModule(vtable)) return false;
        const auto fn = *reinterpret_cast<FreeFn*>(vtable + kFreeSlot);
        if (!executableGameFunction(reinterpret_cast<void*>(fn), g_checkedFree)) return false;
        fn(allocator, data);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* callSettingsFindComponent(void* registry, const void* idView)
{
    if (g_settingsFindComponent == nullptr || registry == nullptr || idView == nullptr) {
        return nullptr;
    }
    unsigned char out[16]{};
    __try {
        g_settingsFindComponent(registry, out, idView, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    if (out[8] != 1) {
        return nullptr;
    }
    void* found = nullptr;
    std::memcpy(&found, out, sizeof(found));
    return found;
}

bool callGameModeContinueDestroyBlock(void* gameMode, const void* pos, std::uint8_t face,
                                      const void* playerPos, bool* out)
{
    return g_gameModeContinueDestroyBlock != nullptr
        ? g_gameModeContinueDestroyBlock(gameMode, pos, face, playerPos, out) : false;
}

bool callGameModeDestroyBlock(void* gameMode, const void* pos, std::uint8_t face)
{
    return g_gameModeDestroyBlock != nullptr ? g_gameModeDestroyBlock(gameMode, pos, face) : false;
}

bool hasGameModeContinueDestroyBlock() { return g_gameModeContinueDestroyBlock != nullptr; }
bool hasGameModeDestroyBlock() { return g_gameModeDestroyBlock != nullptr; }
bool hasNotifyInventoryOpen() { return g_notifyInventoryOpen != nullptr; }

}
