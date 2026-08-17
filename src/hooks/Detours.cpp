#include "hooks/Detours.h"

#include "core/Logger.h"
#include "game/CommandRequest.h"
#include "game/GameData.h"
#include "game/GameVersion.h"
#include "game/UiSound.h"
#include "game/InventoryActionBridge.h"
#include "game/InventoryScreen.h"
#include "game/ItemStackRequest.h"
#include "game/UiProbe.h"
#include "hooks/HookManager.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"
#include "modules/AntiDarkness.h"
#include "modules/AutoTool.h"
#include "modules/CreativeNoClip.h"
#include "modules/FastBlockPlacement.h"
#include "modules/FastRightClick.h"
#include "modules/FlySpeed.h"
#include "modules/GameModeSwitch.h"
#include "modules/HandRestock.h"
#include "modules/OffhandSwap.h"
#include "modules/FreeCamera.h"
#include "modules/Scaffold.h"
#include "render/Overlay.h"

#include <Windows.h>

#include <cmath>
#include <cstdint>

namespace tsukuyomi {

extern "C" {

void tsukuyomiCameraTrampolineEntry();
void* tsukuyomiCameraTrampoline = nullptr;

void tsukuyomiPlayerViewTrampolineEntry();
void* tsukuyomiPlayerViewTrampoline = nullptr;

void tsukuyomiPacketSendTrampolineEntry();
void* tsukuyomiPacketSendTrampoline = nullptr;

void tsukuyomiCameraHook(void* cameraBase)
{
    FreeCamera::instance().onCameraWrite(cameraBase);
}

using ViewVectorFn = void*(__fastcall*)(void* actor, float* out, float partial);
ViewVectorFn g_viewVector = nullptr;

void* __fastcall detourViewVector(void* actor, float* out, float partial)
{
    void* const result = (g_viewVector != nullptr) ? g_viewVector(actor, out, partial) : nullptr;
    if (FreeCamera::instance().enabled() && GameData::instance().isPlayerEntity(actor)) {
        FreeCamera::instance().freezeViewVector(out);
    }
    return result;
}

void tsukuyomiPlayerViewHook(void* viewBase)
{
    constexpr size_t kViewSize = sizeof(float) * 5;
    if (!memory::isReadable(viewBase, kViewSize)) {
        return;
    }

    const auto* const fields = reinterpret_cast<const float*>(viewBase);
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

    static std::atomic<bool> announced{false};
    if (!announced.exchange(true)) {
        log().info(L"Entered a world (view updates are arriving)");
    }

    FastBlockPlacement::instance().onPlayerViewUpdate();
    Scaffold::instance().onPlayerViewUpdate();

    GameModeSwitch::instance().onPlayerViewUpdate();

    UiSound::instance().pump();

    ItemStackRequest::instance().onFrame();

    InventoryActionBridge::instance().onFrame();

    HandRestock::instance().onPlayerViewUpdate();

    OffhandSwap::instance().onPlayerViewUpdate();

    uiprobe::pumpMenuSelection();

    uiprobe::pumpControlsKeybind();
}

unsigned char tsukuyomiShouldBlockPacket(void* packet)
{

    ItemStackRequest::instance().observePacket(packet);

    return InventoryActionBridge::instance().shouldBlockPacket(packet) ? 1u : 0u;
}

}

}

