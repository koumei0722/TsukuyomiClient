#include "hooks/Detours.h"

#include "core/Logger.h"
#include "core/Paths.h"
#include "core/Perf.h"
#include "game/BlockRegistry.h"
#include "game/BlockWrite.h"
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
#include "modules/FastInventory.h"
#include "modules/FreeCamera.h"
#include "modules/Fullbright.h"
#include "modules/NoRender.h"
#include "modules/Zoom.h"
#include "modules/Scaffold.h"
#include "modules/Schematica.h"
#include "render/BoxRenderer.h"
#include "render/DiffAtlas.h"
#include "render/FrameTrace.h"
#include "render/PackTexture.h"
#include "render/GhostLayer.h"
#include "render/Overlay.h"
#include "render/WorldMesh.h"

#include <Windows.h>

#include <intrin.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <set>
#include <unordered_map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace tsukuyomi {

extern "C" {

void tsukuyomiCameraTrampolineEntry();
void* tsukuyomiCameraTrampoline = nullptr;

void tsukuyomiPlayerViewTrampolineEntry();
void* tsukuyomiPlayerViewTrampoline = nullptr;

void tsukuyomiPacketSendTrampolineEntry();
void* tsukuyomiPacketSendTrampoline = nullptr;

void tsukuyomiCameraHook(void* cameraBase, void* source)
{
    FreeCamera::instance().onCameraWrite(cameraBase);

    Zoom::instance().onCameraWrite(cameraBase, source);
    boxes::noteCamera(cameraBase);
}

using ViewVectorFn = void*(__fastcall*)(void* actor, float* out, float partial);
ViewVectorFn g_viewVector = nullptr;

void* __fastcall detourViewVector(void* actor, float* out, float partial)
{
    void* const result = (g_viewVector != nullptr) ? g_viewVector(actor, out, partial) : nullptr;
    if (!FreeCamera::instance().enabled() && !GameData::instance().hasLivePlayer()) {
        GameData::instance().adoptPlayerFromEntity(actor);
    }
    if (FreeCamera::instance().enabled() && GameData::instance().isPlayerEntity(actor)) {
        FreeCamera::instance().freezeViewVector(out);
    }
    return result;
}

void tsukuyomiPlayerViewHook(void* viewBase)
{

    blocks::setSimThread(GetCurrentThreadId());

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

    AutoTool::instance().onPlayerViewUpdate();

    FastBlockPlacement::instance().onPlayerViewUpdate();
    Scaffold::instance().onPlayerViewUpdate();

    GameModeSwitch::instance().onPlayerViewUpdate();

    UiSound::instance().pump();

    ItemStackRequest::instance().onFrame();

    InventoryActionBridge::instance().onFrame();

    HandRestock::instance().onPlayerViewUpdate();

    OffhandSwap::instance().onPlayerViewUpdate();

    Schematica::instance().onPlayerViewUpdate();

    uiprobe::pumpMenuSelection();

    uiprobe::pumpControlsKeybind();
}

void noteAuthInputPacket(void* packet)
{
    static std::atomic<int> logged{0};
    static std::atomic<bool> wanted{false};
    static std::atomic<bool> checked{false};
    if (!checked.exchange(true, std::memory_order_acq_rel)) {
        std::error_code ec;
        wanted.store(std::filesystem::exists(paths::dataDir() / L"diag-authinput.txt", ec),
                     std::memory_order_release);
    }
    if (!wanted.load(std::memory_order_acquire)) {
        return;
    }
    if (logged.load(std::memory_order_relaxed) >= 24) {
        return;
    }
    const bool pressing = (GetAsyncKeyState('W') & 0x8000) != 0;
    if (!pressing) {
        return;
    }
    constexpr std::size_t kSpan = 0x140;
    if (!memory::isReadable(packet, kSpan)) {
        return;
    }
    const auto* const words = reinterpret_cast<const std::uint32_t*>(packet);
    std::wstring line;
    for (std::size_t i = 2; i < kSpan / 4; ++i) {
        if (words[i] == 0) {
            continue;
        }
        float f = 0.0f;
        std::memcpy(&f, &words[i], sizeof(f));
        if (std::isfinite(f) && std::fabs(f) > 1.0e-6f && std::fabs(f) < 1.0e6f) {
            line += std::format(L" +{:03x}={:.3f}", i * 4, f);
        } else {
            line += std::format(L" +{:03x}={:08x}", i * 4, words[i]);
        }
    }
    logged.fetch_add(1, std::memory_order_relaxed);
}

bool zeroAuthInputMove(void* packet)
{
    constexpr std::size_t kMoveAt = 0x080;
    constexpr std::size_t kFlagsAt = 0x014;

    auto* const base = static_cast<std::byte*>(packet);
    if (memory::isWritable(base + kMoveAt, sizeof(float) * 2)) {
        auto* const at = reinterpret_cast<float*>(base + kMoveAt);
        at[0] = 0.0f;
        at[1] = 0.0f;
    }
    if (memory::isWritable(base + 0x088, sizeof(std::uint32_t))) {
        *reinterpret_cast<std::uint32_t*>(base + 0x088) = 0;
    }
    if (memory::isWritable(base + kFlagsAt, sizeof(std::uint32_t))) {
        *reinterpret_cast<std::uint32_t*>(base + kFlagsAt) = 0;
    }
    return true;
}

unsigned char tsukuyomiShouldBlockPacket(void* packet)
{
    if (FreeCamera::instance().enabled()
        && ItemStackRequest::packetId(packet) == 0x90) {
        noteAuthInputPacket(packet);
        FreeCamera::instance().armPacketTrace(static_cast<std::byte*>(packet) + 0x084);
        static std::atomic<int> markerChecked{0};
        static std::atomic<bool> marker{false};
        if (markerChecked.exchange(1, std::memory_order_acq_rel) == 0) {
            std::error_code ec;
            marker.store(std::filesystem::exists(paths::dataDir() / L"diag-marker.txt", ec),
                         std::memory_order_release);
        }
        if (!marker.load(std::memory_order_acquire)) {
            zeroAuthInputMove(packet);
        }
    }

    ItemStackRequest::instance().observePacket(packet);

    return InventoryActionBridge::instance().shouldBlockPacket(packet) ? 1u : 0u;
}

}

}

