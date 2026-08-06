#include "hooks/Detours.h"

#include "core/Logger.h"
#include "game/CommandRequest.h"
#include "game/GameData.h"
#include "game/InventoryActionBridge.h"
#include "game/InventoryScreen.h"
#include "game/ItemStackRequest.h"
#include "hooks/HookManager.h"
#include "input/Capture.h"
#include "input/MouseCapture.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"
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

    FastBlockPlacement::instance().onPlayerViewUpdate();
    Scaffold::instance().onPlayerViewUpdate();

    GameModeSwitch::instance().onPlayerViewUpdate();

    ItemStackRequest::instance().onFrame();

    InventoryActionBridge::instance().onFrame();

    HandRestock::instance().onPlayerViewUpdate();

    OffhandSwap::instance().onPlayerViewUpdate();
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

GetDestroySpeedFn g_getDestroySpeed = nullptr;
SetSelectedSlotFn g_setSelectedSlot = nullptr;
BuildBlockFn g_buildBlock = nullptr;
AbilitiesAccessFn g_abilitiesAccess = nullptr;
UseItemFn g_useItem = nullptr;
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

float __fastcall detourGetDestroySpeed(void* rcx, void* rdx, void* r8, void* r9)
{
    return AutoTool::instance().onGetDestroySpeed(rcx, rdx, r8, r9);
}

void __fastcall detourSetSelectedSlot(void* rcx, void* rdx, void* r8, void* r9)
{

    HandRestock::instance().onSetSelectedSlot(rcx);
    OffhandSwap::instance().onSetSelectedSlot(rcx);

    InventoryActionBridge::instance().onSetSelectedSlot(rcx);

    if (input::consoleCapturing()) {
        return;
    }

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

    hooks.create(scanner.address(Target::AbilitiesAccess), &detourAbilitiesAccess,
                 reinterpret_cast<void**>(&g_abilitiesAccess), L"AbilitiesAccess");

    hooks.create(scanner.address(Target::UseItem), &detourUseItem,
                 reinterpret_cast<void**>(&g_useItem), L"useItem");

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

    hooks.create(scanner.address(Target::AddRequestAction), &detourAddRequestAction,
                 reinterpret_cast<void**>(&g_addRequestAction), L"AddRequestAction");

    hooks.create(ItemStackRequest::findContainerOpenHandle(), &detourContainerOpenHandle,
                 reinterpret_cast<void**>(&g_containerOpenHandle), L"ContainerOpenHandle");

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

    input::installMouseHooks();

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

bool callBuildBlock(void* gameMode, void* blockPos, unsigned char face, unsigned char extra)
{
    return g_buildBlock != nullptr ? g_buildBlock(gameMode, blockPos, face, extra) : false;
}

int callUseItem(void* gameMode, void* itemStack)
{
    return g_useItem != nullptr ? g_useItem(gameMode, itemStack) : 0;
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

void callNotifyInventoryOpen(void* client)
{

    if (g_notifyInventoryOpen != nullptr && client != nullptr) {
        g_notifyInventoryOpen(client, 0);
    }
}

bool hasGetDestroySpeed() { return g_getDestroySpeed != nullptr; }
bool hasSetSelectedSlot() { return g_setSelectedSlot != nullptr; }
bool hasBuildBlock() { return g_buildBlock != nullptr; }
bool hasUseItem() { return g_useItem != nullptr; }
bool hasSetGameMode() { return g_setGameMode != nullptr; }
bool hasNotifyInventoryOpen() { return g_notifyInventoryOpen != nullptr; }

}