namespace tsukuyomi::hooks {

namespace {

using GetDestroySpeedFn = float(__fastcall*)(void*, void*, void*, void*);
using SetSelectedSlotFn = void(__fastcall*)(void*, void*, void*, void*);
using BuildBlockFn = bool(__fastcall*)(void*, void*, unsigned char, unsigned char);
using AbilitiesAccessFn = bool(__fastcall*)(void*, void*, void*, void*);

using UseItemFn = int(__fastcall*)(void*, void*);

using SetGameModeFn = void(__fastcall*)(void*, int, int);

using NotifyInventoryOpenFn = void(__fastcall*)(void*, int);

using MoveInputHandlerFn = void*(__fastcall*)(void*);

using ContainerOpenHandleFn = void(__fastcall*)(void*, void*, void*, void*);

using InventoryContentReadFn = void*(__fastcall*)(void*, void*, void*, void*);

using HandleItemStackResponseFn = void(__fastcall*)(void*, void*, void*, void*);

using InventoryHoveredSlotFn = void*(__fastcall*)(void*, void*);

using InventoryHotbarKeyFn = void(__fastcall*)(void*, void*, int);

using AddRequestActionFn = void(__fastcall*)(void**, void**);

using UiDefLookupFn = void*(__fastcall*)(void*, const void*, const void*);

using OptionRegisterFn = void*(__fastcall*)(void*, int, const void*);

using UiButtonMappingsFn = void*(__fastcall*)(void*, const void*, void*, void*);

using UiBagLookupFn = void*(__fastcall*)(void*, const char*, void*, void*);

using UiBagFindFn = void*(__fastcall*)(void*, const char*);

using UiBindingReadFn = void*(__fastcall*)(void*, void*, void*, void*);

using KeyDisplayNameFn = void(__fastcall*)(void*, void*, int);

using SettingsGroupRegisterFn = void*(__fastcall*)(void*, const void*, void*, void*);

using SettingsProviderCallFn = void*(__fastcall*)(void*, void*, void*, void*);

using SettingsGroupInfoUpdateFn = void(__fastcall*)(void*);

using SettingsFindComponentFn = void*(__fastcall*)(void*, void*, const void*, void*);

using GameAllocateFn = void*(__fastcall*)(void*, size_t);

using KeybindListBuildFn = void*(__fastcall*)(void*, void*, void*, void*);

using ControlsBindingNameFn = void*(__fastcall*)(void*, void*, void*, void*);

using ControlsRowBindingsFn = void*(__fastcall*)(void*, void*, void*, void*);

using ControlsSectionSetupFn = void*(__fastcall*)(void*, void*, void*, void*);

using OreFacetBindFn = void*(__fastcall*)(void*, void*, void*, unsigned);

using OreKeyboardInputGroupFn = void*(__fastcall*)(void*, void*, void*, void*);

using KeyActionNameFn = void*(__fastcall*)(void*, int);

using KeyRowListBuildFn = void*(__fastcall*)(void*, void*);

using OreKeyRowsBuildFn = void*(__fastcall*)(void*, void*, void*);

using KeyBindingLookupFn = void*(__fastcall*)(void*, const void*);

using OreKeyRowsWrapFn = void*(__fastcall*)(void*, void*);

using OreKeyRowsConsumeFn = void*(__fastcall*)(void*, void*);

using OreKeyRowDataFn = void*(__fastcall*)(void*, void*, std::uintptr_t, void*);

using RowDataCandFn = void*(__fastcall*)(void*, void*, void*, void*);

using OreKeyNameToIndexFn = void*(__fastcall*)(void*, void*, void*, void*);

using I18nGetFn = void*(__fastcall*)(void*, void*, const void*, void*);

using UiResolveVarFn = void*(__fastcall*)(void*, const void*, void*, void*);

using UiEventDispatchFn = int(__fastcall*)(void*, const void*, void*, void*);

GetDestroySpeedFn g_getDestroySpeed = nullptr;
SetSelectedSlotFn g_setSelectedSlot = nullptr;
BuildBlockFn g_buildBlock = nullptr;
AbilitiesAccessFn g_abilitiesAccess = nullptr;
UseItemFn g_useItem = nullptr;
UseItemFn g_useItemTransaction = nullptr;
SetGameModeFn g_setGameMode = nullptr;
NotifyInventoryOpenFn g_notifyInventoryOpen = nullptr;
MoveInputHandlerFn g_moveInputHandler = nullptr;
ContainerOpenHandleFn g_containerOpenHandle = nullptr;
InventoryContentReadFn g_inventoryContentRead = nullptr;

InventoryContentReadFn g_containerOpenRead = nullptr;
HandleItemStackResponseFn g_handleItemStackResponse = nullptr;
InventoryHoveredSlotFn g_inventoryHoveredSlot = nullptr;
InventoryHotbarKeyFn g_inventoryHotbarKey = nullptr;
AddRequestActionFn g_addRequestAction = nullptr;
UiDefLookupFn g_uiDefLookup = nullptr;
OptionRegisterFn g_optionRegister = nullptr;
UiButtonMappingsFn g_uiButtonMappings = nullptr;
UiBagLookupFn g_uiBagLookup = nullptr;
UiBagFindFn g_uiBagFind = nullptr;
UiResolveVarFn g_uiResolveVar = nullptr;
UiEventDispatchFn g_uiEventDispatch = nullptr;
UiBindingReadFn g_uiBindingRead = nullptr;
KeyDisplayNameFn g_keyDisplayName = nullptr;
SettingsGroupRegisterFn g_settingsGroupRegister = nullptr;
SettingsProviderCallFn g_settingsProviderCall = nullptr;
SettingsGroupInfoUpdateFn g_settingsGroupInfoUpdate = nullptr;
SettingsFindComponentFn g_settingsFindComponent = nullptr;
GameAllocateFn g_gameAllocate = nullptr;
KeybindListBuildFn g_keybindListBuild = nullptr;
ControlsBindingNameFn g_controlsBindingName = nullptr;
ControlsRowBindingsFn g_controlsRowBindings = nullptr;
ControlsSectionSetupFn g_controlsSectionSetup = nullptr;
OreFacetBindFn g_oreFacetBind = nullptr;
OreKeyboardInputGroupFn g_oreKeyboardInputGroup = nullptr;
KeyActionNameFn g_keyActionName = nullptr;
KeyRowListBuildFn g_keyRowListBuild = nullptr;
OreKeyRowsBuildFn g_oreKeyRowsBuild = nullptr;
KeyBindingLookupFn g_keyBindingLookup = nullptr;
OreKeyRowsWrapFn g_oreKeyRowsWrap = nullptr;
OreKeyRowsConsumeFn g_oreKeyRowsConsume = nullptr;
OreKeyRowDataFn g_oreKeyRowData = nullptr;
RowDataCandFn g_rowDataCandA = nullptr;
RowDataCandFn g_rowDataCandB = nullptr;
OreKeyNameToIndexFn g_oreKeyNameToIndex = nullptr;
I18nGetFn g_i18nGet = nullptr;

std::atomic<int> g_keyRowConsumeDepth{0};

constexpr bool kKeepRowsAfterConsume = true;

float __fastcall detourGetDestroySpeed(void* rcx, void* rdx, void* r8, void* r9)
{
    return AutoTool::instance().onGetDestroySpeed(rcx, rdx, r8, r9);
}

void __fastcall detourSetSelectedSlot(void* rcx, void* rdx, void* r8, void* r9)
{

    HandRestock::instance().onSetSelectedSlot(rcx);
    OffhandSwap::instance().onSetSelectedSlot(rcx);

    InventoryActionBridge::instance().onSetSelectedSlot(rcx);

    AutoTool::instance().onSetSelectedSlot(rcx, rdx, r8, r9);
}

void __fastcall detourHandleItemStackResponse(void* rcx, void* rdx, void* r8, void* r9)
{
    ItemStackRequest::instance().onResponse(rdx);

    if (g_handleItemStackResponse != nullptr) {
        g_handleItemStackResponse(rcx, rdx, r8, r9);
    }
}

bool __fastcall detourBuildBlock(void* gameMode, void* blockPos, unsigned char face,
                                 unsigned char extra)
{

    GameData::instance().setGameMode(gameMode);

    return FastBlockPlacement::instance().onBuildBlock(gameMode, blockPos, face, extra);
}

bool __fastcall detourAbilitiesAccess(void* rcx, void* rdx, void* r8, void* r9)
{
    CreativeNoClip::instance().onAbilitiesAccess(rdx);
    FlySpeed::instance().onAbilitiesAccess(rdx);
    return g_abilitiesAccess != nullptr ? g_abilitiesAccess(rcx, rdx, r8, r9) : false;
}

int __fastcall detourUseItem(void* gameMode, void* itemStack)
{
    return FastRightClick::instance().onUseItem(gameMode, itemStack);
}

int __fastcall detourUseItemTransaction(void* gameMode, void* itemStack)
{
    return FastRightClick::instance().onUseItemTransaction(gameMode, itemStack);
}

void __fastcall detourSetGameMode(void* self, int mode, int extra)
{
    GameModeSwitch::instance().onSetGameMode(self, mode, extra);
    CommandRequest::instance().onEntityContext(self);

    if (g_setGameMode != nullptr) {
        g_setGameMode(self, mode, extra);
    }
}

void __fastcall detourNotifyInventoryOpen(void* client, int which)
{
    ItemStackRequest::instance().onNotifyInventoryOpen(client);

    if (g_notifyInventoryOpen != nullptr) {
        g_notifyInventoryOpen(client, which);
    }
}

void* __fastcall detourMoveInputHandler(void* client)
{
    if (ItemStackRequest::instance().suppressingInputReset()) {
        return nullptr;
    }
    return (g_moveInputHandler != nullptr) ? g_moveInputHandler(client) : nullptr;
}

void __fastcall detourContainerOpenHandle(void* packet, void* result, void* callback,
                                          void* network)
{
    if (ItemStackRequest::instance().onContainerOpenHandle(packet, result)) {
        return;
    }

    if (g_containerOpenHandle != nullptr) {
        g_containerOpenHandle(packet, result, callback, network);

        ItemStackRequest::instance().rememberContainerOpenResult(result);
    }
}

InventoryContentReadFn g_mobEffectRead = nullptr;

void* __fastcall detourMobEffectRead(void* packet, void* out, void* stream, void* network)
{
    if (g_mobEffectRead == nullptr) {
        return nullptr;
    }
    void* const result = g_mobEffectRead(packet, out, stream, network);

    AntiDarkness::instance().onMobEffectRead(packet);
    return result;
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

void* __fastcall detourContainerOpenRead(void* packet, void* out, void* stream, void* network)
{
    if (g_containerOpenRead == nullptr) {
        return out;
    }
    void* const result = g_containerOpenRead(packet, out, stream, network);
    InventoryActionBridge::instance().onContainerOpen(packet);
    return result;
}

void* __fastcall detourInventoryHoveredSlot(void* controller, void* out)
{
    InventoryScreen::instance().onController(controller);

    return g_inventoryHoveredSlot != nullptr ? g_inventoryHoveredSlot(controller, out) : out;
}

void __fastcall detourInventoryHotbarKey(void* controller, void* collection, int hotbarIndex)
{

    InventoryScreen::instance().onController(controller);

    if (OffhandSwap::instance().onInventoryHotbarKey(controller, hotbarIndex)) {
        return;
    }

    if (g_inventoryHotbarKey != nullptr) {
        g_inventoryHotbarKey(controller, collection, hotbarIndex);
    }
}

void* __fastcall detourUiDefLookup(void* self, const void* space, const void* name)
{

    uiprobe::onLookup(space, name);
    void* const value = (g_uiDefLookup != nullptr) ? g_uiDefLookup(self, space, name) : nullptr;

    uiprobe::onLookupResult(space, name, value);

    if (void* const swapped = uiprobe::substitute(self, space, name)) {
        return swapped;
    }
    return value;
}

void* __fastcall detourOptionRegister(void* self, int id, const void* name)
{
    uiprobe::onOptionRegister(self, id, name);
    return (g_optionRegister != nullptr) ? g_optionRegister(self, id, name) : nullptr;
}

void* __fastcall detourUiButtonMappings(void* self, const void* bag, void* out, void* r9)
{
    uiprobe::onButtonMappingsBegin(out);
    void* const result = (g_uiButtonMappings != nullptr)
                             ? g_uiButtonMappings(self, bag, out, r9)
                             : nullptr;
    uiprobe::onButtonMappingsEnd(out);
    return result;
}

void* __fastcall detourUiBagLookup(void* self, const char* key, void* r8, void* r9)
{
    void* const result =
        (g_uiBagLookup != nullptr) ? g_uiBagLookup(self, key, r8, r9) : nullptr;
    uiprobe::onBagLookup(self, key, result);
    return result;
}

void* __fastcall detourUiBindingRead(void* self, void* bag, void* r8, void* r9)
{
    uiprobe::onBindingRead(bag);
    return (g_uiBindingRead != nullptr) ? g_uiBindingRead(self, bag, r8, r9) : nullptr;
}

void* __fastcall detourKeybindListBuild(void* self, void* rdx, void* r8, void* r9)
{
    void* const result =
        (g_keybindListBuild != nullptr) ? g_keybindListBuild(self, rdx, r8, r9) : nullptr;

    uiprobe::onKeybindListBuilt(self);
    return result;
}

void* __fastcall detourControlsBindingName(void* self, void* rdx, void* r8, void* r9)
{
    uiprobe::onControlsBindingName(self, r8);
    return (g_controlsBindingName != nullptr) ? g_controlsBindingName(self, rdx, r8, r9)
                                              : nullptr;
}

void* __fastcall detourControlsRowBindings(void* rcx, void* rdx, void* ctx, void* r9)
{
    uiprobe::onControlsRowBindings(rcx, rdx, ctx);
    return (g_controlsRowBindings != nullptr) ? g_controlsRowBindings(rcx, rdx, ctx, r9)
                                              : nullptr;
}

void* __fastcall detourControlsSectionSetup(void* self, void* rdx, void* r8, void* r9)
{
    uiprobe::onControlsSectionSetup(self, rdx);
    return (g_controlsSectionSetup != nullptr) ? g_controlsSectionSetup(self, rdx, r8, r9)
                                               : nullptr;
}

void* __fastcall detourOreFacetBind(void* out, void* rdx, void* name, unsigned flag)
{
    uiprobe::onOreFacetBind(out, rdx, name, flag);
    return (g_oreFacetBind != nullptr) ? g_oreFacetBind(out, rdx, name, flag) : nullptr;
}

void* __fastcall detourOreKeyboardInputGroup(void* self, void* out, void* r8, void* r9)
{
    void* const result = (g_oreKeyboardInputGroup != nullptr)
                             ? g_oreKeyboardInputGroup(self, out, r8, r9)
                             : nullptr;

    uiprobe::onOreKeyboardInputGroup(self, out);
    return result;
}

void* __fastcall detourKeyActionName(void* out, int index)
{
    void* const result = (g_keyActionName != nullptr) ? g_keyActionName(out, index) : nullptr;

    uiprobe::onKeyActionName(out, index);

    uiprobe::overrideKeyActionName(out, index);
    return result;
}

void* __fastcall detourKeyRowListBuild(void* array, void* rdx)
{
    uiprobe::onKeyRowListBuild(array, rdx, false);
    void* const result = (g_keyRowListBuild != nullptr) ? g_keyRowListBuild(array, rdx) : nullptr;

    uiprobe::onKeyRowListBuild(array, rdx, true);
    return result;
}

void* __fastcall detourOreKeyRowsBuild(void* out, void* rdx, void* r8)
{
    uiprobe::onOreKeyRowsBegin();

    constexpr bool kDumpRowContainer = false;
    if (kDumpRowContainer) {
        uiprobe::dumpKeyRowContainer(rdx, r8);
    }

    constexpr bool kBumpRowLimit = false;
    const int bumped = kBumpRowLimit ? uiprobe::bumpKeyRowLimit(rdx, +1) : 0;

    constexpr bool kAddRowEntry = false;

    constexpr bool kPurgeOwnRows = false;
    const bool purged = kPurgeOwnRows && !kAddRowEntry && uiprobe::purgeOwnKeyRows(rdx);
    const bool substituted =
        !purged && (kAddRowEntry && bumped == 0)
        && uiprobe::substituteKeyRows(rdx, kKeepRowsAfterConsume);
    void* const result =
        (g_oreKeyRowsBuild != nullptr) ? g_oreKeyRowsBuild(out, rdx, r8) : nullptr;

    if (substituted && !kKeepRowsAfterConsume && g_keyRowConsumeDepth.load() == 0) {
        uiprobe::popKeyRowSubstitution();
    }
    if (bumped != 0) {
        uiprobe::bumpKeyRowLimit(rdx, -bumped);
    }
    uiprobe::onOreKeyRowsEnd(out);
    return result;
}

void* __fastcall detourKeyBindingLookup(void* container, const void* name)
{
    uiprobe::onKeyBindingLookup(name);
    return (g_keyBindingLookup != nullptr) ? g_keyBindingLookup(container, name) : nullptr;
}

void* __fastcall detourOreKeyRowsWrap(void* out, void* container)
{
    uiprobe::onOreKeyRowsWrap(out, container);
    return (g_oreKeyRowsWrap != nullptr) ? g_oreKeyRowsWrap(out, container) : nullptr;
}

void* __fastcall detourOreKeyRowsConsume(void* rcx, void* rdx)
{
    uiprobe::onOreKeyRowsConsume(rcx, rdx, false);
    g_keyRowConsumeDepth.fetch_add(1);
    void* const result =
        (g_oreKeyRowsConsume != nullptr) ? g_oreKeyRowsConsume(rcx, rdx) : nullptr;

    if (g_keyRowConsumeDepth.fetch_sub(1) == 1 && !kKeepRowsAfterConsume) {
        uiprobe::restoreKeyRows();
    }
    uiprobe::onOreKeyRowsConsume(rcx, rdx, true);
    return result;
}

void* __fastcall detourOreKeyRowData(void* out, void* container, std::uintptr_t index, void* r9)
{
    constexpr bool kSubstituteInRowData = false;
    const bool substituted = kSubstituteInRowData && uiprobe::substituteKeyRows(container);
    uiprobe::onOreKeyRowData(container, index, substituted);
    void* const result =
        (g_oreKeyRowData != nullptr) ? g_oreKeyRowData(out, container, index, r9) : nullptr;
    uiprobe::onOreKeyRowDataResult(out, index);
    if (substituted) {
        uiprobe::popKeyRowSubstitution();
    }
    return result;
}

void* __fastcall detourRowDataCandA(void* a, void* b, void* c, void* d)
{
    uiprobe::onRowDataCandidate(0, d);
    return (g_rowDataCandA != nullptr) ? g_rowDataCandA(a, b, c, d) : nullptr;
}

void* __fastcall detourRowDataCandB(void* a, void* b, void* c, void* d)
{
    uiprobe::onRowDataCandidate(1, d);
    return (g_rowDataCandB != nullptr) ? g_rowDataCandB(a, b, c, d) : nullptr;
}

void* __fastcall detourOreKeyNameToIndex(void* a, void* b, void* c, void* d)
{
    uiprobe::onOreKeyNameToIndex(a);
    return (g_oreKeyNameToIndex != nullptr) ? g_oreKeyNameToIndex(a, b, c, d) : nullptr;
}

void* __fastcall detourI18nGet(void* self, void* out, const void* key, void* r9)
{

    constexpr bool kProbeTranslate = true;
    if (kProbeTranslate) {
        uiprobe::onTranslate(key);
    }
    void* const result = (g_i18nGet != nullptr) ? g_i18nGet(self, out, key, r9) : nullptr;

    constexpr bool kOverrideRowName = true;
    if (kOverrideRowName) {
        uiprobe::overrideTranslation(key, out);
    }
    return result;
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

void __fastcall detourSettingsGroupInfoUpdate(void* self)
{
    const bool swapped = uiprobe::beforeSettingsGroupInfoUpdate(self);
    if (g_settingsGroupInfoUpdate != nullptr) {
        g_settingsGroupInfoUpdate(self);
    }
    uiprobe::afterSettingsGroupInfoUpdate(self, swapped);
}

void* __fastcall detourSettingsFindComponent(void* self, void* out, const void* idView, void* r9)
{
    void* const result = (g_settingsFindComponent != nullptr)
                             ? g_settingsFindComponent(self, out, idView, r9)
                             : nullptr;
    uiprobe::onSettingsFindComponent(self, out, idView);
    return result;
}

void* __fastcall detourUiResolveVar(void* self, const void* name, void* r8, void* r9)
{
    void* const saved = uiprobe::onResolveVarBegin(self, name);
    void* const result =
        (g_uiResolveVar != nullptr) ? g_uiResolveVar(self, name, r8, r9) : nullptr;
    uiprobe::onResolveVarEnd(saved);
    return result;
}

int __fastcall detourUiEventDispatch(void* self, const void* event, void* r8, void* r9)
{
    uiprobe::onUiEvent(self, event);
    return (g_uiEventDispatch != nullptr) ? g_uiEventDispatch(self, event, r8, r9) : 0;
}

void __fastcall detourAddRequestAction(void** clientHolder, void** action)
{
    InventoryActionBridge::instance().onAddRequestAction(clientHolder, action);

    if (g_addRequestAction != nullptr) {
        g_addRequestAction(clientHolder, action);
    }
}

}

void installAll()
{
    const Scanner& scanner = Scanner::instance();
    HookManager& hooks = HookManager::instance();

    hooks.create(scanner.address(Target::GetDestroySpeed), &detourGetDestroySpeed,
                 reinterpret_cast<void**>(&g_getDestroySpeed), L"GetDestroySpeed");

    hooks.create(scanner.address(Target::SetSelectedSlot), &detourSetSelectedSlot,
                 reinterpret_cast<void**>(&g_setSelectedSlot), L"SetSelectedSlot");

    hooks.create(scanner.address(Target::BuildBlock), &detourBuildBlock,
                 reinterpret_cast<void**>(&g_buildBlock), L"buildBlock");

    hooks.create(scanner.address(Target::ViewVector), &detourViewVector,
                 reinterpret_cast<void**>(&g_viewVector), L"ViewVector");

    hooks.create(scanner.address(Target::AbilitiesAccess), &detourAbilitiesAccess,
                 reinterpret_cast<void**>(&g_abilitiesAccess), L"AbilitiesAccess");

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

    hooks.create(scanner.address(Target::HandleItemStackResponse),
                 &detourHandleItemStackResponse,
                 reinterpret_cast<void**>(&g_handleItemStackResponse),
                 L"HandleItemStackResponse");

    hooks.create(scanner.address(Target::InventoryHoveredSlot), &detourInventoryHoveredSlot,
                 reinterpret_cast<void**>(&g_inventoryHoveredSlot), L"InventoryHoveredSlot");

    hooks.create(scanner.address(Target::InventoryHotbarKey), &detourInventoryHotbarKey,
                 reinterpret_cast<void**>(&g_inventoryHotbarKey), L"InventoryHotbarKey");

    hooks.create(scanner.address(Target::UiDefLookup), &detourUiDefLookup,
                 reinterpret_cast<void**>(&g_uiDefLookup), L"UiDefLookup");

    hooks.create(scanner.address(Target::OptionRegister), &detourOptionRegister,
                 reinterpret_cast<void**>(&g_optionRegister), L"OptionRegister");

    hooks.create(scanner.address(Target::UiButtonMappings), &detourUiButtonMappings,
                 reinterpret_cast<void**>(&g_uiButtonMappings), L"UiButtonMappings");

    g_uiBagFind = scanner.addressAs<UiBagFindFn>(Target::UiBagFind);

    g_keyDisplayName = scanner.addressAs<KeyDisplayNameFn>(Target::KeyDisplayName);

    hooks.create(scanner.address(Target::SettingsGroupRegister), &detourSettingsGroupRegister,
                 reinterpret_cast<void**>(&g_settingsGroupRegister), L"SettingsGroupRegister");

    hooks.create(scanner.address(Target::SettingsProviderCall), &detourSettingsProviderCall,
                 reinterpret_cast<void**>(&g_settingsProviderCall), L"SettingsProviderCall");

    hooks.create(scanner.address(Target::SettingsGroupInfoUpdate), &detourSettingsGroupInfoUpdate,
                 reinterpret_cast<void**>(&g_settingsGroupInfoUpdate), L"SettingsGroupInfoUpdate");

    hooks.create(scanner.address(Target::SettingsFindComponent), &detourSettingsFindComponent,
                 reinterpret_cast<void**>(&g_settingsFindComponent), L"SettingsFindComponent");

    g_gameAllocate = scanner.addressAs<GameAllocateFn>(Target::GameAllocate);

    hooks.create(scanner.address(Target::UiResolveVar), &detourUiResolveVar,
                 reinterpret_cast<void**>(&g_uiResolveVar), L"UiResolveVar");

    hooks.create(scanner.address(Target::UiEventDispatch), &detourUiEventDispatch,
                 reinterpret_cast<void**>(&g_uiEventDispatch), L"UiEventDispatch");

    (void)&detourUiBindingRead;

    hooks.create(scanner.address(Target::KeybindListBuild), &detourKeybindListBuild,
                 reinterpret_cast<void**>(&g_keybindListBuild), L"KeybindListBuild");

    hooks.create(scanner.address(Target::ControlsBindingName), &detourControlsBindingName,
                 reinterpret_cast<void**>(&g_controlsBindingName), L"ControlsBindingName");

    hooks.create(scanner.address(Target::ControlsRowBindings), &detourControlsRowBindings,
                 reinterpret_cast<void**>(&g_controlsRowBindings), L"ControlsRowBindings");

    hooks.create(scanner.address(Target::ControlsSectionSetup), &detourControlsSectionSetup,
                 reinterpret_cast<void**>(&g_controlsSectionSetup), L"ControlsSectionSetup");

    hooks.create(scanner.address(Target::OreFacetBind), &detourOreFacetBind,
                 reinterpret_cast<void**>(&g_oreFacetBind), L"OreFacetBind");
    hooks.create(scanner.address(Target::OreKeyboardInputGroup), &detourOreKeyboardInputGroup,
                 reinterpret_cast<void**>(&g_oreKeyboardInputGroup), L"OreKeyboardInputGroup");

    hooks.create(scanner.address(Target::KeyActionName), &detourKeyActionName,
                 reinterpret_cast<void**>(&g_keyActionName), L"KeyActionName");

    hooks.create(scanner.address(Target::KeyRowListBuild), &detourKeyRowListBuild,
                 reinterpret_cast<void**>(&g_keyRowListBuild), L"KeyRowListBuild");

    hooks.create(scanner.address(Target::OreKeyRowsBuild), &detourOreKeyRowsBuild,
                 reinterpret_cast<void**>(&g_oreKeyRowsBuild), L"OreKeyRowsBuild");

    hooks.create(scanner.address(Target::KeyBindingLookup), &detourKeyBindingLookup,
                 reinterpret_cast<void**>(&g_keyBindingLookup), L"KeyBindingLookup");

    hooks.create(scanner.address(Target::OreKeyRowsWrap), &detourOreKeyRowsWrap,
                 reinterpret_cast<void**>(&g_oreKeyRowsWrap), L"OreKeyRowsWrap");

    uiprobe::setOreKeyRowsWrapAddr(scanner.address(Target::OreKeyRowsWrap));

    hooks.create(scanner.address(Target::OreKeyRowsConsume), &detourOreKeyRowsConsume,
                 reinterpret_cast<void**>(&g_oreKeyRowsConsume), L"OreKeyRowsConsume");

    constexpr bool kHookOreKeyRowData = true;
    if (void* const consume = kHookOreKeyRowData ? scanner.address(Target::OreKeyRowsConsume)
                                                 : nullptr;
        consume != nullptr) {
        const auto at = reinterpret_cast<const unsigned char*>(consume);

        constexpr std::ptrdiff_t kTableLea = 0x6B;
        if (memory::isReadable(at + kTableLea, 7)) {
            std::int32_t disp = 0;
            std::memcpy(&disp, at + kTableLea + 3, sizeof(disp));
            const auto* const table = at + kTableLea + 7 + disp;
            if (memory::isReadable(table, 0x80)) {
                void* fn = nullptr;
                std::memcpy(&fn, table + 0x48, sizeof(fn));
                log().info(L"OreKeyRowData: table {:#x} fn {:#x}",
                           reinterpret_cast<std::uintptr_t>(table),
                           reinterpret_cast<std::uintptr_t>(fn));
                if (fn != nullptr) {
                    hooks.create(fn, &detourOreKeyRowData,
                                 reinterpret_cast<void**>(&g_oreKeyRowData), L"OreKeyRowData");
                }
            }
        }
    }

    hooks.create(scanner.address(Target::RowDataCandA), &detourRowDataCandA,
                 reinterpret_cast<void**>(&g_rowDataCandA), L"RowDataCandA");
    hooks.create(scanner.address(Target::RowDataCandB), &detourRowDataCandB,
                 reinterpret_cast<void**>(&g_rowDataCandB), L"RowDataCandB");

    if (void* const anchor = scanner.address(Target::I18nAnchor); anchor != nullptr) {
        const auto at = reinterpret_cast<const unsigned char*>(anchor);

        if (memory::isReadable(at, 21)) {
            std::int32_t disp = 0;
            std::memcpy(&disp, at + 17, sizeof(disp));
            const auto global = reinterpret_cast<void* const*>(at + 21 + disp);
            if (memory::isReadable(global, sizeof(void*))) {
                const auto vtable = reinterpret_cast<void* const*>(*global);
                if (vtable != nullptr
                    && memory::isReadable(vtable, 0x88)) {
                    void* const fn = vtable[0x80 / sizeof(void*)];
                    log().info(L"Localization: global {:#x} vtable {:#x} fn {:#x}",
                               reinterpret_cast<std::uintptr_t>(global),
                               reinterpret_cast<std::uintptr_t>(vtable),
                               reinterpret_cast<std::uintptr_t>(fn));
                    if (fn != nullptr) {
                        hooks.create(fn, &detourI18nGet,
                                     reinterpret_cast<void**>(&g_i18nGet), L"I18nGet");
                    }
                }
            }
        }
    }

    constexpr bool kHookKeyNameToIndex = false;
    if (kHookKeyNameToIndex) {
        hooks.create(scanner.address(Target::OreKeyNameToIndex), &detourOreKeyNameToIndex,
                     reinterpret_cast<void**>(&g_oreKeyNameToIndex), L"OreKeyNameToIndex");
    } else {
        (void)&detourOreKeyNameToIndex;
        (void)g_oreKeyNameToIndex;
    }

    hooks.create(scanner.address(Target::AddRequestAction), &detourAddRequestAction,
                 reinterpret_cast<void**>(&g_addRequestAction), L"AddRequestAction");

    hooks.create(ItemStackRequest::findContainerOpenHandle(), &detourContainerOpenHandle,
                 reinterpret_cast<void**>(&g_containerOpenHandle), L"ContainerOpenHandle");

    hooks.create(AntiDarkness::findReader(), &detourMobEffectRead,
                 reinterpret_cast<void**>(&g_mobEffectRead), L"MobEffectRead");

    hooks.create(ItemStackRequest::findInventoryContentReader(), &detourInventoryContentRead,
                 reinterpret_cast<void**>(&g_inventoryContentRead), L"InventoryContentRead");

    hooks.create(ItemStackRequest::findContainerOpenReader(), &detourContainerOpenRead,
                 reinterpret_cast<void**>(&g_containerOpenRead), L"ContainerOpenRead");

    if (std::byte* const cameraBase = scanner.address(Target::CameraUpdate)) {
        hooks.create(cameraBase + FreeCamera::kTrampolineOffset,
                     reinterpret_cast<void*>(&tsukuyomiCameraTrampolineEntry),
                     &tsukuyomiCameraTrampoline, L"CameraUpdate");
    }

    hooks.create(scanner.address(Target::PlayerView),
                 reinterpret_cast<void*>(&tsukuyomiPlayerViewTrampolineEntry),
                 &tsukuyomiPlayerViewTrampoline, L"PlayerView");

    hooks.create(scanner.address(Target::PacketSend),
                 reinterpret_cast<void*>(&tsukuyomiPacketSendTrampolineEntry),
                 &tsukuyomiPacketSendTrampoline, L"PacketSend");

    render::installOverlayHooks();

    uiprobe::installPublishPump();

    hooks.applyQueued();
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

int callUseItem(void* gameMode, void* itemStack)
{
    return g_useItem != nullptr ? g_useItem(gameMode, itemStack) : 0;
}

int callUseItemTransaction(void* gameMode, void* itemStack)
{
    return g_useItemTransaction != nullptr ? g_useItemTransaction(gameMode, itemStack) : 0;
}

namespace {

int gameModeFaultFilter(unsigned long code)
{
    return (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR)
               ? EXCEPTION_EXECUTE_HANDLER
               : EXCEPTION_CONTINUE_SEARCH;
}

bool callSetGameModeGuarded(SetGameModeFn fn, void* self, int mode, int extra)
{
    __try {
        fn(self, mode, extra);
        return true;
    } __except (gameModeFaultFilter(GetExceptionCode())) {
        return false;
    }
}

bool callBuildBlockGuarded(BuildBlockFn fn, void* gameMode, void* blockPos, unsigned char face,
                           unsigned char extra, bool& out)
{
    __try {
        out = fn(gameMode, blockPos, face, extra);
        return true;
    } __except (gameModeFaultFilter(GetExceptionCode())) {
        return false;
    }
}

}

bool callSetGameMode(void* self, int mode, int extra)
{
    if (g_setGameMode == nullptr || self == nullptr) {
        return false;
    }

    if (callSetGameModeGuarded(g_setGameMode, self, mode, extra)) {
        return true;
    }
    log().error(L"SetGameMode faulted for target {:#x}, dropping it",
                reinterpret_cast<std::uintptr_t>(self));
    return false;
}

bool callBuildBlock(void* gameMode, void* blockPos, unsigned char face, unsigned char extra)
{
    if (g_buildBlock == nullptr || gameMode == nullptr) {
        return false;
    }

    bool placed = false;
    if (callBuildBlockGuarded(g_buildBlock, gameMode, blockPos, face, extra, placed)) {
        return placed;
    }

    log().error(L"buildBlock faulted for game mode {:#x}, dropping it",
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

void* callUiBagFind(void* bag, const char* key)
{
    if (g_uiBagFind == nullptr || bag == nullptr || key == nullptr) {
        return nullptr;
    }
    return g_uiBagFind(bag, key);
}

bool callKeyDisplayName(void* outString, int keyCode)
{
    if (g_keyDisplayName == nullptr || outString == nullptr) {
        return false;
    }
    __try {
        g_keyDisplayName(nullptr, outString, keyCode);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
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

void* callGameAllocate(size_t size)
{
    if (g_gameAllocate == nullptr || size == 0 || size > 0x10000) {
        return nullptr;
    }
    __try {
        return g_gameAllocate(nullptr, size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
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

bool hasGetDestroySpeed() { return g_getDestroySpeed != nullptr; }
bool hasSetSelectedSlot() { return g_setSelectedSlot != nullptr; }
bool hasBuildBlock() { return g_buildBlock != nullptr; }
bool hasUseItem() { return g_useItem != nullptr; }
bool hasUseItemTransaction() { return g_useItemTransaction != nullptr; }
bool hasSetGameMode() { return g_setGameMode != nullptr; }
bool hasNotifyInventoryOpen() { return g_notifyInventoryOpen != nullptr; }

}