namespace tsukuyomi::hooks {

std::atomic<std::size_t> g_predAnswers{0};
std::atomic<std::size_t> g_visibilityScans{0};
std::atomic<std::size_t> g_getBlockGhost{0};
std::atomic<std::size_t> g_getBlockCalls{0};

void noteChunkBuiltForRebuilds(const void* rec, std::uint64_t buildSeq);
void noteChunkBoxTries(const void* rec, std::uint32_t tries, std::uint32_t stacked,
                       std::uint64_t buildSeq);

namespace {

using GetDestroySpeedFn = float(__fastcall*)(void*, void*, void*, void*);
using SetSelectedSlotFn = void(__fastcall*)(void*, void*, void*, void*);
using BuildBlockFn = bool(__fastcall*)(void*, void*, unsigned char, unsigned char, bool);
using AbilitiesAccessFn = bool(__fastcall*)(void*, void*, void*, void*);

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
using SettingsTabListFn = void*(__fastcall*)(void*, void*, void*, void*);
using SettingsAddTabFn = void*(__fastcall*)(void*, unsigned, const void*, const void*);

using SettingsInvokeActionFn = unsigned char(__fastcall*)(void*, void*);

using OpenHowToPlayScreenFn = void(__fastcall*)(void*);

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

using KeyBindingIsDefaultFn = unsigned char(__fastcall*)(void*, std::uintptr_t);

using SettingsActionDataFn = void*(__fastcall*)(void*, void*);

using SettingsActionQueryUpdateFn = void*(__fastcall*)(void*);

using OreKeyRowsWrapFn = void*(__fastcall*)(void*, void*);

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
UseItemTransactionFn g_useItemTransaction = nullptr;
SetGameModeFn g_setGameMode = nullptr;
NotifyInventoryOpenFn g_notifyInventoryOpen = nullptr;
MoveInputHandlerFn g_moveInputHandler = nullptr;
using InputGatherFn = void*(__fastcall*)(void*, void*, void*, void*);
InputGatherFn g_inputGather = nullptr;
using MoveApplyFn = void*(__fastcall*)(void*, void*, void*, void*);
MoveApplyFn g_moveApply = nullptr;
using MoveIntentFn = void*(__fastcall*)(void*, void*, void*, void*);
MoveIntentFn g_moveIntent = nullptr;
GetActorEffectFn g_getActorEffect = nullptr;
OpenInventoryScreenFn g_openInventoryScreen = nullptr;
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
SettingsTabListFn g_settingsTabList = nullptr;
SettingsAddTabFn g_settingsAddTab = nullptr;
SettingsInvokeActionFn g_settingsInvokeAction = nullptr;
OpenHowToPlayScreenFn g_openHowToPlayScreen = nullptr;

std::atomic<void*> g_clientInstance{nullptr};
SettingsProviderCallFn g_settingsProviderCall = nullptr;
SettingsGroupInfoUpdateFn g_settingsGroupInfoUpdate = nullptr;
SettingsFindComponentFn g_settingsFindComponent = nullptr;
GameAllocateFn g_gameAllocate = nullptr;
KeybindListBuildFn g_keybindListBuild = nullptr;
using FogSettingsFetchFn = void*(__fastcall*)(void*, void*, void*, void*);
FogSettingsFetchFn g_fogSettingsFetch = nullptr;
ControlsBindingNameFn g_controlsBindingName = nullptr;
ControlsRowBindingsFn g_controlsRowBindings = nullptr;
ControlsSectionSetupFn g_controlsSectionSetup = nullptr;
OreFacetBindFn g_oreFacetBind = nullptr;
OreKeyboardInputGroupFn g_oreKeyboardInputGroup = nullptr;
KeyActionNameFn g_keyActionName = nullptr;
KeyRowListBuildFn g_keyRowListBuild = nullptr;
OreKeyRowsBuildFn g_oreKeyRowsBuild = nullptr;
KeyBindingLookupFn g_keyBindingLookup = nullptr;
KeyBindingIsDefaultFn g_keyBindingIsDefault = nullptr;
SettingsActionDataFn g_settingsActionData = nullptr;
SettingsActionQueryUpdateFn g_settingsActionQueryUpdate = nullptr;
OreKeyRowsWrapFn g_oreKeyRowsWrap = nullptr;
RowDataCandFn g_rowDataCandA = nullptr;
RowDataCandFn g_rowDataCandB = nullptr;
OreKeyNameToIndexFn g_oreKeyNameToIndex = nullptr;
I18nGetFn g_i18nGet = nullptr;

constexpr bool kKeepRowsAfterConsume = true;

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
                                 unsigned char extra, bool simTick)
{
    GameData::instance().setGameMode(gameMode);

    static bool seenSimTick[2] = {false, false};
    const int slot = simTick ? 1 : 0;
    if (!seenSimTick[slot]) {
        seenSimTick[slot] = true;
    }

    return FastBlockPlacement::instance().onBuildBlock(gameMode, blockPos, face, extra, simTick);
}

bool __fastcall detourAbilitiesAccess(void* rcx, void* rdx, void* r8, void* r9)
{
    CreativeNoClip::instance().onAbilitiesAccess(rdx);
    FlySpeed::instance().onAbilitiesAccess(rdx);
    return g_abilitiesAccess != nullptr ? g_abilitiesAccess(rcx, rdx, r8, r9) : false;
}

int __fastcall detourUseItem(void* gameMode, void* itemStack, int extra)
{
    return FastRightClick::instance().onUseItem(gameMode, itemStack, extra);
}

int __fastcall detourUseItemTransaction(void* gameMode, void* itemStack, int extra)
{
    return FastRightClick::instance().onUseItemTransaction(gameMode, itemStack, extra);
}

using SubChunkSetBlockFn = void(__fastcall*)(void*, unsigned int, unsigned int, const void*);
SubChunkSetBlockFn g_subChunkSetBlock = nullptr;

bool cjFacesOn()
{
    static const bool on = [] {
        std::error_code ec;
        return std::filesystem::exists(paths::dataDir() / L"diag-cj-faces.txt", ec);
    }();
    return on;
}

bool chFreedOn()
{
    static const bool on = [] {
        std::error_code ec;
        return std::filesystem::exists(paths::dataDir() / L"diag-ch-freed.txt", ec);
    }();
    return on;
}

void __fastcall detourSubChunkSetBlock(void* self, unsigned int layer, unsigned int index,
                                       const void* block)
{
    armStorageFromSubChunk(self);

    const void* const write = blockwrite::onSubChunkWrite(self, layer, index, block);
    if (write == nullptr) {
        return;
    }

    if (layer == 0 && blocks::ghostOn()) {
        int at[3] = {0, 0, 0};
        if (blockwrite::worldPosOfSubChunkWrite(self, index, at)) {
            const bool mine = blockwrite::selfWriting();
            if (write == blocks::airBlock()) {
                const bool restored =
                    blocks::restoreGhostCellFromWant(at[0], at[1], at[2]);
                if (chFreedOn()) {
                    const bool inside = blocks::ghostInside(at[0], at[1], at[2]);
                    if (restored && !mine && inside) {
                        Schematica::instance().noteGhostCellFreed(at[0], at[1], at[2]);
                    }
                    static std::atomic<int> shots{0};
                }
            } else if (!mine) {
                blocks::dropGhostCell(at[0], at[1], at[2]);
                Schematica::instance().noteActorCellTaken(at[0], at[1], at[2]);
            }
            if (!mine) {
                const void* extra = nullptr;
                const bool extraKnown =
                    blockwrite::readWorldExtraAt(at[0], at[1], at[2], extra);
                blocks::noteWorldBlockAt(at[0], at[1], at[2], write, extra, extraKnown);
            }
        }
    }
    else if (layer == 1 && blocks::ghostOn() && !blockwrite::selfWriting()) {
        int at[3] = {0, 0, 0};
        if (blockwrite::worldPosOfSubChunkWrite(self, index, at)) {
            if (const void* const real = blockwrite::readWorldAt(at[0], at[1], at[2]);
                real != nullptr) {
                blocks::noteWorldBlockAt(at[0], at[1], at[2], real, write, true);
            }
        }
    }

    if (g_subChunkSetBlock != nullptr) {
        g_subChunkSetBlock(self, layer, index, write);
    }
}

using StoragePredFn = bool(__fastcall*)(void*, const void*);

constexpr std::size_t kStoragePredMax = 64;
void* g_storagePredTarget[kStoragePredMax] = {};
StoragePredFn g_storagePredOriginal[kStoragePredMax] = {};
std::atomic<std::size_t> g_storagePredCount{0};

constexpr std::size_t kGhostStorageSlots = 2048;

struct GhostStorageSlot {
    std::atomic<std::int32_t> baseX{0};
    std::atomic<std::int32_t> baseY{0};
    std::atomic<std::int32_t> baseZ{0};
    std::atomic<bool> used{false};
    std::atomic<void*> storage[2] = {};
};

GhostStorageSlot g_ghostSlots[kGhostStorageSlots];
std::atomic<std::size_t> g_ghostSlotCount{0};

bool isGhostStorage(const void* storage)
{
    if (storage == nullptr) {
        return false;
    }
    const std::size_t used = std::min(g_ghostSlotCount.load(std::memory_order_acquire),
                                      kGhostStorageSlots);
    for (std::size_t i = 0; i < used; ++i) {
        if (!g_ghostSlots[i].used.load(std::memory_order_acquire)) {
            continue;
        }
        if (g_ghostSlots[i].storage[0].load(std::memory_order_relaxed) == storage
            || g_ghostSlots[i].storage[1].load(std::memory_order_relaxed) == storage) {
            return true;
        }
    }
    return false;
}

std::size_t findGhostSlot(std::int32_t baseX, std::int32_t baseY, std::int32_t baseZ)
{
    const std::size_t used = std::min(g_ghostSlotCount.load(std::memory_order_acquire),
                                      kGhostStorageSlots);
    for (std::size_t i = 0; i < used; ++i) {
        if (g_ghostSlots[i].baseX.load(std::memory_order_relaxed) == baseX
            && g_ghostSlots[i].baseY.load(std::memory_order_relaxed) == baseY
            && g_ghostSlots[i].baseZ.load(std::memory_order_relaxed) == baseZ) {
            return i;
        }
    }
    return kGhostStorageSlots;
}

void noteGhostStorage(std::int32_t baseX, std::int32_t baseY, std::int32_t baseZ, int layer,
                      void* storage)
{
    std::size_t at = findGhostSlot(baseX, baseY, baseZ);
    if (at >= kGhostStorageSlots) {
        const std::size_t used = g_ghostSlotCount.load(std::memory_order_acquire);
        if (used >= kGhostStorageSlots) {
            static std::atomic<bool> told{false};
            if (!told.exchange(true, std::memory_order_relaxed)) {
                log().warn(L"Schematica: ran out of slots for remembered storage ({} chunks); "
                           L"chunks past this will not show the ghost",
                           kGhostStorageSlots);
            }
            return;
        }
        at = used;
        g_ghostSlots[at].baseX.store(baseX, std::memory_order_relaxed);
        g_ghostSlots[at].baseY.store(baseY, std::memory_order_relaxed);
        g_ghostSlots[at].baseZ.store(baseZ, std::memory_order_relaxed);
        g_ghostSlots[at].storage[0].store(nullptr, std::memory_order_relaxed);
        g_ghostSlots[at].storage[1].store(nullptr, std::memory_order_relaxed);
        g_ghostSlots[at].used.store(true, std::memory_order_release);
        g_ghostSlotCount.store(at + 1, std::memory_order_release);
    }
    g_ghostSlots[at].storage[layer & 1].store(storage, std::memory_order_release);
}

void noteGhostStorageSeen(std::int32_t baseX, std::int32_t baseY, std::int32_t baseZ,
                          void* storage)
{
    if (storage == nullptr) {
        return;
    }
    const std::size_t at = findGhostSlot(baseX, baseY, baseZ);
    if (at < kGhostStorageSlots) {
        for (int layer = 0; layer < 2; ++layer) {
            void* const held = g_ghostSlots[at].storage[layer].load(std::memory_order_relaxed);
            if (held == storage) {
                return;
            }
        }
        for (int layer = 0; layer < 2; ++layer) {
            if (g_ghostSlots[at].storage[layer].load(std::memory_order_relaxed) == nullptr) {
                g_ghostSlots[at].storage[layer].store(storage, std::memory_order_release);
                return;
            }
        }
        g_ghostSlots[at].storage[0].store(g_ghostSlots[at].storage[1].load(
                                              std::memory_order_relaxed),
                                          std::memory_order_relaxed);
        g_ghostSlots[at].storage[1].store(storage, std::memory_order_release);
        return;
    }
    noteGhostStorage(baseX, baseY, baseZ, 0, storage);
}

constexpr std::size_t kStorageVtableMax = 32;
std::atomic<void*> g_storageVtable[kStorageVtableMax] = {};
std::atomic<std::size_t> g_storageVtableCount{0};

bool seenStorageVtable(const void* vtable)
{
    const std::size_t used = std::min(g_storageVtableCount.load(std::memory_order_acquire),
                                      kStorageVtableMax);
    for (std::size_t i = 0; i < used; ++i) {
        if (g_storageVtable[i].load(std::memory_order_relaxed) == vtable) {
            return true;
        }
    }
    return false;
}

void noteStorageVtable(void* vtable)
{
    const std::size_t used = g_storageVtableCount.load(std::memory_order_acquire);
    if (used >= kStorageVtableMax || seenStorageVtable(vtable)) {
        return;
    }
    g_storageVtable[used].store(vtable, std::memory_order_relaxed);
    g_storageVtableCount.store(used + 1, std::memory_order_release);
}

constexpr std::size_t kPendingVtableMax = 32;
std::atomic<void*> g_pendingVtable[kPendingVtableMax] = {};
std::atomic<std::size_t> g_pendingWrite{0};
std::atomic<std::size_t> g_pendingRead{0};

void notePendingStorageVtable(void* vtable)
{
    const std::size_t at = g_pendingWrite.fetch_add(1, std::memory_order_acq_rel);
    if (at >= kPendingVtableMax) {
        return;
    }
    g_pendingVtable[at].store(vtable, std::memory_order_release);
}

bool takePendingStorageVtable(void** out)
{
    const std::size_t write = std::min(g_pendingWrite.load(std::memory_order_acquire),
                                       kPendingVtableMax);
    std::size_t read = g_pendingRead.load(std::memory_order_relaxed);
    while (read < write) {
        void* const one = g_pendingVtable[read].load(std::memory_order_acquire);
        g_pendingRead.store(read + 1, std::memory_order_relaxed);
        if (one != nullptr) {
            *out = one;
            return true;
        }
        read = g_pendingRead.load(std::memory_order_relaxed);
    }
    return false;
}

__declspec(noinline) bool readStorageVtable(const void* storage, void** out)
{
    __try {
        std::memcpy(out, storage, sizeof(*out));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *out = nullptr;
        return false;
    }
}

__declspec(noinline) bool readStoragePointers(const void* subChunk, void* out[2])
{
    __try {
        const auto* const at = reinterpret_cast<void* const*>(
            static_cast<const std::uint8_t*>(subChunk) + 0x20);
        out[0] = at[0];
        out[1] = at[1];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = nullptr;
        out[1] = nullptr;
        return false;
    }
}

using ChunkVisibilityScanFn = void(__fastcall*)(void*, void*);
ChunkVisibilityScanFn g_chunkVisibilityScan = nullptr;

__declspec(noinline) bool readSharedOrigin(const void* chunk, std::int32_t out[3])
{
    __try {
        const auto* const at = static_cast<const std::uint8_t*>(chunk);
        std::int32_t x = 0;
        std::int16_t y = 0;
        std::int32_t z = 0;
        std::memcpy(&x, at + 0x14, sizeof(x));
        std::memcpy(&y, at + 0x18, sizeof(y));
        std::memcpy(&z, at + 0x1c, sizeof(z));
        out[0] = x;
        out[1] = y;
        out[2] = z;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

__declspec(noinline) void clearSeeThroughFlag(void* shared)
{
    __try {
        auto* const at = static_cast<volatile std::uint8_t*>(shared);
        at[2] = 0;
        at[0] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void __fastcall detourChunkVisibilityScan(void* shared, void* region)
{
    g_visibilityScans.fetch_add(1, std::memory_order_relaxed);
    if (g_chunkVisibilityScan != nullptr) {
        g_chunkVisibilityScan(shared, region);
    }
    if (shared == nullptr || !blocks::ghostOn()) {
        return;
    }
    std::int32_t origin[3] = {0, 0, 0};
    if (!readSharedOrigin(shared, origin)) {
        return;
    }
    if (!blocks::ghostBoxTouchesSubChunk(origin[0] >> 4 << 4, origin[1] >> 4 << 4,
                                         origin[2] >> 4 << 4)) {
        return;
    }
    blocks::noteGhostHit(blocks::GhostHook::Filled);
    clearSeeThroughFlag(shared);
}

thread_local std::int32_t t_gateOrigin[3] = {0, 0, 0};
thread_local bool t_gateValid = false;

using VisibilityGateFn = void(__fastcall*)(void*, void*);
VisibilityGateFn g_visibilityGate = nullptr;

__declspec(noinline) bool readGateOrigin(const void* task, std::int32_t out[3])
{
    __try {
        const auto* const shared = *reinterpret_cast<const std::uint8_t* const*>(task);
        if (shared == nullptr) {
            return false;
        }
        std::int32_t x = 0;
        std::int16_t y = 0;
        std::int32_t z = 0;
        std::memcpy(&x, shared + 0x14, sizeof(x));
        std::memcpy(&y, shared + 0x18, sizeof(y));
        std::memcpy(&z, shared + 0x1c, sizeof(z));
        out[0] = x >> 4 << 4;
        out[1] = static_cast<std::int32_t>(y) >> 4 << 4;
        out[2] = z >> 4 << 4;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::atomic<void*> g_visCoordinator{nullptr};

void __fastcall detourVisibilityGate(void* self, void* task)
{
    if (self != nullptr) {
        g_visCoordinator.store(self, std::memory_order_relaxed);
    }
    const bool had = t_gateValid;
    std::int32_t saved[3] = {t_gateOrigin[0], t_gateOrigin[1], t_gateOrigin[2]};
    t_gateValid = task != nullptr && readGateOrigin(task, t_gateOrigin);
    if (g_visibilityGate != nullptr) {
        g_visibilityGate(self, task);
    }
    t_gateValid = had;
    t_gateOrigin[0] = saved[0];
    t_gateOrigin[1] = saved[1];
    t_gateOrigin[2] = saved[2];
}

bool storagePredAnswer(std::size_t slot, void* self, const void* block)
{
    g_predAnswers.fetch_add(1, std::memory_order_relaxed);
    if (blocks::ghostOn()) {
        if (t_gateValid) {
            if (blocks::ghostBoxTouchesSubChunk(t_gateOrigin[0], t_gateOrigin[1],
                                                t_gateOrigin[2])) {
                noteGhostStorageSeen(t_gateOrigin[0], t_gateOrigin[1],
                                     t_gateOrigin[2], self);
                blocks::noteGhostHit(blocks::GhostHook::Filled);
                return false;
            }
        } else if (isGhostStorage(self)) {
            blocks::noteGhostHit(blocks::GhostHook::Filled);
            return false;
        }
    }
    const StoragePredFn original = g_storagePredOriginal[slot];
    return original != nullptr ? original(self, block) : false;
}

template <std::size_t Slot>
bool __fastcall detourStoragePred(void* self, const void* block)
{
    return storagePredAnswer(Slot, self, block);
}

template <std::size_t... Slots>
constexpr std::array<void*, sizeof...(Slots)> makeStoragePredTable(
    std::index_sequence<Slots...>)
{
    return {reinterpret_cast<void*>(&detourStoragePred<Slots>)...};
}

const std::array<void*, kStoragePredMax> kStoragePredDetours =
    makeStoragePredTable(std::make_index_sequence<kStoragePredMax>{});

void armStoragePred(void* target)
{
    if (target == nullptr) {
        return;
    }
    const std::size_t used = std::min(g_storagePredCount.load(std::memory_order_acquire),
                                      kStoragePredMax);
    for (std::size_t i = 0; i < used; ++i) {
        if (g_storagePredTarget[i] == target) {
            return;
        }
    }
    if (used >= kStoragePredMax) {
        static std::atomic<bool> told{false};
        if (!told.exchange(true, std::memory_order_relaxed)) {
            log().warn(L"Schematica: ran out of slots for the storage emptiness predicate "
                       L"({}); wider chunks will not show the ghost",
                       kStoragePredMax);
        }
        return;
    }
    g_storagePredTarget[used] = target;
    HookManager& hooks = HookManager::instance();
    if (!hooks.create(target, kStoragePredDetours[used],
                      reinterpret_cast<void**>(&g_storagePredOriginal[used]),
                      L"SubChunkStoragePredicate")) {
        g_storagePredTarget[used] = nullptr;
        return;
    }
    hooks.applyQueued();
    g_storagePredCount.store(used + 1, std::memory_order_release);
}

using BlockSourceGetBlockFn = const void*(__fastcall*)(void*, const void*);
BlockSourceGetBlockFn g_blockSourceGetBlock = nullptr;

int overlayGrowUnits()
{
    static int value = -1;
    if (value < 0) {
        value = 8;
        std::error_code ec;
        const std::filesystem::path flag = paths::dataDir() / L"diag-overlay.txt";
        if (std::filesystem::exists(flag, ec)) {
            std::ifstream in(flag);
            int got = 0;
            if (in >> got && got >= 0 && got <= 512) {
                value = got;
            }
        }
    }
    return value;
}

bool ghostDrawDiagOn()
{
    static int mode = 0;
    if (mode == 0) {
        std::error_code ec;
        mode = std::filesystem::exists(paths::dataDir() / L"diag-ghostdraw.txt", ec) ? 1 : 2;
    }
    return mode == 1;
}

bool boxDiagOn()
{
    static int mode = 0;
    if (mode == 0) {
        std::error_code ec;
        mode = std::filesystem::exists(paths::dataDir() / L"diag-box.txt", ec) ? 1 : 2;
    }
    return mode == 1;
}

int beCubeMode()
{
    if (!beModelsOn()) {
        return 0;
    }
    static std::atomic<int> mode{0};
    static std::atomic<ULONGLONG> readAt{0};
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG last = readAt.load(std::memory_order_acquire);
    if (last != 0 && now - last < 500) {
        return mode.load(std::memory_order_relaxed);
    }
    readAt.store(now, std::memory_order_release);
    int want = 5;
    std::error_code ec;
    const std::filesystem::path file = paths::dataDir() / L"diag-becube.txt";
    if (std::filesystem::exists(file, ec)) {
        std::ifstream in{file};
        if (!in || !(in >> want) || want < 0 || want > 6) {
            want = 5;
        }
    }
    mode.store(want, std::memory_order_release);
    return want;
}

thread_local int t_boxDepth = 0;

bool inBoxTessellate()
{
    return t_boxDepth > 0;
}

std::atomic<std::size_t> g_boxAskGetBlock{0};
std::atomic<std::size_t> g_boxAskGetExtra{0};
std::atomic<std::size_t> g_boxAskLookup{0};

std::atomic<std::size_t> g_boxVerts{0};

std::atomic<std::size_t> g_overlayStacked{0};
std::atomic<std::size_t> g_overlayMissed{0};

std::atomic<std::size_t> g_beCubeCells{0};
std::atomic<std::size_t> g_beSkipped{0};

struct ModelVertex {
    float pos[3] = {};
    float uv[2] = {};
};

struct ModelBox {
    float lo[3] = {};
    float hi[3] = {};
    std::vector<ModelVertex> verts;
};

std::mutex g_modelMutex;
std::unordered_map<const void*, std::vector<ModelBox>> g_modelBoxes;
std::unordered_map<const void*, bool> g_modelDone;
std::atomic<unsigned long long> g_modelLearnedAt{0};
thread_local const void* t_learning = nullptr;

bool modelLearned(const void* legacy);
std::vector<ModelBox> modelBoxesOf(const void* legacy);
bool hasModelBoxes(const void* legacy);
std::string modelTextureOf(const void* key);
std::atomic<std::size_t> g_beCubeVerts{0};
std::atomic<std::size_t> g_uvPainted{0};
std::atomic<std::size_t> g_uvMissed{0};
std::atomic<std::size_t> g_beCubeMiss{0};
std::atomic<std::size_t> g_beCubeRealDrew{0};
std::atomic<std::size_t> g_beCubeBothDrew{0};
std::atomic<std::size_t> g_beCubeNoBox{0};

using BlockSourceGetExtraFn = const void*(__fastcall*)(void*, const void*);
BlockSourceGetExtraFn g_blockSourceGetExtra = nullptr;

bool drawingSide(unsigned long tid)
{
    return blocks::isMeshThread(tid);
}

const void* __fastcall detourBlockSourceGetExtra(void* region, const void* pos)
{
    if (inBoxTessellate()) {
        g_boxAskGetExtra.fetch_add(1, std::memory_order_relaxed);
    }
    if (pos != nullptr && blocks::ghostOn() && drawingSide(GetCurrentThreadId())) {
        const auto* const p = static_cast<const std::int32_t*>(pos);
        if (const void* const ghost = blocks::ghostBlockAt(p[0], p[1], p[2], 1);
            ghost != nullptr) {
            return ghost;
        }
    }
    return g_blockSourceGetExtra != nullptr ? g_blockSourceGetExtra(region, pos) : nullptr;
}

thread_local int t_beRenderDepth = 0;

bool inBeRenderLoop()
{
    return t_beRenderDepth > 0;
}

bool fromCollisionQuery(void* returnAddress)
{
    struct Range {
        std::uintptr_t begin;
        std::size_t size;
    };
    static const Range range = [] {
        const Scanner& scanner = Scanner::instance();
        if (!scanner.found(Target::BlockCollisionQuery)) {
            return Range{0, 0};
        }
        void* const at = scanner.addressAs<void*>(Target::BlockCollisionQuery);
        const std::size_t size = memory::functionSize(at);
        if (size == 0) {
            log().warn(L"Schematica: could not get the length of the collision function "
                       L"(falling back to not lying)");
        }
        return Range{reinterpret_cast<std::uintptr_t>(at), size};
    }();
    if (range.begin == 0 || range.size == 0) {
        return false;
    }
    const auto site = reinterpret_cast<std::uintptr_t>(returnAddress);
    return site >= range.begin && site < range.begin + range.size;
}

constexpr std::size_t kHitSize = 0x85;
constexpr std::size_t kHitBlockPos = 0x20;
constexpr std::size_t kHitRefBegin = 0x38;
constexpr std::size_t kHitRefEnd = 0x48;

using HitAssignFn = void*(__fastcall*)(void*, const void*);
HitAssignFn g_hitAssign = nullptr;

std::atomic<bool> g_hitBlankReady{false};
unsigned char g_hitBlank[kHitSize] = {};

void* __fastcall detourHitAssign(void* dst, const void* src)
{
    void* const out = g_hitAssign != nullptr ? g_hitAssign(dst, src) : dst;
    if (dst == nullptr) {
        return out;
    }
    auto* const p = static_cast<unsigned char*>(dst);
    std::int32_t cell[3] = {0, 0, 0};
    std::memcpy(cell, p + kHitBlockPos, sizeof(cell));

    if (cell[0] == 0 && cell[1] == 0 && cell[2] == 0) {
        if (!g_hitBlankReady.load(std::memory_order_relaxed)) {
            std::memcpy(g_hitBlank, p, kHitSize);
            g_hitBlankReady.store(true, std::memory_order_release);
        }
        return out;
    }
    if (!blocks::ghostOn() || !g_hitBlankReady.load(std::memory_order_acquire)
        || !blocks::ghostCell(cell[0], cell[1], cell[2])) {
        return out;
    }
    std::memcpy(p + 0x18, g_hitBlank + 0x18, kHitRefBegin - 0x18);
    std::memcpy(p + kHitRefEnd, g_hitBlank + kHitRefEnd, kHitSize - kHitRefEnd);
    blocks::noteGhostHit(blocks::GhostHook::Aim);
    return out;
}

const void* __fastcall detourBlockSourceGetBlock(void* region, const void* pos)
{
    g_getBlockCalls.fetch_add(1, std::memory_order_relaxed);
    if (inBoxTessellate()) {
        g_boxAskGetBlock.fetch_add(1, std::memory_order_relaxed);
    }
    if (pos != nullptr && blocks::ghostOn()) {
        const auto* const p = static_cast<const std::int32_t*>(pos);
        if (const void* const ghost = blocks::ghostBlockAt(p[0], p[1], p[2]);
            ghost != nullptr) {
            if (drawingSide(GetCurrentThreadId())) {
                blocks::noteGhostHit(blocks::GhostHook::Layer);
                return ghost;
            }
            if (inBeRenderLoop() && blocks::hasBlockEntityFast(ghost)) {
                blocks::noteGhostHit(blocks::GhostHook::Actor);
                return ghost;
            }
        }
    }

    if (pos != nullptr && blocks::ghostOn()) {
        const auto* const p = static_cast<const std::int32_t*>(pos);
        if (blocks::ghostCell(p[0], p[1], p[2])
            && fromCollisionQuery(_ReturnAddress())) {
            const unsigned long sim = blocks::simThread();
            if (sim != 0 && GetCurrentThreadId() == sim) {
                if (const void* const air = blocks::airBlock(); air != nullptr) {
                    blocks::noteGhostHit(blocks::GhostHook::Phantom);
                    return air;
                }
            }
        }
    }
    return g_blockSourceGetBlock != nullptr ? g_blockSourceGetBlock(region, pos) : nullptr;
}

using BlockSourceSetBlockFn = bool(__fastcall*)(void*, const void*, const void*, unsigned int,
                                                unsigned int, void*);
BlockSourceSetBlockFn g_blockSourceSetBlock = nullptr;

bool __fastcall detourBlockSourceSetBlock(void* region, const void* pos, const void* block,
                                          unsigned int mode, unsigned int updateFlags,
                                          void* actor)
{
    if (!blockwrite::renderWriting()) {
        blockwrite::noteRegion(region, mode, updateFlags, actor);
    }

    if (!blockwrite::renderWriting()) {
        blockwrite::noteWritePos(static_cast<const int*>(pos));
    }

    if (pos != nullptr && !blockwrite::selfWriting()) {
        const auto* const p = static_cast<const std::int32_t*>(pos);
        if (blocks::ghostInside(p[0], p[1], p[2])) {
            const void* const air = blocks::airBlock();
            if (air != nullptr && block == air) {
                blocks::restoreGhostCellFromWant(p[0], p[1], p[2]);
                Schematica::instance().noteGhostCellFreed(p[0], p[1], p[2]);
            } else {
                blocks::dropGhostCell(p[0], p[1], p[2]);
                Schematica::instance().noteActorCellTaken(p[0], p[1], p[2]);
            }
        }
    }

    if (g_blockSourceSetBlock != nullptr) {
        return g_blockSourceSetBlock(region, pos, block, mode, updateFlags, actor);
    }
    return false;
}

using BlockRenderLookupFn = void*(__fastcall*)(void*, void*, void*);
BlockRenderLookupFn g_blockRenderLookup = nullptr;

using BlockDrawFn = void(__fastcall*)(void*, void*, void*, void*, void*);
BlockDrawFn g_blockTessellate = nullptr;

using BlockFacesFn = void(__fastcall*)(void*, void*, void*, void*, std::uint32_t, void*);
BlockFacesFn g_blockCubeFaces = nullptr;

BlockDrawFn g_blockTessellateCube = nullptr;

static std::atomic<std::size_t> g_boxPushDiff{0};
static std::atomic<std::size_t> g_boxBucketOk{0};
static std::atomic<std::size_t> g_boxBucketSkip{0};
static std::atomic<std::uint32_t> g_boxBucketLastLayer{0xffffffffU};

static std::atomic<std::int32_t> g_boxMinX{INT32_MAX};
static std::atomic<std::int32_t> g_boxMaxX{INT32_MIN};
static std::atomic<std::int32_t> g_boxMinZ{INT32_MAX};
static std::atomic<std::int32_t> g_boxMaxZ{INT32_MIN};

static void atomicMin(std::atomic<std::int32_t>& at, std::int32_t v)
{
    std::int32_t cur = at.load(std::memory_order_relaxed);
    while (v < cur && !at.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
    }
}

static void atomicMax(std::atomic<std::int32_t>& at, std::int32_t v)
{
    std::int32_t cur = at.load(std::memory_order_relaxed);
    while (v > cur && !at.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
    }
}

thread_local std::uint32_t t_buildBoxTries = 0;
thread_local std::uint32_t t_buildBoxStacked = 0;

static void noteBoxCoverage(std::int32_t x, std::int32_t z)
{
    atomicMin(g_boxMinX, x);
    atomicMax(g_boxMaxX, x);
    atomicMin(g_boxMinZ, z);
    atomicMax(g_boxMaxZ, z);
}

inline std::uintptr_t realLayerByte(void* real)
{
    return reinterpret_cast<std::uintptr_t>(real) & 0xFF;
}

void* __fastcall detourBlockRenderLookup(void* block, void* context, void* pos)
{
    if (inBoxTessellate()) {
        g_boxAskLookup.fetch_add(1, std::memory_order_relaxed);
    }
    blocks::noteMeshThread(GetCurrentThreadId());

    void* const real =
        g_blockRenderLookup != nullptr ? g_blockRenderLookup(block, context, pos) : nullptr;

    if (pos != nullptr && blocks::ghostOn()) {
        const auto* const p = static_cast<const std::int32_t*>(pos);
        const void* const ghostHere = blocks::ghostBlockAt(p[0], p[1], p[2]);
        const bool pushForBox = blocks::meshBoxesOn() && blocks::boxBlock() != nullptr;
        const bool overlayOn = blocks::ghostOverMismatchOn();
        const blocks::DiffColor pushColor = (ghostHere == nullptr && (pushForBox || overlayOn))
                                                ? blocks::diffCellAt(p[0], p[1], p[2])
                                                : blocks::DiffColor::None;
        const bool pushForOverlay =
            overlayOn
            && (pushColor == blocks::DiffColor::Wrong || pushColor == blocks::DiffColor::State)
            && blocks::wantBlockAt(p[0], p[1], p[2]) != nullptr;
        if (pushColor != blocks::DiffColor::None && (pushForBox || pushForOverlay)) {
            if (const auto value = realLayerByte(real); value < 32) {
                blocks::noteRealLayer(block, static_cast<int>(value));
            }
            g_boxPushDiff.fetch_add(1, std::memory_order_relaxed);
            if (boxDiagOn()) {
                static std::atomic<int> shots{0};
            }
            return reinterpret_cast<void*>(static_cast<std::uintptr_t>(blocks::kGhostLayer));
        }
        if (ghostHere != nullptr) {
            blocks::noteGhostHit(blocks::GhostHook::Layer);
            if (const auto value = realLayerByte(real); value < 32) {
                blocks::noteRealLayer(block, static_cast<int>(value));
            }
            return reinterpret_cast<void*>(
                static_cast<std::uintptr_t>(blocks::kGhostLayer));
        }
    }
    return real;
}

bool boxUvTile(int& col, int& row, int& cols, int& rows)
{
    if (atlas::tile(col, row, cols, rows)) {
        return true;
    }
    cols = 64;
    rows = 64;
    static int mode = 0;
    static int wantCol = -1;
    static int wantRow = -1;
    if (mode == 0) {
        mode = 2;
        std::error_code ec;
        const auto path = paths::dataDir() / L"diag-uv.txt";
        if (std::filesystem::exists(path, ec)) {
            std::ifstream file(path);
            int c = -1;
            int r = -1;
            if (file >> c >> r && c >= -1 && c < 64 && r >= -1 && r < 64) {
                wantCol = c;
                wantRow = r;
                mode = 1;
            }
        }
    }
    if (mode != 1) {
        return false;
    }
    col = wantCol;
    row = wantRow;
    return true;
}

constexpr std::ptrdiff_t kVtxScanSanity = 1 << 20;

std::ptrdiff_t tripleLength(const void* a, std::size_t at)
{
    const auto* const base = static_cast<const unsigned char*>(a);
    unsigned char* begin = nullptr;
    unsigned char* end = nullptr;
    unsigned char* cap = nullptr;
    std::memcpy(&begin, base + at, sizeof(begin));
    std::memcpy(&end, base + at + 8, sizeof(end));
    std::memcpy(&cap, base + at + 16, sizeof(cap));
    if (begin == nullptr) {
        return (end == nullptr && cap == nullptr) ? 0 : -1;
    }
    if (end < begin || cap < end) {
        return -1;
    }
    const std::ptrdiff_t len = end - begin;
    return len <= kVtxScanSanity ? len : -1;
}

constexpr std::size_t kVertexPosBegin = 0x10;
constexpr std::size_t kVertexUvBegin = 0xA0;
constexpr std::size_t kVertexUv2Begin = 0xB8;

bool tessellatorLayerWritable(const void* self)
{
    constexpr std::size_t kSlots = 64;
    struct Slot {
        std::atomic<const void*> key{nullptr};
        std::atomic<bool> ok{false};
    };
    static Slot slots[kSlots];
    const std::size_t at = (reinterpret_cast<std::uintptr_t>(self) >> 4) % kSlots;
    if (slots[at].key.load(std::memory_order_acquire) == self) {
        return slots[at].ok.load(std::memory_order_relaxed);
    }
    const bool ok = memory::isWritable(self, 0x84);
    slots[at].ok.store(ok, std::memory_order_relaxed);
    slots[at].key.store(self, std::memory_order_release);
    return ok;
}

constexpr std::size_t kTessellatorShape = 0x5f8;
constexpr std::size_t kTessellatorShapeFlag = 0x610;

bool tessellatorShapeWritable(const void* self)
{
    constexpr std::size_t kSlots = 64;
    struct Slot {
        std::atomic<const void*> key{nullptr};
        std::atomic<bool> ok{false};
    };
    static Slot slots[kSlots];
    const std::size_t at = (reinterpret_cast<std::uintptr_t>(self) >> 4) % kSlots;
    if (slots[at].key.load(std::memory_order_acquire) == self) {
        return slots[at].ok.load(std::memory_order_relaxed);
    }
    const bool ok = memory::isWritable(self, kTessellatorShapeFlag + 1);
    slots[at].ok.store(ok, std::memory_order_relaxed);
    slots[at].key.store(self, std::memory_order_release);
    return ok;
}

struct SavedShape {
    float box[6] = {};
    unsigned char flag = 0;
    bool held = false;
};

bool writeShape(void* self, const ModelBox& box, SavedShape& saved)
{
    if (self == nullptr || !tessellatorShapeWritable(self)) {
        return false;
    }
    auto* const base = static_cast<unsigned char*>(self);
    std::memcpy(saved.box, base + kTessellatorShape, sizeof(saved.box));
    std::memcpy(&saved.flag, base + kTessellatorShapeFlag, 1);
    saved.held = true;
    const float want[6] = {box.lo[0], box.lo[1], box.lo[2],
                           box.hi[0], box.hi[1], box.hi[2]};
    std::memcpy(base + kTessellatorShape, want, sizeof(want));
    const unsigned char one = 1;
    std::memcpy(base + kTessellatorShapeFlag, &one, 1);
    return true;
}

void restoreShape(void* self, const SavedShape& saved)
{
    if (self == nullptr || !saved.held) {
        return;
    }
    auto* const base = static_cast<unsigned char*>(self);
    std::memcpy(base + kTessellatorShape, saved.box, sizeof(saved.box));
    std::memcpy(base + kTessellatorShapeFlag, &saved.flag, 1);
}

bool readShape(const void* self, float out[6])
{
    if (self == nullptr || !tessellatorShapeWritable(self)) {
        return false;
    }
    std::memcpy(out, static_cast<const unsigned char*>(self) + kTessellatorShape,
                sizeof(float) * 6);
    return true;
}

ModelBox turnBox(const ModelBox& box, int quarters)
{
    ModelBox out = box;
    const int turns = ((quarters % 4) + 4) % 4;
    for (int i = 0; i < turns; ++i) {
        const ModelBox in = out;
        const float x0 = 0.5F - (in.hi[2] - 0.5F);
        const float x1 = 0.5F - (in.lo[2] - 0.5F);
        const float z0 = 0.5F + (in.lo[0] - 0.5F);
        const float z1 = 0.5F + (in.hi[0] - 0.5F);
        out.lo[0] = std::min(x0, x1);
        out.hi[0] = std::max(x0, x1);
        out.lo[2] = std::min(z0, z1);
        out.hi[2] = std::max(z0, z1);
    }
    return out;
}

bool vertsBounds(void* a, std::ptrdiff_t from, std::ptrdiff_t to, float lo[3], float hi[3])
{
    if (a == nullptr || to <= from || to - from > (1 << 20)) {
        return false;
    }
    unsigned char* begin = nullptr;
    std::memcpy(&begin, static_cast<unsigned char*>(a) + kVertexPosBegin, sizeof(begin));
    if (begin == nullptr) {
        return false;
    }
    for (int k = 0; k < 3; ++k) {
        lo[k] = 1e9F;
        hi[k] = -1e9F;
    }
    for (unsigned char* q = begin + from; q + 12 <= begin + to; q += 12) {
        float v[3] = {};
        std::memcpy(v, q, sizeof(v));
        for (int k = 0; k < 3; ++k) {
            if (!std::isfinite(v[k])) {
                return false;
            }
            lo[k] = std::min(lo[k], v[k]);
            hi[k] = std::max(hi[k], v[k]);
        }
    }
    return hi[0] >= lo[0];
}

constexpr int kUnusedFaceTable = 0;

void turnPoint(float p[3], int quarters)
{
    const int turns = ((quarters % 4) + 4) % 4;
    for (int i = 0; i < turns; ++i) {
        const float x = 0.5F - (p[2] - 0.5F);
        const float z = 0.5F + (p[0] - 0.5F);
        p[0] = x;
        p[2] = z;
    }
}

void pitchPoint(float p[3], int quarters)
{
    const int turns = ((quarters % 4) + 4) % 4;
    for (int i = 0; i < turns; ++i) {
        const float y = 0.5F - (p[2] - 0.5F);
        const float z = 0.5F + (p[1] - 0.5F);
        p[1] = y;
        p[2] = z;
    }
}

ModelBox pitchBox(const ModelBox& box, int quarters)
{
    ModelBox out = box;
    const int turns = ((quarters % 4) + 4) % 4;
    for (int i = 0; i < turns; ++i) {
        const ModelBox in = out;
        const float y0 = 0.5F - (in.hi[2] - 0.5F);
        const float y1 = 0.5F - (in.lo[2] - 0.5F);
        const float z0 = 0.5F + (in.lo[1] - 0.5F);
        const float z1 = 0.5F + (in.hi[1] - 0.5F);
        out.lo[1] = std::min(y0, y1);
        out.hi[1] = std::max(y0, y1);
        out.lo[2] = std::min(z0, z1);
        out.hi[2] = std::max(z0, z1);
    }
    return out;
}

bool paintFaceUv(void* a, std::ptrdiff_t from, std::ptrdiff_t to, const ModelBox& box,
                 int quarters, int pitchQuarters, int col, int row, int tw, int th, int cols,
                 int rows)
{
    if (a == nullptr || to <= from || box.verts.size() < 24 || cols <= 0 || rows <= 0
        || tw <= 0 || th <= 0) {
        return false;
    }
    const std::size_t count = static_cast<std::size_t>(to - from) / 12;
    if (count == 0 || count > 8) {
        return false;
    }
    const auto* const base = static_cast<const unsigned char*>(a);
    unsigned char* posBegin = nullptr;
    std::memcpy(&posBegin, base + kVertexPosBegin, sizeof(posBegin));
    unsigned char* uvBegin = nullptr;
    unsigned char* uvEnd = nullptr;
    std::memcpy(&uvBegin, base + kVertexUvBegin, sizeof(uvBegin));
    std::memcpy(&uvEnd, base + kVertexUvBegin + 8, sizeof(uvEnd));
    if (posBegin == nullptr || uvBegin == nullptr || uvEnd < uvBegin) {
        return false;
    }
    const std::size_t firstVertex = static_cast<std::size_t>(from) / 12;
    const std::size_t uvBytes = static_cast<std::size_t>(uvEnd - uvBegin);
    if ((firstVertex + count) * 8 > uvBytes) {
        return false;
    }
    const std::size_t groups = std::min<std::size_t>(6, box.verts.size() / 4);
    float want[6][4][3] = {};
    float uv[6][4][2] = {};
    for (std::size_t g = 0; g < groups; ++g) {
        for (std::size_t k = 0; k < 4; ++k) {
            const ModelVertex& mv = box.verts[g * 4 + k];
            float p[3] = {mv.pos[0], mv.pos[1], mv.pos[2]};
            pitchPoint(p, pitchQuarters);
            turnPoint(p, quarters);
            want[g][k][0] = p[0];
            want[g][k][1] = p[1];
            want[g][k][2] = p[2];
            uv[g][k][0] = mv.uv[0];
            uv[g][k][1] = mv.uv[1];
        }
    }
    float lo[3] = {};
    float hi[3] = {};
    if (!vertsBounds(a, from, to, lo, hi)) {
        return false;
    }
    const float org[3] = {std::floor((lo[0] + hi[0]) * 0.5F),
                          std::floor((lo[1] + hi[1]) * 0.5F),
                          std::floor((lo[2] + hi[2]) * 0.5F)};
    float got[8][3] = {};
    for (std::size_t i = 0; i < count; ++i) {
        std::memcpy(got[i], posBegin + from + static_cast<std::ptrdiff_t>(i) * 12,
                    sizeof(got[i]));
        for (int k = 0; k < 3; ++k) {
            got[i][k] -= org[k];
        }
    }
    std::size_t best = 0;
    float bestScore = 1e9F;
    for (std::size_t g = 0; g < groups; ++g) {
        float score = 0.0F;
        for (std::size_t i = 0; i < count; ++i) {
            float closest = 1e9F;
            for (std::size_t k = 0; k < 4; ++k) {
                const float dx = got[i][0] - want[g][k][0];
                const float dy = got[i][1] - want[g][k][1];
                const float dz = got[i][2] - want[g][k][2];
                closest = std::min(closest, dx * dx + dy * dy + dz * dz);
            }
            score += closest;
        }
        if (score < bestScore) {
            bestScore = score;
            best = g;
        }
    }
    bool wrote = false;
    for (std::size_t i = 0; i < count; ++i) {
        std::size_t pick = 0;
        float closest = 1e9F;
        for (std::size_t k = 0; k < 4; ++k) {
            const float dx = got[i][0] - want[best][k][0];
            const float dy = got[i][1] - want[best][k][1];
            const float dz = got[i][2] - want[best][k][2];
            const float d = dx * dx + dy * dy + dz * dz;
            if (d < closest) {
                closest = d;
                pick = k;
            }
        }
        const float u = (static_cast<float>(col) + uv[best][pick][0] * static_cast<float>(tw))
                        / static_cast<float>(cols);
        const float v = (static_cast<float>(row) + uv[best][pick][1] * static_cast<float>(th))
                        / static_cast<float>(rows);
        float out[2] = {u, v};
        std::memcpy(uvBegin + (firstVertex + i) * 8, out, sizeof(out));
        wrote = true;
    }
    return wrote;
}

void turnVerts(void* a, std::ptrdiff_t from, std::ptrdiff_t to, float yawDeg)
{
    if (yawDeg == 0.0F) {
        return;
    }
    float lo[3] = {};
    float hi[3] = {};
    if (!vertsBounds(a, from, to, lo, hi)) {
        return;
    }
    unsigned char* begin = nullptr;
    std::memcpy(&begin, static_cast<unsigned char*>(a) + kVertexPosBegin, sizeof(begin));
    if (begin == nullptr) {
        return;
    }
    const float cx = std::floor((lo[0] + hi[0]) * 0.5F) + 0.5F;
    const float cz = std::floor((lo[2] + hi[2]) * 0.5F) + 0.5F;
    const float rad = yawDeg * 3.14159265F / 180.0F;
    const float cs = std::cos(rad);
    const float sn = std::sin(rad);
    for (unsigned char* q = begin + from; q + 12 <= begin + to; q += 12) {
        float v[3] = {};
        std::memcpy(v, q, sizeof(v));
        const float dx = v[0] - cx;
        const float dz = v[2] - cz;
        v[0] = cx + dx * cs - dz * sn;
        v[2] = cz + dx * sn + dz * cs;
        std::memcpy(q, v, sizeof(v));
    }
}

using ChunkMeshBuildFn = void*(__fastcall*)(void*, void*, void*, void*, void*, void*);
ChunkMeshBuildFn g_chunkMeshBuild = nullptr;

bool chunkMeshDiagOn()
{
    static const bool on = [] {
        std::error_code ec;
        return std::filesystem::exists(paths::dataDir() / L"diag-chunkmesh.txt", ec);
    }();
    return on;
}

inline constexpr std::size_t kMeshLayers = 22;

inline constexpr std::size_t kCtxLayerVerts = 0x1c8;
inline constexpr std::size_t kCtxOutputGate = 0x2c9;

struct ChunkMeshDiag {
    std::int32_t org[3];
    std::uint8_t flag2a4;
    std::uint32_t start[kMeshLayers];
    std::uint32_t count[kMeshLayers];
    std::uint32_t verts[kMeshLayers];
};

bool readChunkOrigin(const void* recV, const void* ctxV, std::int32_t* out,
                     std::uint32_t* ghostIdx)
{
    __try {
        const auto* const rec = static_cast<const unsigned char*>(recV);
        std::memcpy(&out[0], rec + 0x34, sizeof(std::int32_t));
        std::memcpy(&out[1], rec + 0x38, sizeof(std::int32_t));
        std::memcpy(&out[2], rec + 0x3c, sizeof(std::int32_t));
        *ghostIdx = 0;
        if (ctxV != nullptr) {
            const auto* const ctx = static_cast<const unsigned char*>(ctxV);
            std::uint32_t sum = 0;
            for (std::size_t i = 0; i < kMeshLayers; ++i) {
                std::uint32_t one = 0;
                std::memcpy(&one, ctx + kCtxLayerVerts + i * 8, sizeof(one));
                sum += one;
            }
            *ghostIdx = sum;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool chunkContentChanged(const std::int32_t org[3], std::uint32_t now)
{
    struct Seen {
        std::int32_t x = 0, y = 0, z = 0;
        std::uint32_t idx = 0;
        bool used = false;
    };
    static std::mutex mu;
    static Seen seen[16];
    std::unique_lock<std::mutex> lock{mu};
    Seen* slot = nullptr;
    for (Seen& one : seen) {
        if (one.used && one.x == org[0] && one.y == org[1] && one.z == org[2]) {
            slot = &one;
            break;
        }
        if (slot == nullptr && !one.used) {
            slot = &one;
        }
    }
    if (slot == nullptr) {
        slot = &seen[0];
        slot->used = false;
    }
    if (!slot->used) {
        slot->used = true;
        slot->x = org[0];
        slot->y = org[1];
        slot->z = org[2];
        slot->idx = now;
        return false;
    }
    const bool shrank = now != slot->idx;
    slot->idx = now;
    lock.unlock();
    static std::atomic<int> told{0};
    return shrank;
}

bool readChunkMeshDiag(const void* ctxV, const void* recV, ChunkMeshDiag* out)
{
    __try {
        const auto* const ctx = static_cast<const unsigned char*>(ctxV);
        const auto* const rec = static_cast<const unsigned char*>(recV);
        std::memcpy(&out->org[0], rec + 0x34, sizeof(std::int32_t));
        std::memcpy(&out->org[1], rec + 0x38, sizeof(std::int32_t));
        std::memcpy(&out->org[2], rec + 0x3c, sizeof(std::int32_t));
        std::memcpy(&out->flag2a4, ctx + kCtxOutputGate, sizeof(std::uint8_t));
        for (std::size_t i = 0; i < kMeshLayers; ++i) {
            std::memcpy(&out->start[i], ctx + 0xc8 + i * 8, sizeof(std::uint32_t));
            std::memcpy(&out->count[i], ctx + 0xc8 + i * 8 + 4, sizeof(std::uint32_t));
            std::memcpy(&out->verts[i], ctx + kCtxLayerVerts + i * 8, sizeof(std::uint32_t));
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* __fastcall detourChunkMeshBuild(void* ctx, void* rec, void* a3, void* a4, void* a5,
                                      void* a6)
{
    const long long perfBegan = perf::now();
    t_buildBoxTries = 0;
    t_buildBoxStacked = 0;
    const std::uint64_t buildSeq = nextBuildSeq();
    void* const ret = g_chunkMeshBuild != nullptr
                          ? g_chunkMeshBuild(ctx, rec, a3, a4, a5, a6)
                          : nullptr;
    perf::add(perf::Slot::ChunkBuild, perfBegan);
    noteChunkBoxTries(rec, t_buildBoxTries, t_buildBoxStacked, buildSeq);
    if (rec != nullptr && beModelsOn()) {
        std::int32_t org[3] = {};
        std::uint32_t ghostIdx = 0;
        if (readChunkOrigin(rec, ctx, org, &ghostIdx)
            && chunkContentChanged(org, ghostIdx)) {
            Schematica::instance().noteChunkRebuilt(org[0], org[1], org[2]);
        }
    }
    noteChunkBuiltForRebuilds(rec, buildSeq);
    return ret;
}

void __fastcall detourBlockTessellate(void* self, void* a, void* graphics, void* record,
                                      void* arg5)
{
    constexpr std::size_t kVertexColorBegin = 0x70;
    constexpr std::size_t kVertexColorEnd = 0x78;
    constexpr std::size_t kVertexColorSanity = 1u << 20;

    blocks::noteMeshThread(GetCurrentThreadId());

    blocks::DiffColor diffColor = blocks::DiffColor::None;
    if (record != nullptr && self != nullptr && a != nullptr && blocks::ghostOn()
        && blocks::meshBoxesOn() && blocks::boxBlock() != nullptr) {
        const auto* const p = static_cast<const std::int32_t*>(record);
        diffColor = blocks::diffCellAt(p[0], p[1], p[2]);
    }
    const bool wantsBox = diffColor != blocks::DiffColor::None;

    const void* overlayWant = nullptr;
    if (record != nullptr && self != nullptr && a != nullptr && blocks::ghostOn()
        && blocks::ghostOverMismatchOn()) {
        const auto* const p = static_cast<const std::int32_t*>(record);
        const blocks::DiffColor here =
            wantsBox ? diffColor : blocks::diffCellAt(p[0], p[1], p[2]);
        if (here == blocks::DiffColor::Wrong || here == blocks::DiffColor::State) {
            overlayWant = blocks::wantBlockAt(p[0], p[1], p[2]);
        }
    }

    float ghostAlpha = 1.0F;
    bool isGhost = false;
    const void* ghostBlock = nullptr;
    if (record != nullptr && self != nullptr && a != nullptr && blocks::ghostOn()) {
        const auto* const p = static_cast<const std::int32_t*>(record);
        ghostBlock = blocks::ghostBlockAt(p[0], p[1], p[2]);
        if (ghostBlock != nullptr) {
            ghostAlpha = blocks::ghostAlpha();
            blocks::noteGhostHit(blocks::GhostHook::Tessellate);
            blocks::noteGhostDraw(p[0], p[1], p[2]);
            isGhost = true;
        }
    }

    std::size_t colorBytesBefore = 0;
    bool colorTracked = false;
    if (isGhost && memory::isReadable(a, kVertexColorEnd + sizeof(void*))) {
        const auto* const base = static_cast<const unsigned char*>(a);
        unsigned char* begin = nullptr;
        unsigned char* end = nullptr;
        std::memcpy(&begin, base + kVertexColorBegin, sizeof(begin));
        std::memcpy(&end, base + kVertexColorEnd, sizeof(end));
        if (end >= begin) {
            colorBytesBefore = static_cast<std::size_t>(end - begin);
            colorTracked = true;
        }
    }

    constexpr std::size_t kTessellatorLayer = 0x80;
    std::uint32_t savedLayer = 0;
    bool layerPatched = false;
    bool layerKnown = false;
    if ((isGhost || wantsBox || overlayWant != nullptr) && self != nullptr
        && tessellatorLayerWritable(self)) {
        std::memcpy(&savedLayer, static_cast<const unsigned char*>(self) + kTessellatorLayer,
                    sizeof(savedLayer));
        layerKnown = true;
        if (savedLayer == static_cast<std::uint32_t>(blocks::kGhostLayer)) {
            const int real = blocks::realLayer(graphics);
            if (real >= 0 && real != blocks::kGhostLayer) {
                const auto want = static_cast<std::uint32_t>(real);
                std::memcpy(static_cast<unsigned char*>(self) + kTessellatorLayer, &want,
                            sizeof(want));
                layerPatched = true;
            }
        }
    }

    const bool ghostBucket =
        !layerKnown || savedLayer == static_cast<std::uint32_t>(blocks::kGhostLayer);
    const bool skipReal = isGhost && !ghostBucket;
    if (wantsBox) {
        (ghostBucket ? g_boxBucketOk : g_boxBucketSkip)
            .fetch_add(1, std::memory_order_relaxed);
        if (ghostBucket) {
            ++t_buildBoxTries;
        }
        if (!ghostBucket) {
            g_boxBucketLastLayer.store(savedLayer, std::memory_order_relaxed);
        }
    }

    std::ptrdiff_t posBeforeReal = -1;
    if (isGhost && ghostBucket && memory::isReadable(a, kVertexUv2Begin + 24)) {
        posBeforeReal = tripleLength(a, kVertexPosBegin);
    }

    if (g_blockTessellate != nullptr && !skipReal) {
        g_blockTessellate(self, a, graphics, record, arg5);
    }

    const bool realDrewIt =
        posBeforeReal >= 0 && tripleLength(a, kVertexPosBegin) > posBeforeReal;
    const bool beGhostCell = isGhost && ghostBucket && ghostBlock != nullptr
                             && blocks::hasBlockEntityFast(ghostBlock);
    const bool haveModel = beGhostCell && hasModelBoxes(ghostBlock);
    if (beGhostCell && realDrewIt && !haveModel) {
        g_beCubeRealDrew.fetch_add(1, std::memory_order_relaxed);
    }
    if (beGhostCell && realDrewIt && haveModel) {
        g_beCubeBothDrew.fetch_add(1, std::memory_order_relaxed);
    }
    if (beGhostCell && (haveModel || !realDrewIt)) {
        g_beCubeCells.fetch_add(1, std::memory_order_relaxed);
        const int mode = beCubeMode();
        if (mode > 0 && memory::isReadable(a, kVertexUv2Begin + 24)) {
            const auto* const p = static_cast<const std::int32_t*>(record);
            const void* const use =
                (mode == 2 || mode == 4 || mode == 6) ? blocks::boxBlock() : ghostBlock;
            struct BeRecord {
                std::int32_t x = 0;
                std::int32_t y = 0;
                std::int32_t z = 0;
                std::int32_t pad = 0;
                const void* block = nullptr;
            };
            static_assert(sizeof(BeRecord) == 24);
            BeRecord rec;
            rec.x = p[0];
            rec.y = p[1];
            rec.z = p[2];
            rec.block = use;
            alignas(16) unsigned char scratch[256] = {};
            const std::ptrdiff_t posBefore = tripleLength(a, kVertexPosBegin);
            const std::vector<ModelBox> boxes =
                (mode == 5) ? modelBoxesOf(ghostBlock) : std::vector<ModelBox>{};
            float yaw = 0.0F;
            int pitchQuarters = 0;
            if (!boxes.empty()
                && !blocks::ghostOrientAt(p[0], p[1], p[2], yaw, pitchQuarters)) {
                yaw = blocks::ghostYaw(ghostBlock);
            }
            const int quarters = static_cast<int>(std::lround(yaw / 90.0F));
            const float restYaw = yaw - static_cast<float>(quarters) * 90.0F;
            int atlasCol = 0;
            int atlasRow = 0;
            int atlasTw = 0;
            int atlasTh = 0;
            int atlasCols = 0;
            int atlasRows = 0;
            bool haveTile = false;
            if (!boxes.empty()) {
                const std::string tex = modelTextureOf(ghostBlock);
                haveTile = !tex.empty()
                           && atlas::entityTile(tex, atlasCol, atlasRow, atlasTw, atlasTh,
                                                atlasCols, atlasRows);
            }
            if (use != nullptr && !boxes.empty() && g_blockCubeFaces != nullptr) {
                for (const ModelBox& box : boxes) {
                    const ModelBox turned = turnBox(pitchBox(box, pitchQuarters), quarters);
                    SavedShape saved;
                    const bool told = writeShape(self, turned, saved);
                    const std::ptrdiff_t was = tripleLength(a, kVertexPosBegin);
                    if (haveTile) {
                        for (unsigned bit = 0; bit < 6; ++bit) {
                            const std::ptrdiff_t f0 = tripleLength(a, kVertexPosBegin);
                            g_blockCubeFaces(self, a, const_cast<void*>(use), &rec,
                                             1U << bit, nullptr);
                            const std::ptrdiff_t f1 = tripleLength(a, kVertexPosBegin);
                            if (f1 > f0
                                && paintFaceUv(a, f0, f1, box, quarters, pitchQuarters,
                                               atlasCol, atlasRow, atlasTw, atlasTh,
                                               atlasCols, atlasRows)) {
                                g_uvPainted.fetch_add(1, std::memory_order_relaxed);
                            } else if (f1 > f0) {
                                g_uvMissed.fetch_add(1, std::memory_order_relaxed);
                            }
                        }
                    } else {
                        g_blockCubeFaces(self, a, const_cast<void*>(use), &rec, 0x3fU,
                                         nullptr);
                    }
                    const std::ptrdiff_t now = tripleLength(a, kVertexPosBegin);
                    if (told) {
                        restoreShape(self, saved);
                    }
                    if (was >= 0 && now > was) {
                        turnVerts(a, was, now, restYaw);
                    }
                }
            } else if (use != nullptr) {
                if (mode == 5 && boxes.empty()) {
                    g_beCubeNoBox.fetch_add(1, std::memory_order_relaxed);
                }
                if (mode == 6) {
                    if (g_blockCubeFaces != nullptr) {
                        g_blockCubeFaces(self, a, const_cast<void*>(use), &rec, 0x3fU,
                                         nullptr);
                    }
                } else if (mode == 3 || mode == 4) {
                    if (g_blockTessellate != nullptr) {
                        g_blockTessellate(self, a, const_cast<void*>(use), &rec, arg5);
                    }
                } else if (g_blockTessellateCube != nullptr) {
                    g_blockTessellateCube(self, a, const_cast<void*>(use), &rec, scratch);
                }
            }
            const std::ptrdiff_t posAfter = tripleLength(a, kVertexPosBegin);
            const bool grew = posBefore >= 0 && posAfter > posBefore
                              && posAfter - posBefore
                                     <= static_cast<std::ptrdiff_t>(kVertexColorSanity);
            if (grew) {
                g_beCubeVerts.fetch_add(
                    static_cast<std::size_t>((posAfter - posBefore) / 12),
                    std::memory_order_relaxed);
            } else {
                g_beCubeMiss.fetch_add(1, std::memory_order_relaxed);
            }
            static std::atomic<int> shots{0};
        }
    }

    std::size_t colorBytesAfterReal = 0;
    bool realTracked = false;
    if (colorTracked) {
        if (const std::ptrdiff_t now = tripleLength(a, kVertexColorBegin); now >= 0) {
            colorBytesAfterReal = static_cast<std::size_t>(now);
            realTracked = true;
        }
    }

    if (overlayWant != nullptr && ghostBucket && g_blockTessellate != nullptr
        && memory::isReadable(a, kVertexUv2Begin + 24)) {
        struct WantRecord {
            std::int32_t x = 0;
            std::int32_t y = 0;
            std::int32_t z = 0;
            std::int32_t pad = 0;
            const void* block = nullptr;
        };
        static_assert(sizeof(WantRecord) == 24);
        const auto* const p = static_cast<const std::int32_t*>(record);
        WantRecord rec;
        rec.x = p[0];
        rec.y = p[1];
        rec.z = p[2];
        rec.block = overlayWant;
        const std::ptrdiff_t colorBefore = tripleLength(a, kVertexColorBegin);
        const std::ptrdiff_t posBefore = tripleLength(a, kVertexPosBegin);

        std::uint32_t layerNow = 0;
        bool layerSwapped = false;
        if (layerKnown) {
            std::memcpy(&layerNow, static_cast<const unsigned char*>(self) + kTessellatorLayer,
                        sizeof(layerNow));
            const int wantLayer = blocks::realLayer(overlayWant);
            if (wantLayer >= 0 && static_cast<std::uint32_t>(wantLayer) != layerNow) {
                const auto value = static_cast<std::uint32_t>(wantLayer);
                std::memcpy(static_cast<unsigned char*>(self) + kTessellatorLayer, &value,
                            sizeof(value));
                layerSwapped = true;
            }
        }
        g_blockTessellate(self, a, const_cast<void*>(overlayWant), &rec, arg5);
        if (layerSwapped) {
            std::memcpy(static_cast<unsigned char*>(self) + kTessellatorLayer, &layerNow,
                        sizeof(layerNow));
        }

        const std::ptrdiff_t colorAfter = tripleLength(a, kVertexColorBegin);
        const std::ptrdiff_t posAfter = tripleLength(a, kVertexPosBegin);
        const bool grewColor = colorBefore >= 0 && colorAfter > colorBefore
                               && colorAfter - colorBefore
                                      <= static_cast<std::ptrdiff_t>(kVertexColorSanity);
        const bool grewPos = posBefore >= 0 && posAfter > posBefore
                             && posAfter - posBefore
                                    <= static_cast<std::ptrdiff_t>(kVertexColorSanity);
        g_overlayStacked.fetch_add(grewPos ? 1 : 0, std::memory_order_relaxed);
        g_overlayMissed.fetch_add(grewPos ? 0 : 1, std::memory_order_relaxed);
        if (grewColor) {
            const auto* const base = static_cast<const unsigned char*>(a);
            unsigned char* begin = nullptr;
            std::memcpy(&begin, base + kVertexColorBegin, sizeof(begin));
            if (begin != nullptr) {
                const auto value = static_cast<unsigned char>(
                    std::lround(std::clamp(blocks::ghostAlpha(), 0.0F, 1.0F) * 255.0F));
                for (unsigned char* q = begin + colorBefore; q + 4 <= begin + colorAfter;
                     q += 4) {
                    q[3] = value;
                }
            }
        }
        if (grewPos) {
            const auto* const base = static_cast<const unsigned char*>(a);
            unsigned char* begin = nullptr;
            std::memcpy(&begin, base + kVertexPosBegin, sizeof(begin));
            if (begin != nullptr) {
                const float center[3] = {static_cast<float>(p[0] & 15) + 0.5F,
                                         static_cast<float>(p[1] & 15) + 0.5F,
                                         static_cast<float>(p[2] & 15) + 0.5F};
                const float kGrow =
                    1.0F + static_cast<float>(overlayGrowUnits()) / 512.0F;
                for (unsigned char* q = begin + posBefore; q + 12 <= begin + posAfter;
                     q += 12) {
                    for (int k = 0; k < 3; ++k) {
                        float v = 0.0F;
                        std::memcpy(&v, q + k * 4, sizeof(v));
                        v = center[k] + (v - center[k]) * kGrow;
                        std::memcpy(q + k * 4, &v, sizeof(v));
                    }
                }
            }
        }
    }

    if (wantsBox && ghostBucket && memory::isReadable(a, kVertexUv2Begin + 24)) {
        struct BoxRecord {
            std::int32_t x = 0;
            std::int32_t y = 0;
            std::int32_t z = 0;
            std::int32_t pad = 0;
            const void* block = nullptr;
        };
        static_assert(sizeof(BoxRecord) == 24);

        const auto* const p = static_cast<const std::int32_t*>(record);
        const void* const box = blocks::boxBlock();

        const auto lengthOf = [a](std::size_t at) -> std::ptrdiff_t {
            return tripleLength(a, at);
        };
        const std::ptrdiff_t colorBefore = lengthOf(kVertexColorBegin);
        const std::ptrdiff_t posBefore = lengthOf(kVertexPosBegin);
        const std::ptrdiff_t uvBefore = lengthOf(kVertexUvBegin);

        BoxRecord rec;
        rec.x = p[0];
        rec.y = p[1];
        rec.z = p[2];
        rec.block = box;
        if (g_blockTessellate != nullptr && box != nullptr) {
            ++t_boxDepth;
            g_blockTessellate(self, a, const_cast<void*>(box), &rec, arg5);
            --t_boxDepth;
        }

        const std::ptrdiff_t colorAfter = lengthOf(kVertexColorBegin);
        const std::ptrdiff_t posAfter = lengthOf(kVertexPosBegin);
        blocks::noteGhostHit(colorAfter > colorBefore ? blocks::GhostHook::Box
                                                     : blocks::GhostHook::BoxMiss);
        if (colorAfter > colorBefore) {
            noteBoxCoverage(rec.x, rec.z);
            ++t_buildBoxStacked;
        }
        if (posBefore >= 0 && posAfter > posBefore
            && posAfter - posBefore <= static_cast<std::ptrdiff_t>(kVertexColorSanity)) {
            g_boxVerts.fetch_add(static_cast<std::size_t>((posAfter - posBefore) / 12),
                                 std::memory_order_relaxed);
        }

        float rgb[3] = {1.0F, 1.0F, 1.0F};
        float faceAlpha = 0.35F;
        if (colorBefore >= 0 && colorAfter > colorBefore
            && colorAfter - colorBefore <= static_cast<std::ptrdiff_t>(kVertexColorSanity)
            && boxes::boxStyle(diffColor, rgb, &faceAlpha)) {
            const auto* const base = static_cast<const unsigned char*>(a);
            unsigned char* begin = nullptr;
            std::memcpy(&begin, base + kVertexColorBegin, sizeof(begin));
            if (begin != nullptr) {
                const auto quantize = [](float v) {
                    return static_cast<unsigned char>(
                        std::lround(std::clamp(v, 0.0F, 1.0F) * 255.0F));
                };
                const unsigned char bytes[4] = {quantize(rgb[0]), quantize(rgb[1]),
                                                quantize(rgb[2]), quantize(faceAlpha)};
                for (unsigned char* q = begin + colorBefore; q + 4 <= begin + colorAfter;
                     q += 4) {
                    std::memcpy(q, bytes, sizeof(bytes));
                }
            }
        }

        if (posBefore >= 0 && posAfter > posBefore
            && posAfter - posBefore <= static_cast<std::ptrdiff_t>(kVertexColorSanity)) {
            const auto* const base = static_cast<const unsigned char*>(a);
            unsigned char* begin = nullptr;
            std::memcpy(&begin, base + kVertexPosBegin, sizeof(begin));
            if (begin != nullptr) {
                const float center[3] = {static_cast<float>(p[0] & 15) + 0.5F,
                                         static_cast<float>(p[1] & 15) + 0.5F,
                                         static_cast<float>(p[2] & 15) + 0.5F};
                constexpr float kGrow = 1.0078125F;
                for (unsigned char* q = begin + posBefore; q + 12 <= begin + posAfter;
                     q += 12) {
                    for (int k = 0; k < 3; ++k) {
                        float v = 0.0F;
                        std::memcpy(&v, q + k * 4, sizeof(v));
                        v = center[k] + (v - center[k]) * kGrow;
                        std::memcpy(q + k * 4, &v, sizeof(v));
                    }
                }
            }
        }

        if (int col = 0, row = 0, gridCols = 64, gridRows = 64;
            boxUvTile(col, row, gridCols, gridRows)) {
            const std::ptrdiff_t uvAfter = lengthOf(kVertexUvBegin);
            if (uvBefore >= 0 && uvAfter > uvBefore
                && uvAfter - uvBefore <= static_cast<std::ptrdiff_t>(kVertexColorSanity)) {
                const auto* const base = static_cast<const unsigned char*>(a);
                unsigned char* begin = nullptr;
                std::memcpy(&begin, base + kVertexUvBegin, sizeof(begin));
                if (begin != nullptr) {
                    constexpr float kInset = 0.5F / 16.0F;
                    constexpr std::size_t kFaceVerts = 4;
                    unsigned char* const stop = begin + uvAfter;
                    for (unsigned char* face = begin + uvBefore;
                         face + kFaceVerts * 8 <= stop; face += kFaceVerts * 8) {
                        float uv[kFaceVerts][2] = {};
                        for (std::size_t v = 0; v < kFaceVerts; ++v) {
                            std::memcpy(uv[v], face + v * 8, sizeof(uv[v]));
                        }
                        for (int k = 0; k < 2; ++k) {
                            float lo = uv[0][k];
                            float hi = uv[0][k];
                            for (std::size_t v = 1; v < kFaceVerts; ++v) {
                                lo = std::min(lo, uv[v][k]);
                                hi = std::max(hi, uv[v][k]);
                            }
                            const float span = hi - lo;
                            const int tile = (k == 0) ? col : row;
                            const float grid =
                                static_cast<float>((k == 0) ? gridCols : gridRows);
                            for (std::size_t v = 0; v < kFaceVerts; ++v) {
                                const float t = span > 1e-6F ? (uv[v][k] - lo) / span : 0.0F;
                                const float want =
                                    tile < 0
                                        ? t
                                        : (static_cast<float>(tile) + kInset
                                           + t * (1.0F - 2.0F * kInset))
                                              / (grid > 0.0F ? grid : 64.0F);
                                std::memcpy(face + v * 8 + k * 4, &want, sizeof(want));
                            }
                        }
                    }
                }
            }
        }
    }

    if (layerPatched) {
        std::memcpy(static_cast<unsigned char*>(self) + kTessellatorLayer, &savedLayer,
                    sizeof(savedLayer));
    }

    if (isGhost && colorTracked) {
        const auto* const base = static_cast<const unsigned char*>(a);
        unsigned char* begin = nullptr;
        unsigned char* end = nullptr;
        std::memcpy(&begin, base + kVertexColorBegin, sizeof(begin));
        std::memcpy(&end, base + kVertexColorEnd, sizeof(end));
        if (begin != nullptr && end > begin) {
            auto now = static_cast<std::size_t>(end - begin);
            if (realTracked && colorBytesAfterReal <= now) {
                now = colorBytesAfterReal;
            }
            if (now > colorBytesBefore && now - colorBytesBefore <= kVertexColorSanity) {
                const auto value = static_cast<unsigned char>(
                    std::lround(std::clamp(ghostAlpha, 0.0F, 1.0F) * 255.0F));
                unsigned char* const stop = begin + now;
                for (unsigned char* p = begin + colorBytesBefore; p + 4 <= stop; p += 4) {
                    p[3] = value;
                }
                blocks::noteGhostHit(blocks::GhostHook::Alpha);
            }
        }
    }
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

    if (!ItemStackRequest::instance().suppressingInputReset()) {
        FastInventory::instance().onInventoryOpenSent(client);
    }
}

constexpr std::size_t kMoveApplySetupFlag = 0x50;

__declspec(noinline) bool moveApplyNeedsSetup(const void* self)
{
    if (self == nullptr) {
        return false;
    }
    __try {
        return *(static_cast<const std::uint8_t*>(self) + kMoveApplySetupFlag) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* __fastcall detourMoveApply(void* self, void* pendingInput, void* a3, void* a4)
{
    (void)moveApplyNeedsSetup;
    return (g_moveApply != nullptr) ? g_moveApply(self, pendingInput, a3, a4) : nullptr;
}

void* __fastcall detourInputGather(void* a1, void* out, void* a3, void* a4)
{
    FreeCamera::instance().onInputGatherBefore(out);

    void* const result = (g_inputGather != nullptr) ? g_inputGather(a1, out, a3, a4) : nullptr;

    FreeCamera::instance().onInputGather(out, a1);
    return result;
}

void* __fastcall detourMoveIntent(void* out, void* input, void* a3, void* a4)
{
    void* const result = (g_moveIntent != nullptr) ? g_moveIntent(out, input, a3, a4) : nullptr;
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
    return Fullbright::instance().onGetEffect(effectId, original);
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

void* __fastcall detourFogSettingsFetch(void* self, void* out, void* src, void* r9)
{
    void* const result =
        (g_fogSettingsFetch != nullptr) ? g_fogSettingsFetch(self, out, src, r9) : nullptr;
    if (out != nullptr && NoRender::instance().fogSuppressed()) {
        auto* const bytes = static_cast<unsigned char*>(out);
        if (bytes[0x1c] != 0) {
            constexpr float kFar = 1.0e6f;
            std::memcpy(bytes + 0x10, &kFar, sizeof(kFar));
            std::memcpy(bytes + 0x14, &kFar, sizeof(kFar));
        }
    }
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
    uiprobe::overrideKeyActionName(out, index);
    return result;
}

void* __fastcall detourKeyRowListBuild(void* array, void* rdx)
{
    return (g_keyRowListBuild != nullptr) ? g_keyRowListBuild(array, rdx) : nullptr;
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
    constexpr bool kAddRowEntry = true;
    constexpr bool kPurgeOwnRows = false;
    const bool purged = kPurgeOwnRows && !kAddRowEntry && uiprobe::purgeOwnKeyRows(rdx);
    const bool substituted =
        !purged && (kAddRowEntry && bumped == 0)
        && uiprobe::substituteKeyRows(rdx, kKeepRowsAfterConsume);
    void* const result =
        (g_oreKeyRowsBuild != nullptr) ? g_oreKeyRowsBuild(out, rdx, r8) : nullptr;
    if (substituted && !kKeepRowsAfterConsume) {
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

void* __fastcall detourSettingsActionData(void* out, void* source)
{
    return (g_settingsActionData != nullptr) ? g_settingsActionData(out, source) : nullptr;
}

void* __fastcall detourSettingsActionQueryUpdate(void* self)
{
    return (g_settingsActionQueryUpdate != nullptr) ? g_settingsActionQueryUpdate(self) : nullptr;
}

unsigned char __fastcall detourKeyBindingIsDefault(void* self, std::uintptr_t index)
{
    return (g_keyBindingIsDefault != nullptr) ? g_keyBindingIsDefault(self, index) : 0;
}

void* __fastcall detourOreKeyRowsWrap(void* out, void* container)
{
    uiprobe::onOreKeyRowsWrap(out, container);
    return (g_oreKeyRowsWrap != nullptr) ? g_oreKeyRowsWrap(out, container) : nullptr;
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

void* __fastcall detourSettingsTabList(void* self, void* out, void* r8, void* r9)
{
    void* const result =
        (g_settingsTabList != nullptr) ? g_settingsTabList(self, out, r8, r9) : nullptr;
    uiprobe::afterSettingsTabList(out);
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

void hotPathStats(std::size_t& preds, std::size_t& scans, std::size_t& getBlockGhost,
                  std::size_t& getBlockCalls)
{
    preds = g_predAnswers.exchange(0, std::memory_order_relaxed);
    scans = g_visibilityScans.exchange(0, std::memory_order_relaxed);
    getBlockGhost = g_getBlockGhost.exchange(0, std::memory_order_relaxed);
    getBlockCalls = g_getBlockCalls.exchange(0, std::memory_order_relaxed);
}

std::atomic<int> g_beModelsMode{0};
std::atomic<bool> g_beModelsDeciding{false};

bool beModelsOn()
{
    const int now = g_beModelsMode.load(std::memory_order_acquire);
    if (now != 0) {
        return now == 1;
    }
    if (g_beModelsDeciding.exchange(true, std::memory_order_acq_rel)) {
        return false;
    }
    std::error_code ec;
    const bool want = std::filesystem::exists(paths::dataDir() / L"diag-beparts.txt", ec);
    g_beModelsMode.store(want ? 1 : 2, std::memory_order_release);
    g_beModelsDeciding.store(false, std::memory_order_release);
    return want;
}

const void* readWorldBlock(void* region, const int pos[3])
{
    if (region == nullptr || pos == nullptr || g_blockSourceGetBlock == nullptr) {
        return nullptr;
    }
    return g_blockSourceGetBlock(region, pos);
}

void clearStorageMarks()
{

    const std::size_t used = std::min(g_ghostSlotCount.load(std::memory_order_acquire),
                                      kGhostStorageSlots);
    g_ghostSlotCount.store(0, std::memory_order_release);
    for (std::size_t i = 0; i < used; ++i) {
        g_ghostSlots[i].used.store(false, std::memory_order_release);
        g_ghostSlots[i].storage[0].store(nullptr, std::memory_order_relaxed);
        g_ghostSlots[i].storage[1].store(nullptr, std::memory_order_relaxed);
    }
}

void dropStorageMark(int baseX, int baseY, int baseZ)
{
    const std::size_t at = findGhostSlot(baseX, baseY, baseZ);
    if (at >= kGhostStorageSlots) {
        return;
    }
    g_ghostSlots[at].storage[0].store(nullptr, std::memory_order_release);
    g_ghostSlots[at].storage[1].store(nullptr, std::memory_order_release);
}

void armStorageFromSubChunk(void* subChunk)
{
    if (subChunk == nullptr) {
        return;
    }
    void* current[2] = {nullptr, nullptr};
    if (!readStoragePointers(subChunk, current)) {
        return;
    }
    for (void* const storage : current) {
        if (storage == nullptr) {
            continue;
        }
        void* vtable = nullptr;
        if (!readStorageVtable(storage, &vtable) || vtable == nullptr) {
            continue;
        }
        if (seenStorageVtable(vtable)) {
            continue;
        }
        notePendingStorageVtable(vtable);
        noteStorageVtable(vtable);
    }
}

void armPendingStoragePreds()
{
    void* vtable = nullptr;
    while (takePendingStorageVtable(&vtable)) {
        if (vtable == nullptr || !memory::isReadable(vtable, 8 * 4)) {
            continue;
        }
        for (int slot = 1; slot <= 2; ++slot) {
            void* fn = nullptr;
            std::memcpy(&fn, static_cast<const std::uint8_t*>(vtable) + slot * 8,
                        sizeof(fn));
            if (fn != nullptr && memory::inGameModule(fn) && memory::isExecutable(fn, 16)) {
                armStoragePred(fn);
            }
        }
    }
}

void armStorageHooks(void* subChunk, int baseX, int baseY, int baseZ)
{
    if (subChunk == nullptr) {
        return;
    }
    void* current[2] = {nullptr, nullptr};
    if (!readStoragePointers(subChunk, current)) {
        return;
    }
    const std::size_t known = findGhostSlot(baseX, baseY, baseZ);
    for (int layer = 0; layer < 2; ++layer) {
        void* const storage = current[layer];
        if (storage == nullptr) {
            continue;
        }
        if (known < kGhostStorageSlots
            && g_ghostSlots[known].storage[layer].load(std::memory_order_relaxed)
                   == storage) {
            continue;
        }
        if (!memory::isReadable(storage, 8)) {
            continue;
        }
        void* vtable = nullptr;
        std::memcpy(&vtable, storage, sizeof(vtable));
        if (vtable == nullptr || !memory::isReadable(vtable, 8 * 4)) {
            continue;
        }
        for (int slot = 1; slot <= 2; ++slot) {
            void* fn = nullptr;
            std::memcpy(&fn, static_cast<const std::uint8_t*>(vtable) + slot * 8,
                        sizeof(fn));
            if (fn != nullptr && memory::inGameModule(fn) && memory::isExecutable(fn, 16)) {
                armStoragePred(fn);
            }
        }
        noteGhostStorage(baseX, baseY, baseZ, layer, storage);
    }
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
    return vtable[0x80 / sizeof(void*)];
}

void* resolveBlockSourceGetBlock()
{
    void* const region = blockwrite::region();
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
    void* const region = blockwrite::region();
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

using ModelPartFn = void(__fastcall*)(void*, void*, void*, void*, float, unsigned char,
                                      unsigned, void*);
ModelPartFn g_modelPart = nullptr;

extern void* g_matGhost;

const std::size_t kRendererMaterial = 0x120;

constexpr std::size_t kRendererScan = 0x1000;

struct RendererEntry {
    void* who = nullptr;
    void* orig = nullptr;
    std::vector<std::size_t> slots;
    bool scanned = false;
};

std::mutex g_rendererGuard;
std::vector<RendererEntry> g_renderers;

std::string readStdString(const unsigned char* at);

std::string materialName(void* handle)
{
    if (handle == nullptr || !memory::isReadable(handle, 0x38)) {
        return {};
    }
    return readStdString(static_cast<const unsigned char*>(handle) + 0x18);
}

void* readMaterial(void* model)
{
    if (!memory::isReadable(static_cast<unsigned char*>(model) + kRendererMaterial, 8)) {
        return nullptr;
    }
    void* mat = nullptr;
    std::memcpy(&mat, static_cast<unsigned char*>(model) + kRendererMaterial, sizeof(mat));
    if (mat == nullptr || !memory::isReadable(mat, 0x20)) {
        return nullptr;
    }
    void* self = nullptr;
    std::memcpy(&self, mat, sizeof(self));
    return self == mat ? mat : nullptr;
}

void noteRenderer(void* model)
{
    void* const mat = readMaterial(model);
    if (mat == nullptr || mat == g_matGhost) {
        return;
    }
    const std::lock_guard<std::mutex> lock(g_rendererGuard);
    for (auto& entry : g_renderers) {
        if (entry.who == model) {
            if (entry.orig != mat) {
                entry.orig = mat;
                entry.scanned = false;
                entry.slots.clear();
            }
            return;
        }
    }
    if (g_renderers.size() < 128) {
        RendererEntry entry;
        entry.who = model;
        entry.orig = mat;
        g_renderers.push_back(std::move(entry));
    }
}

void scanRendererSlots(RendererEntry& entry)
{
    entry.scanned = true;
    entry.slots.clear();
    auto* const base = static_cast<unsigned char*>(entry.who);
    auto* const block = static_cast<unsigned char*>(entry.orig) - 0x10;
    for (std::size_t off = 0; off + 16 <= kRendererScan; off += 8) {
        if (!memory::isWritable(base + off, 16)) {
            continue;
        }
        void* v = nullptr;
        void* ctrl = nullptr;
        std::memcpy(&v, base + off, sizeof(v));
        std::memcpy(&ctrl, base + off + 8, sizeof(ctrl));
        if (v == entry.orig && ctrl == block && entry.slots.size() < 16) {
            entry.slots.push_back(off);
        }
    }
    if (std::find(entry.slots.begin(), entry.slots.end(), kRendererMaterial)
        == entry.slots.end()) {
        entry.slots.push_back(kRendererMaterial);
    }
    const std::string name = materialName(entry.orig);
    std::wstring where;
    for (const std::size_t off : entry.slots) {
        where += std::format(L"+{:#x} ", off);
    }
}

thread_local std::int32_t t_bePos[3] = {0, 0, 0};

thread_local void* t_beArgs[12] = {};

bool modelDiagOn()
{
    static int mode = 0;
    if (mode == 0) {
        std::error_code ec;
        mode = std::filesystem::exists(paths::dataDir() / L"diag-model.txt", ec) ? 1 : 2;
    }
    return mode == 1;
}

bool beSkipOn()
{
    static int mode = 0;
    if (mode == 0) {
        std::error_code ec;
        mode = std::filesystem::exists(paths::dataDir() / L"diag-beskip.txt", ec) ? 1 : 2;
    }
    return mode == 1;
}

bool texDiagOn()
{
    static int mode = 0;
    if (mode == 0) {
        std::error_code ec;
        mode = std::filesystem::exists(paths::dataDir() / L"diag-cj-tex.txt", ec) ? 1 : 2;
    }
    return mode == 1;
}

bool looksLikeResourcePath(const std::string& text)
{
    if (text.size() < 5 || text.size() > 200) {
        return false;
    }
    for (const char c : text) {
        if (c < 0x20 || c > 0x7e) {
            return false;
        }
    }
    return text.find("textures/") != std::string::npos
           || (text.size() > 4 && text.compare(text.size() - 4, 4, ".png") == 0);
}

std::mutex g_texDiagMutex;
std::vector<const void*> g_texDiagSeen;

std::unordered_map<const void*, std::vector<std::string>> g_modelTextures;

std::atomic<int> g_packProbes{0};

void probePackImageOnce(const std::string& path)
{
    if (path.rfind("textures/", 0) != 0
        || g_packProbes.fetch_add(1, std::memory_order_relaxed) >= 4) {
        return;
    }
    const std::wstring file = pack::findFile(path);
    if (file.empty()) {
        return;
    }
    std::vector<std::uint8_t> rgba;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    if (!pack::decodeImage(file, rgba, width, height)) {
        return;
    }
}

void noteLearnedTexture(const void* key, std::string path)
{
    if (key == nullptr || path.empty() || path.size() > 200) {
        return;
    }
    if (texDiagOn() && path.rfind("textures/entity/", 0) == 0) {
        probePackImageOnce(path);
    }
    const std::lock_guard<std::mutex> lock{g_modelMutex};
    auto& list = g_modelTextures[key];
    if (list.size() >= 8 || std::find(list.begin(), list.end(), path) != list.end()) {
        return;
    }
    list.push_back(std::move(path));
}

std::string modelTextureOf(const void* key)
{
    const std::lock_guard<std::mutex> lock{g_modelMutex};
    const auto it = g_modelTextures.find(key);
    if (it == g_modelTextures.end()) {
        return {};
    }
    for (const std::string& one : it->second) {
        if (one.rfind("textures/", 0) == 0) {
            return one;
        }
    }
    return {};
}

std::vector<std::string> learnedTextures(const void* key)
{
    const std::lock_guard<std::mutex> lock{g_modelMutex};
    const auto it = g_modelTextures.find(key);
    return it != g_modelTextures.end() ? it->second : std::vector<std::string>{};
}

struct TextureEntry {
    void* handle = nullptr;
    std::string path;
};

std::mutex g_textureNameMutex;
std::vector<TextureEntry> g_textureNames;

void noteTextureHandle(void* handle, std::string path)
{
    if (handle == nullptr || path.empty() || path.size() > 200) {
        return;
    }
    const std::lock_guard<std::mutex> lock{g_textureNameMutex};
    if (g_textureNames.size() >= 1024) {
        return;
    }
    for (const TextureEntry& one : g_textureNames) {
        if (one.handle == handle) {
            return;
        }
    }
    g_textureNames.push_back({handle, std::move(path)});
}

bool looksLikeTexture(const void* p)
{
    if (p == nullptr || !memory::isReadable(p, 0x40)) {
        return false;
    }
    const auto* const base = static_cast<const unsigned char*>(p);
    void* vtable = nullptr;
    std::memcpy(&vtable, base, sizeof(vtable));
    if (!memory::inGameModule(vtable) || !memory::isReadable(vtable, 8)) {
        return false;
    }
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t format = 0;
    std::memcpy(&width, base + 0x18, sizeof(width));
    std::memcpy(&height, base + 0x1c, sizeof(height));
    std::memcpy(&format, base + 0x20, sizeof(format));
    if (width < 1 || width > 8192 || height < 1 || height > 8192) {
        return false;
    }
    const unsigned char* desc = nullptr;
    std::memcpy(&desc, base + 0x60, sizeof(desc));
    const auto raw = reinterpret_cast<std::uintptr_t>(desc);
    if (raw < 0x10000 || (raw & 7) != 0 || !memory::isReadable(desc, 0x10)) {
        return false;
    }
    std::uint32_t dw = 0;
    std::uint32_t dh = 0;
    std::uint32_t df = 0;
    std::memcpy(&dw, desc + 4, sizeof(dw));
    std::memcpy(&dh, desc + 8, sizeof(dh));
    std::memcpy(&df, desc + 12, sizeof(df));
    return dw == width && dh == height && df == format;
}

std::string textureNameOf(const void* handle)
{
    if (handle == nullptr) {
        return {};
    }
    const std::lock_guard<std::mutex> lock{g_textureNameMutex};
    for (const TextureEntry& one : g_textureNames) {
        if (one.handle == handle) {
            return one.path;
        }
    }
    return {};
}

std::size_t textureNameCount()
{
    const std::lock_guard<std::mutex> lock{g_textureNameMutex};
    return g_textureNames.size();
}

void findTextureHandles(const unsigned char* base, std::size_t span,
                        std::vector<std::string>& out, int& reports, int depth)
{
    if (base == nullptr || reports >= 24) {
        return;
    }
    for (std::size_t off = 0; off + 8 <= span; off += 8) {
        if (!memory::isReadable(base + off, 8)) {
            continue;
        }
        void* v = nullptr;
        std::memcpy(&v, base + off, sizeof(v));
        std::string name = textureNameOf(v);
        if (name.empty() && looksLikeTexture(v)) {
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            std::memcpy(&width, static_cast<const unsigned char*>(v) + 0x18, sizeof(width));
            std::memcpy(&height, static_cast<const unsigned char*>(v) + 0x1c, sizeof(height));
            name = std::format("<unnamed {}x{} {:#x}>", width, height,
                               reinterpret_cast<std::uintptr_t>(v));
        }
        if (!name.empty()) {
            if (std::find(out.begin(), out.end(), name) == out.end()) {
                out.push_back(name);
            }
            if (++reports >= 24) {
                return;
            }
            continue;
        }
        if (depth <= 0) {
            continue;
        }
        const auto raw = reinterpret_cast<std::uintptr_t>(v);
        if (raw < 0x10000 || raw >= 0x7fff'ffff'ffffULL || (raw & 7) != 0
            || !memory::isReadable(v, 0x20)) {
            continue;
        }
        findTextureHandles(static_cast<const unsigned char*>(v), 0x120, out, reports, depth - 1);
    }
}

using PackStackOpFn = bool(__fastcall*)(void*, unsigned, void*);
PackStackOpFn g_packStackOp = nullptr;

std::atomic<void*> g_packManager{nullptr};

bool __fastcall detourPackStackOp(void* self, unsigned type, void* fn)
{
    if (self != nullptr) {
        void* expected = nullptr;
        g_packManager.compare_exchange_strong(expected, self, std::memory_order_acq_rel,
                                              std::memory_order_relaxed);
    }
    return g_packStackOp != nullptr ? g_packStackOp(self, type, fn) : false;
}

using TextureLookupFn = void*(__fastcall*)(void*, void*, const void*);
TextureLookupFn g_textureLookup = nullptr;

std::string readCString(const unsigned char* at, std::size_t limit)
{
    if (at == nullptr || !memory::isReadable(at, 8)) {
        return {};
    }
    std::string out;
    for (std::size_t i = 0; i < limit; ++i) {
        if ((i & 0x3f) == 0 && !memory::isReadable(at + i, 0x40)) {
            return {};
        }
        const char c = static_cast<char>(at[i]);
        if (c == '\0') {
            return out;
        }
        if (c < 0x20 || c > 0x7e) {
            return {};
        }
        out.push_back(c);
    }
    return {};
}

std::string sniffResourcePath(const unsigned char* base)
{
    if (base == nullptr || !memory::isReadable(base, 0x30)) {
        return {};
    }
    for (std::size_t off = 0; off <= 0x28; off += 8) {
        std::string text = readStdString(base + off);
        if (looksLikeResourcePath(text)) {
            return text;
        }
        const unsigned char* p = nullptr;
        std::memcpy(&p, base + off, sizeof(p));
        const auto raw = reinterpret_cast<std::uintptr_t>(p);
        if (raw < 0x10000 || raw >= 0x7fff'ffff'ffffULL) {
            continue;
        }
        text = readCString(p, 220);
        if (looksLikeResourcePath(text)) {
            return text;
        }
        if (!memory::isReadable(p, 0x20)) {
            continue;
        }
        text = readStdString(p);
        if (looksLikeResourcePath(text)) {
            return text;
        }
        const unsigned char* q = nullptr;
        std::memcpy(&q, p, sizeof(q));
        text = readCString(q, 220);
        if (looksLikeResourcePath(text)) {
            return text;
        }
    }
    return {};
}

struct LookupSite {
    std::uintptr_t rva = 0;
    unsigned long long calls = 0;
    unsigned long long named = 0;
    std::string sample;
};

std::mutex g_lookupSiteMutex;
std::vector<LookupSite> g_lookupSites;

void* __fastcall detourTextureLookup(void* out, void* group, const void* name)
{
    if (!beModelsOn() && !texDiagOn()) {
        return g_textureLookup != nullptr ? g_textureLookup(out, group, name) : nullptr;
    }
    const void* const key = t_learning;
    std::string path = sniffResourcePath(static_cast<const unsigned char*>(name));
    if (key != nullptr && !path.empty()) {
        noteLearnedTexture(key, path);
    }
    void* const ret = g_textureLookup != nullptr ? g_textureLookup(out, group, name) : nullptr;
    if (!path.empty() && out != nullptr && memory::isReadable(out, 8)) {
        void* handle = nullptr;
        std::memcpy(&handle, out, sizeof(handle));
        noteTextureHandle(handle, path);
    }
    return ret;
}

bool modelLearned(const void* legacy)
{
    if (legacy == nullptr) {
        return false;
    }
    const std::lock_guard<std::mutex> lock{g_modelMutex};
    return g_modelDone.find(legacy) != g_modelDone.end();
}

std::vector<ModelBox> modelBoxesOf(const void* legacy)
{
    if (legacy == nullptr) {
        return {};
    }
    const std::lock_guard<std::mutex> lock{g_modelMutex};
    const auto it = g_modelBoxes.find(legacy);
    return it != g_modelBoxes.end() ? it->second : std::vector<ModelBox>{};
}

bool hasModelBoxes(const void* legacy)
{
    if (legacy == nullptr) {
        return false;
    }
    const std::lock_guard<std::mutex> lock{g_modelMutex};
    const auto it = g_modelBoxes.find(legacy);
    return it != g_modelBoxes.end() && !it->second.empty();
}

void learnModelPart(const void* part)
{
    const void* const key = t_learning;
    if (key == nullptr || part == nullptr || !memory::isReadable(part, 0x1c0)) {
        return;
    }
    const auto* const base = static_cast<const unsigned char*>(part);
    float pivot[3] = {};
    std::memcpy(pivot, base + 0x30, sizeof(pivot));
    const unsigned char* cb = nullptr;
    const unsigned char* ce = nullptr;
    std::memcpy(&cb, base + 0xd0, sizeof(cb));
    std::memcpy(&ce, base + 0xd8, sizeof(ce));
    if (cb == nullptr || ce <= cb || ce - cb > 0x4000) {
        return;
    }
    constexpr std::ptrdiff_t kCubeStride = 240;
    for (std::ptrdiff_t at = 0; at + kCubeStride <= ce - cb; at += kCubeStride) {
        const unsigned char* const cube = cb + at;
        if (!memory::isReadable(cube, 0x50)) {
            break;
        }
        const unsigned char* vb = nullptr;
        const unsigned char* ve = nullptr;
        std::memcpy(&vb, cube + 0x30, sizeof(vb));
        std::memcpy(&ve, cube + 0x38, sizeof(ve));
        if (vb == nullptr || ve <= vb || ve - vb > 0x4000
            || !memory::isReadable(vb, static_cast<std::size_t>(ve - vb))) {
            continue;
        }
        float lo[3] = {1e9F, 1e9F, 1e9F};
        float hi[3] = {-1e9F, -1e9F, -1e9F};
        std::size_t count = 0;
        for (std::ptrdiff_t off = 0; off + 20 <= ve - vb; off += 20) {
            float v[3] = {};
            std::memcpy(v, vb + off, sizeof(v));
            bool ok = true;
            for (const float f : v) {
                if (!std::isfinite(f) || std::fabs(f) > 256.0F) {
                    ok = false;
                }
            }
            if (!ok) {
                continue;
            }
            for (int k = 0; k < 3; ++k) {
                lo[k] = std::min(lo[k], v[k]);
                hi[k] = std::max(hi[k], v[k]);
            }
            ++count;
        }
        if (count < 8) {
            continue;
        }
        ModelBox box;
        box.verts.reserve(static_cast<std::size_t>((ve - vb) / 20));
        for (std::ptrdiff_t off = 0; off + 20 <= ve - vb; off += 20) {
            float raw[5] = {};
            std::memcpy(raw, vb + off, sizeof(raw));
            bool ok = true;
            for (int k = 0; k < 3; ++k) {
                if (!std::isfinite(raw[k]) || std::fabs(raw[k]) > 256.0F) {
                    ok = false;
                }
            }
            if (!ok) {
                continue;
            }
            ModelVertex mv;
            mv.pos[0] = (pivot[0] + raw[0]) / 16.0F;
            mv.pos[1] = (16.0F - (pivot[1] + raw[1])) / 16.0F;
            mv.pos[2] = (pivot[2] + raw[2]) / 16.0F;
            mv.uv[0] = raw[3];
            mv.uv[1] = raw[4];
            box.verts.push_back(mv);
        }
        box.lo[0] = (pivot[0] + lo[0]) / 16.0F;
        box.hi[0] = (pivot[0] + hi[0]) / 16.0F;
        box.lo[2] = (pivot[2] + lo[2]) / 16.0F;
        box.hi[2] = (pivot[2] + hi[2]) / 16.0F;
        box.lo[1] = (16.0F - (pivot[1] + hi[1])) / 16.0F;
        box.hi[1] = (16.0F - (pivot[1] + lo[1])) / 16.0F;
        bool sane = true;
        for (int k = 0; k < 3; ++k) {
            if (!std::isfinite(box.lo[k]) || !std::isfinite(box.hi[k])
                || box.hi[k] - box.lo[k] < 1.0F / 256.0F || box.hi[k] - box.lo[k] > 8.0F) {
                sane = false;
            }
        }
        if (!sane) {
            continue;
        }
        const std::lock_guard<std::mutex> lock{g_modelMutex};
        auto& list = g_modelBoxes[key];
        if (list.size() < 16) {
            list.push_back(box);
        }
    }
}

void normalizeModelBoxes(const void* key)
{
    const std::lock_guard<std::mutex> lock{g_modelMutex};
    const auto it = g_modelBoxes.find(key);
    if (it == g_modelBoxes.end() || it->second.empty()) {
        return;
    }
    std::vector<ModelBox>& list = it->second;
    float lo[3] = {1e9F, 1e9F, 1e9F};
    float hi[3] = {-1e9F, -1e9F, -1e9F};
    for (const ModelBox& box : list) {
        for (int k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], box.lo[k]);
            hi[k] = std::max(hi[k], box.hi[k]);
        }
    }
    bool inside = true;
    for (int k = 0; k < 3; ++k) {
        if (lo[k] < -0.001F || hi[k] > 1.001F) {
            inside = false;
        }
    }
    if (inside) {
        return;
    }
    const float shift[3] = {0.5F - (lo[0] + hi[0]) * 0.5F, -lo[1],
                            0.5F - (lo[2] + hi[2]) * 0.5F};
    float span = 0.0F;
    for (int k = 0; k < 3; ++k) {
        span = std::max(span, hi[k] - lo[k]);
    }
    const float scale = span > 1.0F ? 1.0F / span : 1.0F;
    for (ModelBox& box : list) {
        for (ModelVertex& mv : box.verts) {
            for (int k = 0; k < 3; ++k) {
                mv.pos[k] += shift[k];
            }
            if (scale != 1.0F) {
                mv.pos[0] = 0.5F + (mv.pos[0] - 0.5F) * scale;
                mv.pos[2] = 0.5F + (mv.pos[2] - 0.5F) * scale;
                mv.pos[1] *= scale;
            }
        }
        for (int k = 0; k < 3; ++k) {
            box.lo[k] += shift[k];
            box.hi[k] += shift[k];
        }
        if (scale != 1.0F) {
            box.lo[0] = 0.5F + (box.lo[0] - 0.5F) * scale;
            box.hi[0] = 0.5F + (box.hi[0] - 0.5F) * scale;
            box.lo[2] = 0.5F + (box.lo[2] - 0.5F) * scale;
            box.hi[2] = 0.5F + (box.hi[2] - 0.5F) * scale;
            box.lo[1] *= scale;
            box.hi[1] *= scale;
        }
        for (int k = 0; k < 3; ++k) {
            box.lo[k] = std::clamp(box.lo[k], 0.0F, 1.0F);
            box.hi[k] = std::clamp(box.hi[k], 0.0F, 1.0F);
        }
    }
    list.erase(std::remove_if(list.begin(), list.end(),
                              [](const ModelBox& box) {
                                  for (int k = 0; k < 3; ++k) {
                                      if (box.hi[k] - box.lo[k] < 1.0F / 256.0F) {
                                          return true;
                                      }
                                  }
                                  return false;
                              }),
               list.end());
}

using BeRendererIdFn = unsigned char(__fastcall*)(void*);

void* beRendererFor(void* table, void* actor)
{
    if (table == nullptr || actor == nullptr || !memory::isReadable(actor, 8)
        || !memory::isReadable(table, 8)) {
        return nullptr;
    }
    void* vtable = nullptr;
    std::memcpy(&vtable, actor, sizeof(vtable));
    if (vtable == nullptr || !memory::isReadable(vtable, 0x28)) {
        return nullptr;
    }
    void* fn = nullptr;
    std::memcpy(&fn, static_cast<const unsigned char*>(vtable) + 0x20, sizeof(fn));
    if (fn == nullptr || !memory::isExecutable(fn, 1) || !memory::inGameModule(fn)) {
        return nullptr;
    }
    const unsigned id = reinterpret_cast<BeRendererIdFn>(fn)(actor);
    if (id >= 128) {
        return nullptr;
    }
    auto* const slot = static_cast<unsigned char*>(table) + static_cast<std::size_t>(id) * 8;
    if (!memory::isReadable(slot, 8)) {
        return nullptr;
    }
    void* renderer = nullptr;
    std::memcpy(&renderer, slot, sizeof(renderer));
    return memory::isReadable(renderer, 0x200) ? renderer : nullptr;
}

thread_local void* t_beRenderer = nullptr;

std::mutex g_vtableMutex;
std::vector<const void*> g_vtableSeen;

void findTexturesOnStack(std::vector<std::string>& out, int& reports)
{
    const auto stackBase = static_cast<std::uintptr_t>(__readgsqword(0x08));
    const auto stackLimit = static_cast<std::uintptr_t>(__readgsqword(0x10));
    volatile int here = 0;
    auto at = reinterpret_cast<std::uintptr_t>(&here) & ~static_cast<std::uintptr_t>(7);
    if (stackBase <= stackLimit || at < stackLimit || at >= stackBase) {
        return;
    }
    const std::uintptr_t stop = std::min<std::uintptr_t>(stackBase, at + 0x1800);
    const std::uintptr_t from = at;
    for (int level = 0; level < 3 && reports < 24; ++level) {
    for (at = from; at + 8 <= stop && reports < 24; at += 8) {
        void* v = nullptr;
        std::memcpy(&v, reinterpret_cast<const void*>(at), sizeof(v));
        for (int step = 0; step < level; ++step) {
            const auto raw = reinterpret_cast<std::uintptr_t>(v);
            if (raw < 0x10000 || (raw & 7) != 0 || !memory::isReadable(v, 8)) {
                v = nullptr;
                break;
            }
            void* inner = nullptr;
            std::memcpy(&inner, v, sizeof(inner));
            v = inner;
        }
        if (!looksLikeTexture(v)) {
            continue;
        }
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::memcpy(&width, static_cast<const unsigned char*>(v) + 0x18, sizeof(width));
        std::memcpy(&height, static_cast<const unsigned char*>(v) + 0x1c, sizeof(height));
        const std::string named = textureNameOf(v);
        const std::string label =
            named.empty() ? std::format("<unnamed {}x{} {:#x}>", width, height,
                                        reinterpret_cast<std::uintptr_t>(v))
                          : named;
        if (std::find(out.begin(), out.end(), label) != out.end()) {
            continue;
        }
        out.push_back(label);
        ++reports;
    }
    if (!out.empty()) {
        break;
    }
    }
}

std::mutex g_handleScanMutex;
std::vector<const void*> g_handleScanned;
std::set<const void*> g_handleResolved;

void noteHandlesFromDraw(const void* part, void* model, void* a4)
{
    const void* const key = t_learning;
    bool firstTime = false;
    {
        const std::lock_guard<std::mutex> lock{g_handleScanMutex};
        if (g_handleScanned.size() >= 256) {
            return;
        }
        int seen = 0;
        for (const void* one : g_handleScanned) {
            if (one == key) {
                ++seen;
            }
        }
        if (seen >= 8 || g_handleResolved.count(key) != 0) {
            return;
        }
        firstTime = seen == 0;
        g_handleScanned.push_back(key);
    }
    int reports = 0;
    std::vector<std::string> found;
    {
        std::vector<std::string> onStack;
        int stackReports = 0;
        findTexturesOnStack(onStack, stackReports);
        if (!onStack.empty()) {
            const std::lock_guard<std::mutex> lock{g_handleScanMutex};
            g_handleResolved.insert(key);
        }
        for (const std::string& one : onStack) {
            if (one.rfind("textures/", 0) == 0) {
                atlas::requestEntityImage(one);
            }
        }
        for (std::string& one : onStack) {
            found.push_back(std::move(one));
        }
    }
    if (!firstTime) {
        for (std::string& one : found) {
            noteLearnedTexture(key, std::move(one));
        }
        return;
    }
    findTextureHandles(static_cast<const unsigned char*>(t_beRenderer), 0x1000, found, reports, 1);
    findTextureHandles(static_cast<const unsigned char*>(model), kRendererScan, found, reports, 1);
    findTextureHandles(static_cast<const unsigned char*>(part), 0x200, found, reports, 1);
    findTextureHandles(static_cast<const unsigned char*>(a4), 0x200, found, reports, 1);
    for (int i = 0; i < 12 && reports < 24; ++i) {
        findTextureHandles(static_cast<const unsigned char*>(t_beArgs[i]), 0x200, found, reports,
                           1);
    }
    {
        const auto* const partBase = static_cast<const unsigned char*>(part);
        const auto* const modelBase = static_cast<const unsigned char*>(model);
        std::uint32_t descIndex = 0;
        std::uint32_t bucketIndex = 0;
        if (memory::isReadable(partBase, 0x90) && memory::isReadable(modelBase, 0xf8)) {
            std::memcpy(&descIndex, partBase + 0x84, sizeof(descIndex));
            std::memcpy(&bucketIndex, partBase + 0x8c, sizeof(bucketIndex));
            const unsigned char* p1 = nullptr;
            std::memcpy(&p1, modelBase + 0xf0, sizeof(p1));
            const unsigned char* p2 = nullptr;
            if (p1 != nullptr && memory::isReadable(p1 - 0x10, 8)) {
                std::memcpy(&p2, p1 - 0x10, sizeof(p2));
            }
            const unsigned char* table = nullptr;
            if (p2 != nullptr && memory::isReadable(p2 + 0x88, 8)) {
                std::memcpy(&table, p2 + 0x88, sizeof(table));
            }
            {
                static std::atomic<bool> told{false};
            }
            if (table != nullptr && descIndex < 4096) {
                const unsigned char* const desc = table + static_cast<std::size_t>(descIndex) * 0x60;
                if (memory::isReadable(desc, 0x60)) {
                    findTextureHandles(desc, 0x60, found, reports, 1);
                    const unsigned char* buckets = nullptr;
                    if (memory::isReadable(desc + 0x48, 8)) {
                        std::memcpy(&buckets, desc + 0x48, sizeof(buckets));
                    }
                    if (buckets != nullptr && bucketIndex < 4096) {
                        const unsigned char* const bucket =
                            buckets + static_cast<std::size_t>(bucketIndex) * 0x10;
                        if (memory::isReadable(bucket, 0x10)) {
                            findTextureHandles(bucket, 0x10, found, reports, 1);
                        }
                    }
                }
            }
        }
    }
    for (std::string& one : found) {
        noteLearnedTexture(key, std::move(one));
    }
}

void __fastcall detourModelPart(void* part, void* unused, void* model, void* a4, float scale,
                                unsigned char a6, unsigned a7, void* matrix)
{
    noteRenderer(model);
    learnModelPart(part);
    if (t_learning != nullptr && textureNameCount() > 0) {
        noteHandlesFromDraw(part, model, a4);
    }
    if (g_modelPart != nullptr) {
        g_modelPart(part, unused, model, a4, scale, a6, a7, matrix);
    }
    if (modelDiagOn() && matrix != nullptr && memory::isReadable(matrix, 64)) {
        static std::atomic<int> shots{0};
        if (shots.fetch_add(1, std::memory_order_relaxed) < 12) {
            float m[16] = {};
            std::memcpy(m, matrix, sizeof(m));
            std::wstring row;
            for (const float v : m) {
                row += std::format(L"{:.4g} ", v);
            }
        }
    }
}

constexpr std::uint64_t kAlphaTestHash = 0x0A4F54B100E83415ULL;

thread_local int t_ghostX = 0;

const void* g_alphaBlendName = nullptr;
const void* g_alphaTestName = nullptr;

std::atomic<unsigned> g_swapped{0};

using MaterialLookupFn = void*(__fastcall*)(void*, const void*);
MaterialLookupFn g_materialLookup = nullptr;
void* g_materialRegistry = nullptr;

void* g_materialRegistry2 = nullptr;

void resolveMaterials();
void resolveGhostMaterial();
bool markGhostMaterials();
void unmarkGhostMaterials();
bool ghostMarkMode();
extern std::atomic<bool> g_teardown;

using BeRenderLoopFn = void(__fastcall*)(void*, void*, unsigned int);
BeRenderLoopFn g_beRenderLoop = nullptr;

std::size_t readVectorCount(void* base, std::size_t off)
{
    auto* const at = static_cast<unsigned char*>(base) + off;
    if (!memory::isReadable(at, 16)) {
        return 0;
    }
    void* begin = nullptr;
    void* end = nullptr;
    std::memcpy(&begin, at, sizeof(begin));
    std::memcpy(&end, at + 8, sizeof(end));
    if (begin == nullptr || end == nullptr || end < begin) {
        return 0;
    }
    const std::size_t bytes = static_cast<unsigned char*>(end)
                              - static_cast<unsigned char*>(begin);
    return bytes / 8;
}

void __fastcall detourBeRenderLoop(void* renderer, void* ctx, unsigned int shadow)
{
    void* source = nullptr;
    if (renderer != nullptr) {
        auto* const at = static_cast<unsigned char*>(renderer) + 0x980;
        if (memory::isReadable(at, 8)) {
            std::memcpy(&source, at, sizeof(source));
        }
        blockwrite::noteRenderRegion(source);
    }
    if (g_beRenderLoop != nullptr) {
        ++t_beRenderDepth;
        g_beRenderLoop(renderer, ctx, shadow);
        --t_beRenderDepth;
    }
}

using BeDispatchFn = void(__fastcall*)(void*, void*, void*, void*, void*, void*, void*, void*,
                                       void*, void*, void*, void*);
BeDispatchFn g_beDispatch = nullptr;

thread_local int t_ghostBeDepth = 0;

std::atomic<unsigned long long> g_beCalls{0};
std::atomic<unsigned long long> g_beGhostCells{0};
std::atomic<unsigned long long> g_beMarked{0};
std::atomic<unsigned long long> g_beReportAt{0};

void reportBeDispatch()
{
    const ULONGLONG now = GetTickCount64();
    ULONGLONG last = g_beReportAt.load(std::memory_order_acquire);
    if (last != 0 && now - last < 20000) {
        return;
    }
    if (!g_beReportAt.compare_exchange_strong(last, now, std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
        return;
    }
    static std::atomic<unsigned> told{0};
    if (told.fetch_add(1, std::memory_order_relaxed) >= 24) {
        return;
    }
}

void __fastcall detourBeDispatch(void* a1, void* a2, void* a3, void* a4, void* a5, void* a6,
                                 void* a7, void* a8, void* a9, void* a10, void* a11, void* a12)
{
    bool ghost = false;
    if (beModelsOn() && a7 != nullptr && blocks::ghostOn()
        && memory::isReadable(a7, 12)) {
        const auto* const p = static_cast<const std::int32_t*>(a7);
        ghost = blocks::ghostCell(p[0], p[1], p[2]);
        t_bePos[0] = p[0];
        t_bePos[1] = p[1];
        t_bePos[2] = p[2];
        void* const args[12] = {a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12};
        std::memcpy(t_beArgs, args, sizeof(t_beArgs));
    }
    if (ghost && modelDiagOn()) {
        static std::atomic<int> shots{0};
        if (shots.fetch_add(1, std::memory_order_relaxed) < 4) {
            void* const args[12] = {a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12};
            for (int i = 0; i < 12; ++i) {
                if (args[i] == nullptr || !memory::isReadable(args[i], 0x80)) {
                    continue;
                }
                float f[32] = {};
                std::memcpy(f, args[i], sizeof(f));
                std::wstring row;
                for (const float v : f) {
                    row += (std::isfinite(v) && std::fabs(v) < 4096.0F)
                               ? std::format(L"{:.4g} ", v)
                               : L"* ";
                }
            }
        }
    }
    t_beRenderer = ghost ? beRendererFor(a1, a4) : nullptr;
    const void* learnLegacy = nullptr;
    if (ghost) {
        const void* const key = blocks::ghostBlockAt(t_bePos[0], t_bePos[1], t_bePos[2]);
        if (key != nullptr) {
            if (modelLearned(key)) {
                g_beSkipped.fetch_add(1, std::memory_order_relaxed);
                if (beSkipOn()) {
                    return;
                }
            } else {
                learnLegacy = key;
            }
        }
    }
    g_beCalls.fetch_add(1, std::memory_order_relaxed);
    if (ghost) {
        g_beGhostCells.fetch_add(1, std::memory_order_relaxed);
    }
    resolveMaterials();
    resolveGhostMaterial();

    const bool marked = ghost && ghostMarkMode()
                        && !g_teardown.load(std::memory_order_acquire)
                        && markGhostMaterials();
    if (marked) {
        g_beMarked.fetch_add(1, std::memory_order_relaxed);
    }
    reportBeDispatch();

    const bool doSwap = ghost && g_matGhost != nullptr
                        && !g_teardown.load(std::memory_order_acquire);
    if (doSwap) {
        const std::lock_guard<std::mutex> lock(g_rendererGuard);
        for (auto& entry : g_renderers) {
            if (!entry.scanned) {
                scanRendererSlots(entry);
            }
            for (const std::size_t off : entry.slots) {
                auto* const slot = static_cast<unsigned char*>(entry.who) + off;
                if (memory::isWritable(slot, 8)) {
                    std::memcpy(slot, &g_matGhost, sizeof(g_matGhost));
                }
            }
        }
    }

    if (g_beDispatch != nullptr) {
        if (ghost) {
            ++t_ghostBeDepth;
        }
        t_learning = learnLegacy;
        g_beDispatch(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12);
        t_learning = nullptr;
        if (learnLegacy != nullptr) {
            normalizeModelBoxes(learnLegacy);
            std::size_t boxes = 0;
            {
                const std::lock_guard<std::mutex> lock{g_modelMutex};
                const bool isNew = g_modelDone.find(learnLegacy) == g_modelDone.end();
                g_modelDone[learnLegacy] = true;
                if (isNew) {
                    g_modelLearnedAt.store(GetTickCount64(), std::memory_order_relaxed);
                }
                const auto it = g_modelBoxes.find(learnLegacy);
                boxes = it != g_modelBoxes.end() ? it->second.size() : 0;
            }
            {
                const std::string names = blocks::stateNamesOf(learnLegacy);
            }
            {
                const std::vector<std::string> paths = learnedTextures(learnLegacy);
                std::wstring joined;
                for (const std::string& one : paths) {
                    joined += std::wstring(one.begin(), one.end()) + L" ";
                }
            }
        }
        if (ghost) {
            --t_ghostBeDepth;
        }
    }

    if (doSwap) {
        const std::lock_guard<std::mutex> lock(g_rendererGuard);
        for (const auto& entry : g_renderers) {
            for (const std::size_t off : entry.slots) {
                auto* const slot = static_cast<unsigned char*>(entry.who) + off;
                if (memory::isWritable(slot, 8)) {
                    std::memcpy(slot, &entry.orig, sizeof(entry.orig));
                }
            }
        }
    }

    if (marked) {
        unmarkGhostMaterials();
    }

}

bool g_materialSwapSettled = false;

void installMaterialSwap(HookManager& hooks, bool& created)
{
    if (g_materialSwapSettled) {
        return;
    }
    const Scanner& scanner = Scanner::instance();
    void* const nameSite = scanner.address(Target::AlphaBlendName);
    if (nameSite == nullptr) {
        return;
    }
    g_materialSwapSettled = true;

    if (void* const testSite = scanner.address(Target::AlphaTestName)) {
        const auto* const at = static_cast<const unsigned char*>(testSite);
        std::int32_t disp = 0;
        std::memcpy(&disp, at + 0x0D, sizeof(disp));
        const auto* const target = at + 0x11 + disp;
        if (memory::isReadable(target, 0x28)) {
            std::uint64_t hash = 0;
            std::memcpy(&hash, target, sizeof(hash));
            g_alphaTestName = target;
        }
    }

    {
        const auto* const at = static_cast<const unsigned char*>(nameSite);
        std::int32_t disp = 0;
        std::memcpy(&disp, at + 0x17, sizeof(disp));
        const auto* const target = at + 0x1B + disp;
        if (memory::isReadable(target, 0x28)) {
            std::uint64_t hash = 0;
            std::memcpy(&hash, target, sizeof(hash));
            g_alphaBlendName = target;
        }
    }

}

void* g_matGhost = nullptr;
std::uint64_t g_swapHash = 0;
std::string g_swapName;
bool g_swapRead = false;

std::uint64_t fnv1Hash(std::string_view text)
{
    std::uint64_t h = 0xCBF29CE484222325ULL;
    for (const char c : text) {
        h *= 0x100000001B3ULL;
        h ^= static_cast<unsigned char>(c);
    }
    return h;
}

struct HashedStringBuf {
    std::uint64_t hash;
    void* text;
    std::uint64_t pad[1];
    std::uint64_t size;
    std::uint64_t capacity;
    std::uint64_t extra;
};

char g_nameStore[64]{};

void makeHashedString(std::string_view name, HashedStringBuf& out)
{
    std::memset(&out, 0, sizeof(out));
    out.hash = fnv1Hash(name);
    out.size = name.size();
    if (name.size() <= 15) {
        std::memcpy(&out.text, name.data(), name.size());
        out.capacity = 15;
    } else {
        const std::size_t n = (std::min)(name.size(), sizeof(g_nameStore) - 1);
        std::memcpy(g_nameStore, name.data(), n);
        g_nameStore[n] = 0;
        out.text = g_nameStore;
        out.size = n;
        out.capacity = sizeof(g_nameStore) - 1;
    }
}

void* lookupMaterialByName(std::string_view name)
{
    if (g_materialLookup == nullptr) {
        return nullptr;
    }
    const std::uint64_t hash = fnv1Hash(name);
    alignas(16) HashedStringBuf buf{};
    makeHashedString(name, buf);
    int which = 0;
    void* answer = nullptr;
    for (void* const reg : {g_materialRegistry, g_materialRegistry2}) {
        ++which;
        if (reg == nullptr) {
            continue;
        }
        void* const got = g_materialLookup(reg, &buf);
        bool ok = false;
        std::uint64_t back = 0;
        if (got != nullptr && memory::isReadable(got, 0x20)) {
            void* self = nullptr;
            std::memcpy(&self, got, sizeof(self));
            std::memcpy(&back, static_cast<const unsigned char*>(got) + 0x10, sizeof(back));
            ok = (self == got && back == hash);
        }
        if (ok && answer == nullptr) {
            answer = got;
        }
    }
    return answer;
}

constexpr std::size_t kMatDefName = 0x38;
constexpr std::size_t kMatDefShader = 0xD0;
constexpr std::size_t kMatDefBlend = 0x180;

unsigned char g_blendBytes[6] = {0x00, 0x01, 0x00, 0x01, 0x0F, 0x01};
bool g_blendPatched = false;

std::mutex g_blendGuard;
std::vector<std::pair<unsigned char*, std::array<unsigned char, 6>>> g_blendSaved;

std::atomic<bool> g_teardown{false};

std::string readStdString(const unsigned char* at)
{
    if (!memory::isReadable(at, 0x20)) {
        return {};
    }
    std::uint64_t size = 0;
    std::uint64_t cap = 0;
    std::memcpy(&size, at + 0x10, sizeof(size));
    std::memcpy(&cap, at + 0x18, sizeof(cap));
    if (size == 0 || size > 300 || cap < size) {
        return {};
    }
    const char* text = reinterpret_cast<const char*>(at);
    if (cap > 15) {
        const char* p = nullptr;
        std::memcpy(&p, at, sizeof(p));
        if (!memory::isReadable(p, static_cast<std::size_t>(size))) {
            return {};
        }
        text = p;
    }
    return std::string(text, static_cast<std::size_t>(size));
}

void patchMaterialBlend(const std::string& name);
void readBlendBytes();

void resolveGhostMaterial()
{
    if (g_matGhost != nullptr || g_swapHash == 0
        || g_teardown.load(std::memory_order_acquire)) {
        return;
    }
    void* const got = lookupMaterialByName(g_swapName);
    if (got == nullptr) {
        return;
    }
    g_matGhost = got;
    patchMaterialBlend(g_swapName);
}

template <typename F>
void forEachHeapRegion(F&& visit)
{
    MEMORY_BASIC_INFORMATION mbi{};
    std::uintptr_t addr = 0;
    while (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) == sizeof(mbi)) {
        const auto start = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const std::size_t size = mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && mbi.Protect == PAGE_READWRITE
            && (mbi.Type == MEM_PRIVATE || mbi.Type == MEM_MAPPED)) {
            visit(reinterpret_cast<unsigned char*>(start), size);
        }
        const std::uintptr_t next = start + size;
        if (next <= addr) {
            break;
        }
        addr = next;
    }
}

void patchMaterialBlend(const std::string& name)
{
    if (g_blendPatched || name.empty() || name.size() > 200
        || g_teardown.load(std::memory_order_acquire)) {
        return;
    }
    g_blendPatched = true;

    readBlendBytes();

    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));

    int hit = 0;
    const auto tryPatch = [&](unsigned char* obj) {
        if (!memory::isReadable(obj, kMatDefBlend + 8)) {
            return;
        }
        std::uintptr_t vtable = 0;
        std::memcpy(&vtable, obj, sizeof(vtable));
        if (vtable < base || vtable > base + 0x20000000) {
            return;
        }
        if (readStdString(obj + kMatDefName) != name) {
            return;
        }
        const std::string shader = readStdString(obj + kMatDefShader);
        if (shader.rfind("shaders/", 0) != 0) {
            return;
        }
        unsigned char old[6]{};
        std::memcpy(old, obj + kMatDefBlend, sizeof(old));
        if (std::memcmp(old, g_blendBytes, sizeof(old)) == 0) {
            return;
        }
        {
            const std::lock_guard<std::mutex> lock(g_blendGuard);
            std::array<unsigned char, 6> keep{};
            std::memcpy(keep.data(), old, sizeof(old));
            g_blendSaved.emplace_back(obj + kMatDefBlend, keep);
        }
        std::memcpy(obj + kMatDefBlend, g_blendBytes, sizeof(g_blendBytes));
        ++hit;
    };

    std::vector<unsigned char*> texts;
    forEachHeapRegion([&](unsigned char* start, std::size_t size) {
        if (size < name.size() || g_teardown.load(std::memory_order_acquire)) {
            return;
        }
        const unsigned char first = static_cast<unsigned char>(name[0]);
        for (std::size_t i = 0; i + name.size() <= size; ++i) {
            if (start[i] != first) {
                continue;
            }
            if (std::memcmp(start + i, name.data(), name.size()) != 0) {
                continue;
            }
            if (name.size() <= 15) {
                tryPatch(start + i - kMatDefName);
            }
            if (texts.size() < 256) {
                texts.push_back(start + i);
            }
        }
    });

    if (!texts.empty()) {
        forEachHeapRegion([&](unsigned char* start, std::size_t size) {
            const std::size_t count = size / sizeof(void*);
            auto* const slots = reinterpret_cast<unsigned char**>(start);
            for (std::size_t i = 0; i < count; ++i) {
                unsigned char* const v = slots[i];
                if (v == nullptr) {
                    continue;
                }
                if (std::find(texts.begin(), texts.end(), v) == texts.end()) {
                    continue;
                }
                tryPatch(reinterpret_cast<unsigned char*>(&slots[i]) - kMatDefName);
            }
        });
    }

}

std::mutex g_defIndexGuard;
std::vector<std::pair<std::string, unsigned char*>> g_defIndex;
std::atomic<bool> g_defIndexStarted{false};
std::atomic<bool> g_defIndexReady{false};
std::thread g_defIndexThread;

std::uintptr_t findMatDefVtable(const std::string& name)
{
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    std::uintptr_t answer = 0;

    const auto check = [&](unsigned char* obj) {
        if (answer != 0 || !memory::isReadable(obj, kMatDefBlend + 8)) {
            return;
        }
        std::uintptr_t vtable = 0;
        std::memcpy(&vtable, obj, sizeof(vtable));
        if (vtable < base || vtable > base + 0x20000000) {
            return;
        }
        if (readStdString(obj + kMatDefName) != name) {
            return;
        }
        if (readStdString(obj + kMatDefShader).rfind("shaders/", 0) != 0) {
            return;
        }
        answer = vtable;
    };

    std::vector<unsigned char*> texts;
    forEachHeapRegion([&](unsigned char* start, std::size_t size) {
        if (size < name.size() || g_teardown.load(std::memory_order_acquire)) {
            return;
        }
        const unsigned char first = static_cast<unsigned char>(name[0]);
        for (std::size_t i = 0; i + name.size() <= size; ++i) {
            if (start[i] != first || std::memcmp(start + i, name.data(), name.size()) != 0) {
                continue;
            }
            if (name.size() <= 15) {
                check(start + i - kMatDefName);
            }
            if (texts.size() < 256) {
                texts.push_back(start + i);
            }
        }
    });

    if (answer == 0 && !texts.empty()) {
        forEachHeapRegion([&](unsigned char* start, std::size_t size) {
            const std::size_t count = size / sizeof(void*);
            auto* const slots = reinterpret_cast<unsigned char**>(start);
            for (std::size_t i = 0; i < count; ++i) {
                if (slots[i] == nullptr
                    || std::find(texts.begin(), texts.end(), slots[i]) == texts.end()) {
                    continue;
                }
                check(reinterpret_cast<unsigned char*>(&slots[i]) - kMatDefName);
            }
        });
    }
    return answer;
}

void buildMatDefIndex()
{
    if (g_teardown.load(std::memory_order_acquire)) {
        return;
    }
    const std::uintptr_t vtable = findMatDefVtable("entity_alphablend.skinning");
    if (vtable == 0) {
        log().warn(L"Detours: the vtable of the material definition block was not found");
        return;
    }

    std::vector<std::pair<std::string, unsigned char*>> found;
    forEachHeapRegion([&](unsigned char* start, std::size_t size) {
        if (g_teardown.load(std::memory_order_acquire)) {
            return;
        }
        const std::size_t count = size / sizeof(std::uintptr_t);
        auto* const words = reinterpret_cast<std::uintptr_t*>(start);
        for (std::size_t i = 0; i < count; ++i) {
            if (words[i] != vtable) {
                continue;
            }
            auto* const obj = reinterpret_cast<unsigned char*>(&words[i]);
            if (!memory::isReadable(obj, kMatDefBlend + 8)) {
                continue;
            }
            const std::string name = readStdString(obj + kMatDefName);
            if (name.empty() || name.size() > 120) {
                continue;
            }
            if (readStdString(obj + kMatDefShader).rfind("shaders/", 0) != 0) {
                continue;
            }
            if (found.size() < 4096) {
                found.emplace_back(name, obj);
            }
        }
    });

    {
        const std::lock_guard<std::mutex> lock(g_defIndexGuard);
        g_defIndex = std::move(found);
    }
    g_defIndexReady.store(true, std::memory_order_release);
}

void startMatDefIndex()
{
    if (g_defIndexStarted.exchange(true) || g_teardown.load(std::memory_order_acquire)) {
        return;
    }
    g_defIndexThread = std::thread([] { buildMatDefIndex(); });
}

constexpr unsigned char kMarkWriteMask = 0x0B;

struct MarkSlot {
    unsigned char* at = nullptr;
    std::array<unsigned char, 6> orig{};
};

std::mutex g_markGuard;
std::vector<MarkSlot> g_markSlots;
std::vector<std::string> g_markNames;
bool g_markMode = false;

bool ghostMarkMode()
{
    return g_markMode && ghost::diagBeMode() != 1;
}

std::string readMarkFilter()
{
    std::string want;
    std::error_code ec;
    const std::filesystem::path file = paths::dataDir() / L"diag-mat.txt";
    if (!std::filesystem::exists(file, ec)) {
        return want;
    }
    if (std::ifstream in{file}) {
        std::getline(in, want);
    }
    while (!want.empty() && (want.back() < 0x21)) {
        want.pop_back();
    }
    return want;
}

void ensureMarkSlots()
{
    const std::string filter = readMarkFilter();
    std::vector<std::string> want;
    {
        const std::lock_guard<std::mutex> lock(g_rendererGuard);
        for (const auto& entry : g_renderers) {
            const std::string name = materialName(entry.orig);
            if (!name.empty()) {
                want.push_back(name);
            }
        }
    }
    const std::lock_guard<std::mutex> mark(g_markGuard);
    const std::lock_guard<std::mutex> index(g_defIndexGuard);
    for (const std::string& name : want) {
        if (std::find(g_markNames.begin(), g_markNames.end(), name) != g_markNames.end()) {
            continue;
        }
        g_markNames.push_back(name);
        if (!filter.empty() && name.find(filter) == std::string::npos) {
            continue;
        }
        int hit = 0;
        for (const auto& [have, obj] : g_defIndex) {
            if (have != name && have.rfind(name + "_", 0) != 0) {
                continue;
            }
            MarkSlot slot;
            slot.at = obj + kMatDefBlend;
            if (!memory::isWritable(slot.at, slot.orig.size())) {
                continue;
            }
            std::memcpy(slot.orig.data(), slot.at, slot.orig.size());
            if (slot.orig[4] == kMarkWriteMask) {
                continue;
            }
            g_markSlots.push_back(slot);
            ++hit;
        }
    }
}

void markOne(const MarkSlot& slot)
{
    unsigned char bytes[6];
    std::memcpy(bytes, slot.orig.data(), sizeof(bytes));
    bytes[4] = kMarkWriteMask;
    std::memcpy(slot.at, bytes, sizeof(bytes));
}

bool markGhostMaterials()
{
    if (g_teardown.load(std::memory_order_acquire)) {
        return false;
    }
    startMatDefIndex();
    if (!g_defIndexReady.load(std::memory_order_acquire)) {
        return false;
    }
    ensureMarkSlots();
    const std::lock_guard<std::mutex> lock(g_markGuard);
    if (g_markSlots.empty()) {
        return false;
    }
    ghost::beginMarkWindow();
    for (const MarkSlot& slot : g_markSlots) {
        markOne(slot);
    }
    return true;
}

void unmarkGhostMaterials()
{
    const std::lock_guard<std::mutex> lock(g_markGuard);
    for (const MarkSlot& slot : g_markSlots) {
        std::memcpy(slot.at, slot.orig.data(), slot.orig.size());
    }
    ghost::endMarkWindow();
}

void clearGhostMarks()
{
    if (g_defIndexThread.joinable()) {
        g_defIndexThread.join();
    }
    const std::lock_guard<std::mutex> lock(g_markGuard);
    for (const MarkSlot& slot : g_markSlots) {
        if (memory::isWritable(slot.at, slot.orig.size())) {
            std::memcpy(slot.at, slot.orig.data(), slot.orig.size());
        }
    }
    g_markSlots.clear();
    g_markNames.clear();
}

void readBlendBytes()
{
    const std::filesystem::path file = paths::schematicsDir().parent_path() / L"blend.txt";
    std::string line;
    if (std::ifstream in{file}) {
        std::getline(in, line);
    }
    unsigned v[6]{};
    if (std::sscanf(line.c_str(), "%x %x %x %x %x %x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5])
        == 6) {
        for (int i = 0; i < 6; ++i) {
            g_blendBytes[i] = static_cast<unsigned char>(v[i]);
        }
    }
}

void resolveMaterials()
{
    if (g_swapRead || g_teardown.load(std::memory_order_acquire)
        || g_materialLookup == nullptr || g_materialRegistry == nullptr) {
        return;
    }
    g_swapRead = true;
    const std::filesystem::path file = paths::schematicsDir().parent_path() / L"matswap.txt";
    std::string name;
    if (std::ifstream in{file}) {
        std::getline(in, name);
    }
    while (!name.empty() && (name.back() < 0x21)) {
        name.pop_back();
    }
    ghost::setGhostMark(kMarkWriteMask);

    if (name.empty()) {
        g_markMode = true;
        return;
    }
    if (name == "none") {
        return;
    }
    g_swapName = name;
    g_swapHash = fnv1Hash(name);
}

bool g_lateI18nSettled = false;
bool g_lateBlockGetSettled = false;

}

bool inGhostBeDispatch()
{
    return t_ghostBeDepth > 0;
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

    if (!g_lateBlockGetSettled) {
        bool got = false;
        if (void* const fn = resolveBlockSourceGetBlock(); fn != nullptr) {
            got = hooks.create(fn, &detourBlockSourceGetBlock,
                               reinterpret_cast<void**>(&g_blockSourceGetBlock),
                               L"BlockSourceGetBlock");
            created = got || created;
        }
        if (got) {
            if (void* const extra = resolveBlockSourceGetExtra(); extra != nullptr) {
                created = hooks.create(extra, &detourBlockSourceGetExtra,
                                       reinterpret_cast<void**>(&g_blockSourceGetExtra),
                                       L"BlockSourceGetExtra")
                          || created;
            }
            g_lateBlockGetSettled = true;
        }
    }

    installMaterialSwap(hooks, created);

    if (created) {
        hooks.applyQueued();
    }

    return false;
}

void* materialInfoByName(std::string_view name)
{
    if (g_teardown.load(std::memory_order_acquire)) {
        return nullptr;
    }
    return lookupMaterialByName(name);
}

void restoreMaterialBlend()
{
    g_teardown.store(true, std::memory_order_release);
    clearGhostMarks();
    const std::lock_guard<std::mutex> lock(g_blendGuard);
    for (const auto& [at, keep] : g_blendSaved) {
        if (memory::isWritable(at, keep.size())) {
            std::memcpy(at, keep.data(), keep.size());
        }
    }
    g_blendSaved.clear();
}

using ChunkBuildLookupFn = void*(__fastcall*)(void*, const std::int32_t*);
using ScheduleChunkBuildFn = std::uint64_t(__fastcall*)(void*, void*, void*, void*);

using LevelBuildDispatchFn = void*(__fastcall*)(void*, void*, void*, void*);

ChunkBuildLookupFn g_chunkBuildLookup = nullptr;
ScheduleChunkBuildFn g_scheduleChunkBuild = nullptr;
LevelBuildDispatchFn g_levelBuildDispatch = nullptr;

std::atomic<bool> g_ghostBuildForced{false};
std::atomic<unsigned long long> g_ghostBuildAskedAt{0};
std::atomic<unsigned long long> g_ghostBuildWarnedAt{0};

std::atomic<std::size_t> g_askForcedRounds{0};
std::atomic<std::size_t> g_askRecords{0};
std::atomic<std::size_t> g_askSkippedBuilt{0};
std::atomic<std::size_t> g_askNearTrue{0};
std::atomic<std::size_t> g_askNearFalse{0};
std::atomic<std::size_t> g_askFarTrue{0};
std::atomic<std::size_t> g_askFarFalse{0};
std::atomic<std::size_t> g_askNormalTrue{0};
std::atomic<std::size_t> g_askNormalFalse{0};

__declspec(noinline) bool readCameraPos(std::uint64_t at, float (&out)[3])
{
    if (at == 0) {
        return false;
    }
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(at), sizeof(out));
        return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct BuilderFields {
    std::uint64_t cameraPos = 0;
    void** containerBegin = nullptr;
    void** containerEnd = nullptr;
};

inline constexpr std::ptrdiff_t kContainerStride = 2;

__declspec(noinline) bool readBuilderFields(void* lb, BuilderFields& out)
{
    __try {
        auto* const b = static_cast<std::uint8_t*>(lb);
        out.containerBegin = *reinterpret_cast<void***>(b + 0x130);
        out.containerEnd = *reinterpret_cast<void***>(b + 0x138);
        if (out.containerBegin == nullptr || out.containerEnd == nullptr
            || out.containerEnd < out.containerBegin
            || (out.containerEnd - out.containerBegin) % kContainerStride != 0) {
            return false;
        }
        const std::size_t slots =
            static_cast<std::size_t>(out.containerEnd - out.containerBegin);
        constexpr std::size_t kMaxContainers = 64;
        if (slots / kContainerStride > kMaxContainers
            || !memory::isReadable(out.containerBegin, slots * sizeof(void*))) {
            return false;
        }
        auto* const a = *reinterpret_cast<std::uint8_t**>(b + 0x128);
        if (a == nullptr) {
            return false;
        }
        auto* const camera = *reinterpret_cast<std::uint8_t**>(a + 0x468);
        if (camera == nullptr) {
            return false;
        }
        out.cameraPos = reinterpret_cast<std::uint64_t>(camera + 0x9cc);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

__declspec(noinline) void* readLevelBuilder(void* params)
{
    __try {
        return *reinterpret_cast<void**>(static_cast<std::uint8_t*>(params) + 8);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

std::atomic<std::size_t> g_containerRefused{0};
std::atomic<bool> g_containerRefusedTold{false};

__declspec(noinline) bool vtableLooksSane(void* object)
{
    if (object == nullptr) {
        return false;
    }
    __try {
        return memory::inGameModule(*reinterpret_cast<void**>(object));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::atomic<int> g_containerWhy{0};
std::atomic<std::int32_t> g_containerCount{0};
std::atomic<std::int32_t> g_containerLow{0};
std::atomic<std::int32_t> g_containerHigh{0};

__declspec(noinline) bool containerLooksSane(void* container)
{
    if (container == nullptr) {
        return false;
    }
    __try {
        auto* const c = static_cast<std::uint8_t*>(container);
        void* const vtable = *reinterpret_cast<void**>(c);
        if (!memory::inGameModule(vtable)) {
            g_containerWhy.store(1, std::memory_order_relaxed);
            return false;
        }
        auto* const inner = *reinterpret_cast<std::uint8_t**>(c + 0x5f0);
        if (inner == nullptr) {
            return true;
        }
        if (!memory::isReadable(inner, 0xf0)) {
            g_containerWhy.store(2, std::memory_order_relaxed);
            return false;
        }
        std::uint64_t lock = 0;
        std::memcpy(&lock, inner + 0xc0, sizeof(lock));
        if (lock == 0xFFFFFFFFFFFFFFFFULL) {
            g_containerWhy.store(3, std::memory_order_relaxed);
            return false;
        }
        std::int32_t count = 0;
        std::int32_t low = 0;
        std::int32_t high = 0;
        std::memcpy(&count, inner + 0xec, sizeof(count));
        std::memcpy(&low, inner + 0xc8, sizeof(low));
        std::memcpy(&high, inner + 0xd4, sizeof(high));
        constexpr std::int32_t kChunkLimit = 4000000;
        const bool ok = count >= 0 && count <= 0x100000 && low <= high
                        && low >= -kChunkLimit && high <= kChunkLimit;
        if (!ok) {
            g_containerWhy.store(4, std::memory_order_relaxed);
            g_containerCount.store(count, std::memory_order_relaxed);
            g_containerLow.store(low, std::memory_order_relaxed);
            g_containerHigh.store(high, std::memory_order_relaxed);
        }
        return ok;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void noteContainerRefused()
{
    const std::size_t n = g_containerRefused.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1 && !g_containerRefusedTold.exchange(true, std::memory_order_relaxed)) {
        static const wchar_t* const kWhy[] = {L"?", L"the vtable is outside the game",
                                              L"what +0x5f0 points at cannot be read",
                                              L"the lock is -1", L"the fields look insane"};
        const int why = g_containerWhy.load(std::memory_order_relaxed);
        log().warn(L"Schematica: the chunk record owner looked broken, so it was not called "
                   L"(reason {} = {} / count {} / range {}..{}; further ones are only counted)",
                   why,
                   kWhy[(why >= 0 && why <= 4) ? why : 0],
                   g_containerCount.load(std::memory_order_relaxed),
                   g_containerLow.load(std::memory_order_relaxed),
                   g_containerHigh.load(std::memory_order_relaxed));
    }
}

__declspec(noinline) void* callChunkBuildLookup(void* container, const std::int32_t at[3])
{
    if (!containerLooksSane(container)) {
        noteContainerRefused();
        return nullptr;
    }
    __try {
        return g_chunkBuildLookup(container, at);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

__declspec(noinline) std::uint64_t callScheduleChunkBuild(void* lb, void* rec,
                                                         void* camPos, void* container)
{
    if (!containerLooksSane(container)) {
        return 0;
    }
    __try {
        return g_scheduleChunkBuild(lb, rec, camPos, container);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

__declspec(noinline) bool sharedIsReadable(void* rec)
{
    __try {
        void* shared = nullptr;
        std::memcpy(&shared, static_cast<std::uint8_t*>(rec) + 0x10, sizeof(shared));
        return shared != nullptr && memory::isReadable(shared, 0x40);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

__declspec(noinline) bool readScheduleGate(void* rec, void* lb, std::uint64_t (&out)[9])
{
    __try {
        auto* const r = static_cast<std::uint8_t*>(rec);
        auto* const shared = *reinterpret_cast<std::uint8_t**>(r + 0x10);
        out[0] = reinterpret_cast<std::uint64_t>(shared);
        out[7] = r[9];
        out[8] = 0;
        if (lb != nullptr) {
            auto* const inner = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(lb)
                                                                   + 0x128);
            if (inner != nullptr) {
                out[8] = *reinterpret_cast<std::uint64_t*>(inner + 0x380);
            }
        }
        if (shared == nullptr) {
            out[1] = out[2] = out[3] = out[4] = out[5] = out[6] = 0;
            return true;
        }
        out[1] = *reinterpret_cast<std::uint32_t*>(shared + 4);
        auto* const job = *reinterpret_cast<std::uint8_t**>(shared + 0x28);
        out[2] = reinterpret_cast<std::uint64_t>(job);
        out[3] = job != nullptr ? *reinterpret_cast<std::uint64_t*>(job) : 0;
        out[4] = job != nullptr ? *reinterpret_cast<std::uint64_t*>(job + 8) : 0;
        out[5] = job != nullptr ? (job[0x28] & 0x20) : 0;
        out[6] = shared[0];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

__declspec(noinline) bool resetEmptyJobStampRaw(std::uint8_t* job)
{
    __try {
        auto* const stamp = reinterpret_cast<volatile LONG64*>(job);
        if (stamp[1] != -1) {
            return false;
        }
        return InterlockedCompareExchange64(stamp, 0, -1) == -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

__declspec(noinline) std::uint8_t* readJobPointer(void* rec)
{
    __try {
        auto* const r = static_cast<std::uint8_t*>(rec);
        auto* const shared = *reinterpret_cast<std::uint8_t**>(r + 0x10);
        if (shared == nullptr) {
            return nullptr;
        }
        return *reinterpret_cast<std::uint8_t**>(shared + 0x28);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool resetEmptyJobStamp(void* rec)
{
    std::uint8_t* const job = readJobPointer(rec);
    if (job == nullptr || !memory::isWritable(job, 16)) {
        return false;
    }
    return resetEmptyJobStampRaw(job);
}

__declspec(noinline) bool recordHasGeometry(void* rec)
{
    __try {
        void* held = nullptr;
        std::memcpy(&held, static_cast<std::uint8_t*>(rec) + 0x20, sizeof(held));
        return held != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

__declspec(noinline) bool clearSharedEmptyFlag(void* rec)
{
    __try {
        auto* const r = static_cast<std::uint8_t*>(rec);
        auto* const shared = *reinterpret_cast<std::uint8_t**>(r + 0x10);
        if (shared == nullptr) {
            return false;
        }
        *reinterpret_cast<volatile std::uint8_t*>(shared) = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

__declspec(noinline) bool askVisibilityRebuild(void* rec)
{
    void* const coordinator = g_visCoordinator.load(std::memory_order_relaxed);
    if (coordinator == nullptr || rec == nullptr || g_visibilityGate == nullptr) {
        return false;
    }
    if (!vtableLooksSane(coordinator)) {
        noteContainerRefused();
        return false;
    }
    __try {
        detourVisibilityGate(coordinator, static_cast<std::uint8_t*>(rec) + 0x10);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void askGhostChunkBuilds(void* params)
{
    const perf::Scope perfScope{perf::Slot::AskBuilds};
    if (g_chunkBuildLookup == nullptr || g_scheduleChunkBuild == nullptr ||
        params == nullptr) {
        return;
    }
    void* const lb = readLevelBuilder(params);
    if (lb == nullptr) {
        return;
    }
    BuilderFields fields;
    if (!readBuilderFields(lb, fields) || fields.cameraPos == 0 ||
        fields.containerBegin == nullptr || fields.containerEnd == nullptr) {
        return;
    }
    std::int32_t low[3] = {0, 0, 0};
    std::int32_t high[3] = {0, 0, 0};
    bool afterOff = false;
    if (!blocks::ghostBounds(low, high)) {
        if (!blocks::lastGhostBounds(low, high)) {
            return;
        }
        afterOff = true;
    }
    const bool everything = g_ghostBuildForced.exchange(false, std::memory_order_relaxed)
                            || afterOff;
    std::size_t wanted = 0;
    std::size_t asked = 0;
    float camera[3] = {0.0F, 0.0F, 0.0F};
    const bool haveCamera = readCameraPos(fields.cameraPos, camera);
    if (everything) {
        g_askForcedRounds.fetch_add(1, std::memory_order_relaxed);
    }
    for (std::int32_t cy = low[1] >> 4; cy <= (high[1] >> 4); ++cy) {
        for (std::int32_t cx = low[0] >> 4; cx <= (high[0] >> 4); ++cx) {
            for (std::int32_t cz = low[2] >> 4; cz <= (high[2] >> 4); ++cz) {
                if (!afterOff
                    && !(blocks::meshBoxesOn()
                             ? blocks::ghostBoxTouchesSubChunk(cx << 4, cy << 4, cz << 4)
                             : blocks::ghostSubChunkOccupied(cx << 4, cy << 4, cz << 4))) {
                    continue;
                }
                ++wanted;
                const std::int32_t at[3] = {cx, cy, cz};
                for (void** it = fields.containerBegin; it < fields.containerEnd; it += kContainerStride) {
                    void* const rec = callChunkBuildLookup(*it, at);
                    if (rec == nullptr) {
                        continue;
                    }
                    if (!memory::isReadable(rec, 0x40) || !sharedIsReadable(rec)) {
                        continue;
                    }
                    ++asked;
                    g_askRecords.fetch_add(1, std::memory_order_relaxed);
                    if (!everything && recordHasGeometry(rec)) {
                        g_askSkippedBuilt.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }
                    clearSharedEmptyFlag(rec);
                    askVisibilityRebuild(rec);
                    const std::uint64_t scheduled = callScheduleChunkBuild(
                        lb, rec, reinterpret_cast<void*>(fields.cameraPos), *it);
                    bool isNear = false;
                    if (haveCamera) {
                        const float dx = static_cast<float>((cx << 4) + 8) - camera[0];
                        const float dy = static_cast<float>((cy << 4) + 8) - camera[1];
                        const float dz = static_cast<float>((cz << 4) + 8) - camera[2];
                        isNear = dx * dx + dy * dy + dz * dz <= 24.0F * 24.0F;
                    }
                    const bool ok = (scheduled & 0xFF) != 0;
                    if (everything) {
                        (isNear ? (ok ? g_askNearTrue : g_askNearFalse)
                                : (ok ? g_askFarTrue : g_askFarFalse))
                            .fetch_add(1, std::memory_order_relaxed);
                    } else {
                        (ok ? g_askNormalTrue : g_askNormalFalse)
                            .fetch_add(1, std::memory_order_relaxed);
                    }
                    break;
                }
            }
        }
    }
    if (afterOff) {
        static std::atomic<int> shots{0};
    }

    if (wanted != 0 && asked == 0) {
        const unsigned long long now = GetTickCount64();
        unsigned long long was = g_ghostBuildWarnedAt.load(std::memory_order_relaxed);
        if (now - was >= 30000 &&
            g_ghostBuildWarnedAt.compare_exchange_strong(was, now)) {
            log().warn(L"Schematica: could not ask for {} chunk(s) to be rebuilt (their "
                       L"records cannot be read)",
                       wanted);
        }
    }
}

thread_local void* t_dispatchParams = nullptr;

std::atomic<std::uint64_t> g_buildSeq{0};

std::uint64_t nextBuildSeq()
{
    return g_buildSeq.fetch_add(1, std::memory_order_seq_cst) + 1;
}

struct IncomingRebuild {
    std::int32_t c[3] = {0, 0, 0};
    std::uint64_t seq = 0;
};
struct PendingRebuild {
    std::int32_t c[3] = {0, 0, 0};
    std::uint64_t sinceSeq = 0;
    unsigned long long since = 0;
    unsigned long long firstTry = 0;
    unsigned long long lastTry = 0;
    std::uint32_t tries = 0;
};
std::mutex g_rebuildMutex;
std::vector<IncomingRebuild> g_rebuildIncoming;
std::atomic<bool> g_rebuildHasIncoming{false};
std::atomic<bool> g_rebuildClear{false};
std::vector<PendingRebuild> g_rebuildPending;
struct BuiltChunk {
    std::int32_t c[3] = {0, 0, 0};
    std::uint64_t startSeq = 0;
};
std::mutex g_builtMutex;
std::vector<BuiltChunk> g_builtIncoming;
std::atomic<bool> g_rebuildWatching{false};
std::atomic<std::size_t> g_rbQueued{0};
std::atomic<std::size_t> g_rbBuilt{0};
std::atomic<std::size_t> g_rbTried{0};
std::atomic<std::size_t> g_rbNoRecord{0};
std::atomic<std::size_t> g_rbGaveUp{0};
std::atomic<std::size_t> g_rbPendingNow{0};
std::atomic<std::size_t> g_rbSchedTrue{0};
std::atomic<std::size_t> g_rbSchedFalse{0};
std::atomic<std::size_t> g_rbJobReset{0};
constexpr std::size_t kRebuildPerFrame = 8;
constexpr unsigned long long kRebuildGapMs = 500;
constexpr unsigned long long kRebuildGiveUpMs = 15000;
constexpr unsigned long long kRebuildNeverTriedMs = 60000;
constexpr std::size_t kRebuildPendingMax = 8192;

std::uint64_t rebuildKey(const std::int32_t c[3]);

struct ChunkBoxTries {
    std::uint32_t tries = 0;
    std::uint32_t stacked = 0;
    std::uint64_t startSeq = 0;
};
std::mutex g_chunkBoxTriesMutex;
std::unordered_map<std::uint64_t, ChunkBoxTries> g_chunkBoxTries;
std::atomic<std::size_t> g_buildsOutOfOrder{0};

void noteChunkBoxTries(const void* rec, std::uint32_t tries, std::uint32_t stacked,
                       std::uint64_t buildSeq)
{
    if (rec == nullptr || !blocks::ghostOn()) {
        return;
    }
    std::int32_t org[3] = {};
    std::uint32_t unused = 0;
    if (!readChunkOrigin(rec, nullptr, org, &unused)
        || !blocks::ghostBoxTouchesSubChunk(org[0], org[1], org[2])) {
        return;
    }
    const std::int32_t c[3] = {org[0] >> 4, org[1] >> 4, org[2] >> 4};
    std::lock_guard<std::mutex> lock(g_chunkBoxTriesMutex);
    if (g_chunkBoxTries.size() > 65536) {
        g_chunkBoxTries.clear();
    }
    ChunkBoxTries& one = g_chunkBoxTries[rebuildKey(c)];
    if (buildSeq < one.startSeq) {
        g_buildsOutOfOrder.fetch_add(1, std::memory_order_relaxed);
    }
    one.tries = tries;
    one.stacked = stacked;
    one.startSeq = buildSeq;
}

void noteChunkBuiltForRebuilds(const void* rec, std::uint64_t buildSeq)
{
    if (!g_rebuildWatching.load(std::memory_order_acquire) || rec == nullptr) {
        return;
    }
    std::int32_t org[3] = {};
    std::uint32_t unused = 0;
    if (!readChunkOrigin(rec, nullptr, org, &unused)) {
        return;
    }
    if (!blocks::ghostBoxTouchesSubChunk(org[0], org[1], org[2])) {
        return;
    }
    BuiltChunk one;
    one.c[0] = org[0] >> 4;
    one.c[1] = org[1] >> 4;
    one.c[2] = org[2] >> 4;
    one.startSeq = buildSeq;
    std::lock_guard<std::mutex> lock(g_builtMutex);
    if (g_builtIncoming.size() < kRebuildPendingMax) {
        g_builtIncoming.push_back(one);
    }
}

std::uint64_t rebuildKey(const std::int32_t c[3])
{
    const std::uint64_t ux = static_cast<std::uint32_t>(c[0]) & 0x1FFFFFu;
    const std::uint64_t uy = static_cast<std::uint32_t>(c[1]) & 0x1FFFFFu;
    const std::uint64_t uz = static_cast<std::uint32_t>(c[2]) & 0x1FFFFFu;
    return (ux << 42) | (uy << 21) | uz;
}

void processChunkRebuilds(void* params)
{
    if (g_rebuildClear.exchange(false, std::memory_order_acq_rel)) {
        g_rebuildPending.clear();
        {
            std::lock_guard<std::mutex> lock(g_rebuildMutex);
            g_rebuildIncoming.clear();
            g_rebuildHasIncoming.store(false, std::memory_order_relaxed);
        }
        std::lock_guard<std::mutex> lock(g_builtMutex);
        g_builtIncoming.clear();
    }
    const unsigned long long now = GetTickCount64();
    if (g_rebuildHasIncoming.exchange(false, std::memory_order_acq_rel)) {
        std::vector<IncomingRebuild> incoming;
        {
            std::lock_guard<std::mutex> lock(g_rebuildMutex);
            incoming.swap(g_rebuildIncoming);
        }
        std::unordered_map<std::uint64_t, std::size_t> at;
        at.reserve(g_rebuildPending.size() + incoming.size());
        for (std::size_t i = 0; i < g_rebuildPending.size(); ++i) {
            at.emplace(rebuildKey(g_rebuildPending[i].c), i);
        }
        for (const IncomingRebuild& in : incoming) {
            const auto found = at.find(rebuildKey(in.c));
            if (found != at.end()) {
                PendingRebuild& again = g_rebuildPending[found->second];
                again.sinceSeq = std::max(again.sinceSeq, in.seq);
                again.since = now;
                again.firstTry = 0;
                again.lastTry = 0;
                again.tries = 0;
                continue;
            }
            if (g_rebuildPending.size() >= kRebuildPendingMax) {
                g_rbGaveUp.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            PendingRebuild one;
            one.c[0] = in.c[0];
            one.c[1] = in.c[1];
            one.c[2] = in.c[2];
            one.sinceSeq = in.seq;
            one.since = now;
            at.emplace(rebuildKey(in.c), g_rebuildPending.size());
            g_rebuildPending.push_back(one);
        }
    }
    {
        std::vector<BuiltChunk> built;
        {
            std::lock_guard<std::mutex> lock(g_builtMutex);
            built.swap(g_builtIncoming);
        }
        if (!built.empty() && !g_rebuildPending.empty()) {
            std::unordered_map<std::uint64_t, std::uint64_t> latest;
            latest.reserve(built.size());
            for (const BuiltChunk& one : built) {
                auto& slot = latest[rebuildKey(one.c)];
                slot = std::max(slot, one.startSeq);
            }
            std::size_t keep = 0;
            for (std::size_t i = 0; i < g_rebuildPending.size(); ++i) {
                const auto found = latest.find(rebuildKey(g_rebuildPending[i].c));
                if (found != latest.end() && found->second > g_rebuildPending[i].sinceSeq) {
                    g_rbBuilt.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                g_rebuildPending[keep++] = g_rebuildPending[i];
            }
            g_rebuildPending.resize(keep);
        }
    }
    {
        std::size_t keep = 0;
        for (std::size_t i = 0; i < g_rebuildPending.size(); ++i) {
            const PendingRebuild& one = g_rebuildPending[i];
            const bool triedLongEnough =
                one.firstTry != 0 && now - one.firstTry >= kRebuildGiveUpMs;
            const bool waitedTooLong = now - one.since >= kRebuildNeverTriedMs;
            if (triedLongEnough || waitedTooLong) {
                g_rbGaveUp.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            g_rebuildPending[keep++] = g_rebuildPending[i];
        }
        g_rebuildPending.resize(keep);
    }
    g_rbPendingNow.store(g_rebuildPending.size(), std::memory_order_relaxed);
    g_rebuildWatching.store(!g_rebuildPending.empty(), std::memory_order_release);
    if (g_rebuildPending.empty()) {
        return;
    }
    if (!blocks::ghostOn() || g_chunkBuildLookup == nullptr || g_scheduleChunkBuild == nullptr
        || params == nullptr) {
        g_rebuildPending.clear();
        g_rbPendingNow.store(0, std::memory_order_relaxed);
        g_rebuildWatching.store(false, std::memory_order_release);
        return;
    }
    void* const lb = readLevelBuilder(params);
    if (lb == nullptr) {
        return;
    }
    BuilderFields fields;
    if (!readBuilderFields(lb, fields) || fields.cameraPos == 0 || fields.containerBegin == nullptr
        || fields.containerEnd == nullptr) {
        return;
    }
    float camera[3] = {0.0F, 0.0F, 0.0F};
    const bool haveCamera = readCameraPos(fields.cameraPos, camera);
    const auto distance2 = [&](const PendingRebuild& one) {
        if (!haveCamera) {
            return 0.0F;
        }
        const float dx = static_cast<float>((one.c[0] << 4) + 8) - camera[0];
        const float dy = static_cast<float>((one.c[1] << 4) + 8) - camera[1];
        const float dz = static_cast<float>((one.c[2] << 4) + 8) - camera[2];
        return dx * dx + dy * dy + dz * dz;
    };
    const auto ready = [&](const PendingRebuild& one) {
        return one.lastTry == 0 || now - one.lastTry >= kRebuildGapMs;
    };
    const auto split = std::partition(g_rebuildPending.begin(), g_rebuildPending.end(), ready);
    const std::size_t readyCount = static_cast<std::size_t>(split - g_rebuildPending.begin());
    const std::size_t take = std::min(kRebuildPerFrame, readyCount);
    if (take == 0) {
        return;
    }
    std::partial_sort(g_rebuildPending.begin(),
                      g_rebuildPending.begin() + static_cast<std::ptrdiff_t>(take), split,
                      [&](const PendingRebuild& a, const PendingRebuild& b) {
                          if (a.lastTry != b.lastTry) {
                              return a.lastTry < b.lastTry;
                          }
                          return distance2(a) < distance2(b);
                      });
    for (std::size_t i = 0; i < take; ++i) {
        PendingRebuild& one = g_rebuildPending[i];
        one.lastTry = now;
        void* rec = nullptr;
        void* owner = nullptr;
        for (void** it = fields.containerBegin; it < fields.containerEnd; it += kContainerStride) {
            void* const got = callChunkBuildLookup(*it, one.c);
            if (got != nullptr) {
                rec = got;
                owner = *it;
                break;
            }
        }
        if (rec == nullptr || !memory::isReadable(rec, 0x40) || !sharedIsReadable(rec)) {
            g_rbNoRecord.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        clearSharedEmptyFlag(rec);
        askVisibilityRebuild(rec);
        float center[3] = {static_cast<float>((one.c[0] << 4) + 8),
                           static_cast<float>((one.c[1] << 4) + 8),
                           static_cast<float>((one.c[2] << 4) + 8)};
        std::uint64_t gate[9] = {};
        const bool gateRead = readScheduleGate(rec, lb, gate);
        if (gateRead && gate[2] != 0 && gate[3] == ~std::uint64_t{0} && resetEmptyJobStamp(rec)) {
            g_rbJobReset.fetch_add(1, std::memory_order_relaxed);
        }
        const std::uint64_t scheduled = callScheduleChunkBuild(lb, rec, center, owner);
        const bool ok = (scheduled & 0xFF) != 0;
        (ok ? g_rbSchedTrue : g_rbSchedFalse).fetch_add(1, std::memory_order_relaxed);
        if (one.tries == 0) {
            one.firstTry = now;
        }
        ++one.tries;
        g_rbTried.fetch_add(1, std::memory_order_relaxed);
    }
}

void* __fastcall detourLevelBuildDispatch(void* ret, void* params, void* third,
                                          void* fourth)
{
    t_dispatchParams = params;
    if (blocks::ghostOn()) {
        Schematica::instance().earlyLearnFrame();
    }
    std::int32_t lastLow[3] = {0, 0, 0};
    std::int32_t lastHigh[3] = {0, 0, 0};
    if (blocks::ghostOn() || blocks::lastGhostBounds(lastLow, lastHigh)) {
        const unsigned long long now = GetTickCount64();
        unsigned long long was = g_ghostBuildAskedAt.load(std::memory_order_relaxed);
        if (now - was >= 1000 &&
            g_ghostBuildAskedAt.compare_exchange_strong(was, now)) {
            askGhostChunkBuilds(params);
        }
    }
    processChunkRebuilds(params);
    void* const result =
        (g_levelBuildDispatch != nullptr) ? g_levelBuildDispatch(ret, params, third, fourth) : ret;
    if (blocks::ghostOn()) {
        Schematica::instance().earlyLearnFrame();
    }
    t_dispatchParams = nullptr;
    return result;
}

bool chunkLastBuildBoxTries(std::int32_t cx, std::int32_t cy, std::int32_t cz,
                            std::uint32_t& tries, std::uint64_t* startSeq,
                            std::uint32_t* stacked)
{
    const std::int32_t c[3] = {cx, cy, cz};
    std::lock_guard<std::mutex> lock(g_chunkBoxTriesMutex);
    const auto found = g_chunkBoxTries.find(rebuildKey(c));
    if (found == g_chunkBoxTries.end()) {
        return false;
    }
    tries = found->second.tries;
    if (startSeq != nullptr) {
        *startSeq = found->second.startSeq;
    }
    if (stacked != nullptr) {
        *stacked = found->second.stacked;
    }
    return true;
}

std::size_t buildsOutOfOrderCount()
{
    return g_buildsOutOfOrder.exchange(0, std::memory_order_relaxed);
}

void clearChunkBoxTries()
{
    std::lock_guard<std::mutex> lock(g_chunkBoxTriesMutex);
    g_chunkBoxTries.clear();
}

bool chunkHasGeometryNow(std::int32_t cx, std::int32_t cy, std::int32_t cz)
{
    void* const params = t_dispatchParams;
    if (params == nullptr || g_chunkBuildLookup == nullptr) {
        return false;
    }
    void* const lb = readLevelBuilder(params);
    if (lb == nullptr) {
        return false;
    }
    BuilderFields fields;
    if (!readBuilderFields(lb, fields) || fields.containerBegin == nullptr
        || fields.containerEnd == nullptr) {
        return false;
    }
    const std::int32_t at[3] = {cx, cy, cz};
    for (void** it = fields.containerBegin; it < fields.containerEnd; it += kContainerStride) {
        void* const rec = callChunkBuildLookup(*it, at);
        if (rec == nullptr) {
            continue;
        }
        if (!memory::isReadable(rec, 0x40)) {
            return false;
        }
        return recordHasGeometry(rec);
    }
    return false;
}

void requestGhostChunkBuild()
{
    g_ghostBuildForced.store(true, std::memory_order_relaxed);
}

void requestChunkRebuilds(const std::vector<std::array<std::int32_t, 3>>& chunks)
{
    if (chunks.empty()) {
        return;
    }
    const std::uint64_t seq = nextBuildSeq();
    std::lock_guard<std::mutex> lock(g_rebuildMutex);
    for (const auto& one : chunks) {
        if (g_rebuildIncoming.size() >= kRebuildPendingMax) {
            g_rbGaveUp.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        IncomingRebuild in;
        in.c[0] = one[0];
        in.c[1] = one[1];
        in.c[2] = one[2];
        in.seq = seq;
        g_rebuildIncoming.push_back(in);
    }
    g_rbQueued.fetch_add(chunks.size(), std::memory_order_relaxed);
    g_rebuildHasIncoming.store(true, std::memory_order_release);
}

void clearChunkRebuilds()
{
    g_rebuildClear.store(true, std::memory_order_release);
}

void chunkRebuildStats(std::size_t (&out)[9])
{
    out[8] = g_rbJobReset.exchange(0, std::memory_order_relaxed);
    out[0] = g_rbQueued.exchange(0, std::memory_order_relaxed);
    out[1] = g_rbBuilt.exchange(0, std::memory_order_relaxed);
    out[2] = g_rbTried.exchange(0, std::memory_order_relaxed);
    out[3] = g_rbNoRecord.exchange(0, std::memory_order_relaxed);
    out[4] = g_rbGaveUp.exchange(0, std::memory_order_relaxed);
    out[5] = g_rbPendingNow.load(std::memory_order_relaxed);
    out[6] = g_rbSchedTrue.exchange(0, std::memory_order_relaxed);
    out[7] = g_rbSchedFalse.exchange(0, std::memory_order_relaxed);
}

void boxBucketStats(std::size_t& pushDiff, std::size_t& ok, std::size_t& skipped,
                    std::uint32_t& lastLayer)
{
    pushDiff = g_boxPushDiff.load(std::memory_order_relaxed);
    ok = g_boxBucketOk.load(std::memory_order_relaxed);
    skipped = g_boxBucketSkip.load(std::memory_order_relaxed);
    lastLayer = g_boxBucketLastLayer.load(std::memory_order_relaxed);
}

bool boxCoverageStats(std::int32_t& minX, std::int32_t& maxX, std::int32_t& minZ,
                      std::int32_t& maxZ)
{
    minX = g_boxMinX.exchange(INT32_MAX, std::memory_order_relaxed);
    maxX = g_boxMaxX.exchange(INT32_MIN, std::memory_order_relaxed);
    minZ = g_boxMinZ.exchange(INT32_MAX, std::memory_order_relaxed);
    maxZ = g_boxMaxZ.exchange(INT32_MIN, std::memory_order_relaxed);
    return minX <= maxX;
}

void askBuildStats(std::size_t (&out)[9])
{
    out[0] = g_askForcedRounds.exchange(0, std::memory_order_relaxed);
    out[1] = g_askRecords.exchange(0, std::memory_order_relaxed);
    out[2] = g_askSkippedBuilt.exchange(0, std::memory_order_relaxed);
    out[3] = g_askNearTrue.exchange(0, std::memory_order_relaxed);
    out[4] = g_askNearFalse.exchange(0, std::memory_order_relaxed);
    out[5] = g_askFarTrue.exchange(0, std::memory_order_relaxed);
    out[6] = g_askFarFalse.exchange(0, std::memory_order_relaxed);
    out[7] = g_askNormalTrue.exchange(0, std::memory_order_relaxed);
    out[8] = g_askNormalFalse.exchange(0, std::memory_order_relaxed);
}

void overlayStats(std::size_t& stacked, std::size_t& missed)
{
    stacked = g_overlayStacked.exchange(0, std::memory_order_relaxed);
    missed = g_overlayMissed.exchange(0, std::memory_order_relaxed);
}

void boxCullStats(std::size_t& getBlock, std::size_t& getExtra, std::size_t& lookup)
{
    getBlock = g_boxAskGetBlock.load(std::memory_order_relaxed);
    getExtra = g_boxAskGetExtra.load(std::memory_order_relaxed);
    lookup = g_boxAskLookup.load(std::memory_order_relaxed);
}

void uvPaintStats(std::size_t& painted, std::size_t& missed)
{
    painted = g_uvPainted.load(std::memory_order_relaxed);
    missed = g_uvMissed.load(std::memory_order_relaxed);
}

void boxShapeStats(std::size_t& verts)
{
    verts = g_boxVerts.load(std::memory_order_relaxed);
}

bool modelBoxedForBlock(const void* block)
{
    return !modelBoxesOf(block).empty();
}

unsigned long long lastModelLearnedAt()
{
    return g_modelLearnedAt.load(std::memory_order_relaxed);
}

void beCubeGates(std::size_t& realDrew, std::size_t& noBox, std::size_t& bothDrew)
{
    realDrew = g_beCubeRealDrew.load(std::memory_order_relaxed);
    noBox = g_beCubeNoBox.load(std::memory_order_relaxed);
    bothDrew = g_beCubeBothDrew.load(std::memory_order_relaxed);
}

void beCubeStats(std::size_t& cells, std::size_t& verts, std::size_t& miss)
{
    cells = g_beCubeCells.load(std::memory_order_relaxed);
    verts = g_beCubeVerts.load(std::memory_order_relaxed);
    miss = g_beCubeMiss.load(std::memory_order_relaxed);
}

std::size_t chunkBuildRefusedCount()
{
    return g_containerRefused.load(std::memory_order_relaxed);
}

void installGhostChunkBuilder()
{
    const Scanner& scanner = Scanner::instance();
    HookManager& hooks = HookManager::instance();
    g_chunkBuildLookup =
        reinterpret_cast<ChunkBuildLookupFn>(scanner.address(Target::ChunkBuildLookup));
    g_scheduleChunkBuild = reinterpret_cast<ScheduleChunkBuildFn>(
        scanner.address(Target::ScheduleChunkBuild));
    hooks.create(scanner.address(Target::LevelBuildDispatch), &detourLevelBuildDispatch,
                 reinterpret_cast<void**>(&g_levelBuildDispatch), L"LevelBuildDispatch");
}

void installAll()
{
    const Scanner& scanner = Scanner::instance();
    HookManager& hooks = HookManager::instance();

    hooks.create(scanner.address(Target::GetDestroySpeed), &detourGetDestroySpeed,
                 reinterpret_cast<void**>(&g_getDestroySpeed), L"GetDestroySpeed");

    hooks.create(scanner.address(Target::SetSelectedSlot), &detourSetSelectedSlot,
                 reinterpret_cast<void**>(&g_setSelectedSlot), L"SetSelectedSlot");

    hooks.create(scanner.address(Target::SubChunkSetBlock), &detourSubChunkSetBlock,
                 reinterpret_cast<void**>(&g_subChunkSetBlock), L"SubChunkSetBlock");

    installGhostChunkBuilder();

    hooks.create(scanner.address(Target::ChunkVisibilityScan), &detourChunkVisibilityScan,
                 reinterpret_cast<void**>(&g_chunkVisibilityScan), L"ChunkVisibilityScan");

    hooks.create(scanner.address(Target::VisibilityGate), &detourVisibilityGate,
                 reinterpret_cast<void**>(&g_visibilityGate), L"VisibilityGate");

    hooks.create(scanner.address(Target::BlockRenderLookup), &detourBlockRenderLookup,
                 reinterpret_cast<void**>(&g_blockRenderLookup), L"BlockRenderLookup");
    hooks.create(scanner.address(Target::BlockTessellate), &detourBlockTessellate,
                 reinterpret_cast<void**>(&g_blockTessellate), L"BlockTessellate");

    hooks.create(scanner.address(Target::ChunkMeshBuild), &detourChunkMeshBuild,
                 reinterpret_cast<void**>(&g_chunkMeshBuild), L"ChunkMeshBuild");

    g_blockTessellateCube =
        reinterpret_cast<BlockDrawFn>(scanner.address(Target::BlockTessellateCube));
    g_blockCubeFaces =
        reinterpret_cast<BlockFacesFn>(scanner.address(Target::BlockTessellateShape0));

    hooks.create(scanner.address(Target::HitResultAssign), &detourHitAssign,
                 reinterpret_cast<void**>(&g_hitAssign), L"HitResultAssign");

    hooks.create(scanner.address(Target::ModelPartDraw), &detourModelPart,
                 reinterpret_cast<void**>(&g_modelPart), L"ModelPartDraw");

    hooks.create(scanner.address(Target::TextureLookup), &detourTextureLookup,
                 reinterpret_cast<void**>(&g_textureLookup), L"TextureLookup");

    hooks.create(scanner.address(Target::PackStackOperation), &detourPackStackOp,
                 reinterpret_cast<void**>(&g_packStackOp), L"PackStackOperation");

    hooks.create(scanner.address(Target::BeDispatch), &detourBeDispatch,
                 reinterpret_cast<void**>(&g_beDispatch), L"BeDispatch");

    hooks.create(scanner.address(Target::BeRenderLoop), &detourBeRenderLoop,
                 reinterpret_cast<void**>(&g_beRenderLoop), L"BeRenderLoop");

    hooks.create(scanner.address(Target::BlockSourceSetBlock), &detourBlockSourceSetBlock,
                 reinterpret_cast<void**>(&g_blockSourceSetBlock), L"BlockSourceSetBlock");

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

    hooks.create(scanner.address(Target::InputGather), &detourInputGather,
                 reinterpret_cast<void**>(&g_inputGather), L"InputGather");

    hooks.create(scanner.address(Target::MoveIntentFromInput), &detourMoveIntent,
                 reinterpret_cast<void**>(&g_moveIntent), L"MoveIntentFromInput");

    hooks.create(scanner.address(Target::MoveApply), &detourMoveApply,
                 reinterpret_cast<void**>(&g_moveApply), L"MoveApply");

    hooks.create(scanner.address(Target::GetActorEffect), &detourGetActorEffect,
                 reinterpret_cast<void**>(&g_getActorEffect), L"GetActorEffect");

    g_openInventoryScreen =
        scanner.addressAs<OpenInventoryScreenFn>(Target::OpenInventoryScreen);

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
    hooks.create(scanner.address(Target::SettingsTabList), &detourSettingsTabList,
                 reinterpret_cast<void**>(&g_settingsTabList), L"SettingsTabList");
    g_settingsAddTab =
        reinterpret_cast<SettingsAddTabFn>(scanner.address(Target::SettingsAddTab));
    g_settingsInvokeAction = reinterpret_cast<SettingsInvokeActionFn>(
        scanner.address(Target::SettingsInvokeAction));
    g_openHowToPlayScreen = reinterpret_cast<OpenHowToPlayScreenFn>(
        scanner.address(Target::OpenHowToPlayScreen));

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

    hooks.create(scanner.address(Target::UiSliderPublish), &detourUiSliderPublish,
                 reinterpret_cast<void**>(&g_uiSliderPublish), L"UiSliderPublish");
    resolveCtlBag(scanner.address(Target::UiSliderPublish));

    (void)&detourUiBindingRead;

    hooks.create(scanner.address(Target::KeybindListBuild), &detourKeybindListBuild,
                 reinterpret_cast<void**>(&g_keybindListBuild), L"KeybindListBuild");

    hooks.create(scanner.address(Target::FogSettingsFetch), &detourFogSettingsFetch,
                 reinterpret_cast<void**>(&g_fogSettingsFetch), L"FogSettingsFetch");

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
    hooks.create(scanner.address(Target::KeyBindingIsDefault), &detourKeyBindingIsDefault,
                 reinterpret_cast<void**>(&g_keyBindingIsDefault), L"KeyBindingIsDefault");
    hooks.create(scanner.address(Target::SettingsActionData), &detourSettingsActionData,
                 reinterpret_cast<void**>(&g_settingsActionData), L"SettingsActionData");
    hooks.create(scanner.address(Target::SettingsActionQueryUpdate),
                 &detourSettingsActionQueryUpdate,
                 reinterpret_cast<void**>(&g_settingsActionQueryUpdate),
                 L"SettingsActionQueryUpdate");

    hooks.create(scanner.address(Target::OreKeyRowsWrap), &detourOreKeyRowsWrap,
                 reinterpret_cast<void**>(&g_oreKeyRowsWrap), L"OreKeyRowsWrap");
    uiprobe::setOreKeyRowsWrapAddr(scanner.address(Target::OreKeyRowsWrap));

    hooks.create(scanner.address(Target::RowDataCandA), &detourRowDataCandA,
                 reinterpret_cast<void**>(&g_rowDataCandA), L"RowDataCandA");
    hooks.create(scanner.address(Target::RowDataCandB), &detourRowDataCandB,
                 reinterpret_cast<void**>(&g_rowDataCandB), L"RowDataCandB");

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

    ghost::installGhostLayerHooks();

    boxes::installDepthHooks();

    frametrace::installHooks();

    worldmesh::installHooks();

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
                           unsigned char extra, bool simTick, bool& out)
{
    __try {
        out = fn(gameMode, blockPos, face, extra, simTick);
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

bool callBuildBlock(void* gameMode, void* blockPos, unsigned char face, unsigned char extra,
                    bool simTick)
{
    if (g_buildBlock == nullptr || gameMode == nullptr) {
        return false;
    }

    bool placed = false;
    if (callBuildBlockGuarded(g_buildBlock, gameMode, blockPos, face, extra, simTick, placed)) {
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

bool setUiBagNumber(void* holder, const char* name, unsigned long long size, float value)
{
    if (g_uiBagSet == nullptr || holder == nullptr || name == nullptr) {
        return false;
    }
    const UiName key{name, size};
    g_uiBagSet(holder, &key, &value);
    return true;
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

bool callSettingsAddTab(void* list, unsigned tab, const void* screenView, const void* descView)
{
    if (g_settingsAddTab == nullptr || list == nullptr || screenView == nullptr
        || descView == nullptr) {
        return false;
    }
    __try {
        g_settingsAddTab(list, tab, screenView, descView);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool callSettingsInvokeAction(void* component, void* done)
{
    if (g_settingsInvokeAction == nullptr || component == nullptr || done == nullptr) {
        return false;
    }
    __try {
        return g_settingsInvokeAction(component, done) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* gameClientInstance() { return g_clientInstance.load(std::memory_order_relaxed); }

namespace {

constexpr std::size_t kScreenContextSlot = 0x728;
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
