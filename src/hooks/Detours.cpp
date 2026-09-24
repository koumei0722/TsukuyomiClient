#include "hooks/Detours.h"

#include "config/WriteSwitches.h"

#include "core/FreezeWatch.h"
#include "core/Logger.h"
#include "core/Paths.h"
#include "core/Perf.h"
#include "game/BlockRegistry.h"
#include "game/BlockWrite.h"
#include "game/CommandRequest.h"
#include "game/ContainerUi.h"
#include "game/TradeUi.h"
#include "game/GameData.h"
#include "game/GameVersion.h"
#include "game/UiSound.h"
#include "game/InventoryActionBridge.h"
#include "game/InventoryScreen.h"
#include "game/ItemStackRequest.h"
#include "game/UiProbe.h"
#include "hooks/HookManager.h"
#include "hooks/HookCount.h"
#include "input/GameInput.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"
#include "modules/AntiEffect.h"
#include "modules/AutoTool.h"
#include "modules/CreativeNoClip.h"
#include "modules/FastBlockPlacement.h"
#include "modules/FastRightClick.h"
#include "modules/FlySpeed.h"
#include "modules/GameModeSwitch.h"
#include "modules/HandRestock.h"
#include "modules/InventoryHUD.h"
#include "modules/OffhandSwap.h"
#include "modules/FastInventory.h"
#include "modules/ItemScroller.h"
#include "modules/FreeCamera.h"
#include "modules/Fullbright.h"
#include "modules/NoRender.h"
#include "modules/Zoom.h"
#include "modules/Scaffold.h"
#include "modules/Schematica.h"
#include "modules/ShulkerPreview.h"
#include "render/BoxRenderer.h"
#include "render/FrameTrace.h"
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
#include <mutex>
#include <unordered_map>
#include <string>
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
    TSUKUYOMI_HOOK_COUNT("CameraUpdate");
    FreeCamera::instance().onCameraWrite(cameraBase);

    Zoom::instance().onCameraWrite(cameraBase, source);
    boxes::noteCamera(cameraBase);
}

using ViewVectorFn = void*(__fastcall*)(void* actor, float* out, float partial);
ViewVectorFn g_viewVector = nullptr;

void* __fastcall detourViewVector(void* actor, float* out, float partial)
{
    TSUKUYOMI_HOOK_COUNT("ViewVector");
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
    TSUKUYOMI_HOOK_COUNT("PlayerView");
    const freezewatch::Scope freezeScope{"PlayerView (game thread)"};
    hooks::refreshHookGroups();

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

    ItemScroller::instance().onPlayerViewUpdate();

    Schematica::instance().onPlayerViewUpdate();

    uiprobe::pumpMenuSelection();

    uiprobe::pumpControlsKeybind();
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
    TSUKUYOMI_HOOK_COUNT("PacketSend");
    if (FreeCamera::instance().enabled()
        && ItemStackRequest::packetId(packet) == 0x90) {
        zeroAuthInputMove(packet);
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

std::atomic<bool> g_countHotPaths{false};

inline bool countingHotPaths()
{
    return g_countHotPaths.load(std::memory_order_relaxed);
}

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

using UiBagFindFn = void*(__fastcall*)(void*, const char*);

using KeyDisplayNameFn = void(__fastcall*)(void*, void*, int);

using SettingsGroupRegisterFn = void*(__fastcall*)(void*, const void*, void*, void*);

using SettingsInvokeActionFn = unsigned char(__fastcall*)(void*, void*);

using OpenHowToPlayScreenFn = void(__fastcall*)(void*);

using SettingsProviderCallFn = void*(__fastcall*)(void*, void*, void*, void*);

using SettingsGroupInfoUpdateFn = void(__fastcall*)(void*);

using SettingsFindComponentFn = void*(__fastcall*)(void*, void*, const void*, void*);

using GameAllocateFn = void*(__fastcall*)(void*, size_t);

using OreKeyRowsBuildFn = void*(__fastcall*)(void*, void*, void*);

using I18nGetFn = void*(__fastcall*)(void*, void*, const void*, void*);

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
using MoveIntentFn = void*(__fastcall*)(void*, void*, void*, void*, std::uint64_t, void*);
MoveIntentFn g_moveIntent = nullptr;
GetActorEffectFn g_getActorEffect = nullptr;
OpenInventoryScreenFn g_openInventoryScreen = nullptr;
ContainerOpenHandleFn g_containerOpenHandle = nullptr;
InventoryContentReadFn g_inventoryContentRead = nullptr;
InventoryContentReadFn g_containerOpenRead = nullptr;
HandleItemStackResponseFn g_handleItemStackResponse = nullptr;
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
using ContainerGetItemFn = const void*(__fastcall*)(void*, const void*, int);
ContainerGetItemFn g_containerGetItem = nullptr;
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
InventoryHotbarKeyFn g_inventoryHotbarKey = nullptr;
AddRequestActionFn g_addRequestAction = nullptr;
UiDefLookupFn g_uiDefLookup = nullptr;
UiBagFindFn g_uiBagFind = nullptr;
UiEventDispatchFn g_uiEventDispatch = nullptr;
KeyDisplayNameFn g_keyDisplayName = nullptr;
SettingsGroupRegisterFn g_settingsGroupRegister = nullptr;
SettingsInvokeActionFn g_settingsInvokeAction = nullptr;
OpenHowToPlayScreenFn g_openHowToPlayScreen = nullptr;

std::atomic<void*> g_clientInstance{nullptr};
SettingsProviderCallFn g_settingsProviderCall = nullptr;
SettingsGroupInfoUpdateFn g_settingsGroupInfoUpdate = nullptr;
SettingsFindComponentFn g_settingsFindComponent = nullptr;
GameAllocateFn g_gameAllocate = nullptr;
using FogSettingsFetchFn = void*(__fastcall*)(void*, void*, void*, void*);
FogSettingsFetchFn g_fogSettingsFetch = nullptr;
using LegacyParticleInsertFn = void(__fastcall*)(void* frameBuilder, void* description);
LegacyParticleInsertFn g_legacyParticleInsert = nullptr;
using ShulkerContentsTextFn = void*(__fastcall*)(void* out, const void* tag);
ShulkerContentsTextFn g_shulkerContentsText = nullptr;
using HoverRendererRenderFn = void(__fastcall*)(void* self, void* ctx, void* client, void* owner, int pass);
HoverRendererRenderFn g_hoverRendererRender = nullptr;
using AttackCoreFn = bool(__fastcall*)(void* gameMode, void* target, bool direct, const void* hitPos);
AttackCoreFn g_attackCore = nullptr;
using SendComplexTxFn = void(__fastcall*)(void* player, void** transaction);
SendComplexTxFn g_sendComplexTx = nullptr;
std::uintptr_t g_legacyParticleSubmitBegin = 0;
std::uintptr_t g_legacyParticleSubmitEnd = 0;
OreKeyRowsBuildFn g_oreKeyRowsBuild = nullptr;
I18nGetFn g_i18nGet = nullptr;
void* g_i18nSelf = nullptr;

constexpr bool kKeepRowsAfterConsume = true;

float __fastcall detourGetDestroySpeed(void* rcx, void* rdx, void* r8, void* r9)
{
    return AutoTool::instance().onGetDestroySpeed(rcx, rdx, r8, r9);
}

void __fastcall detourSetSelectedSlot(void* rcx, void* rdx, void* r8, void* r9)
{
    TSUKUYOMI_HOOK_COUNT("SetSelectedSlot");
    const void* const returnAddress = _ReturnAddress();

    if (Zoom::instance().suppressHotbar(returnAddress)) {
        return;
    }

    HandRestock::instance().onSetSelectedSlot(rcx);

    AutoTool::instance().onSetSelectedSlot(rcx, rdx, r8, r9);
}

void __fastcall detourHandleItemStackResponse(void* rcx, void* rdx, void* r8, void* r9)
{
    TSUKUYOMI_HOOK_COUNT("HandleItemStackResponse");
    ItemStackRequest::instance().onResponse(rdx);

    if (g_handleItemStackResponse != nullptr) {
        g_handleItemStackResponse(rcx, rdx, r8, r9);
    }
}

bool __fastcall detourBuildBlock(void* gameMode, void* blockPos, unsigned char face,
                                 unsigned char extra, bool simTick)
{
    TSUKUYOMI_HOOK_COUNT("buildBlock");
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
    TSUKUYOMI_HOOK_COUNT("useItem");
    return FastRightClick::instance().onUseItem(gameMode, itemStack, extra);
}

int __fastcall detourUseItemTransaction(void* gameMode, void* itemStack, int extra)
{
    TSUKUYOMI_HOOK_COUNT("useItemTransaction");
    return FastRightClick::instance().onUseItemTransaction(gameMode, itemStack, extra);
}

using SubChunkSetBlockFn = void(__fastcall*)(void*, unsigned int, unsigned int, const void*);
SubChunkSetBlockFn g_subChunkSetBlock = nullptr;

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
                blocks::restoreGhostCellFromWant(at[0], at[1], at[2]);
            } else if (!mine) {
                blocks::dropGhostCell(at[0], at[1], at[2]);
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

using ChunkCoordinatorFrameFn = void(__fastcall*)(void*);
ChunkCoordinatorFrameFn g_chunkCoordinatorFrame = nullptr;

void __fastcall detourChunkCoordinatorFrame(void* self)
{
    TSUKUYOMI_HOOK_COUNT("ChunkCoordinatorFrame");
    if (self != nullptr) {
        g_visCoordinator.store(self, std::memory_order_relaxed);
    }
    if (g_chunkCoordinatorFrame != nullptr) {
        g_chunkCoordinatorFrame(self);
    }
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

bool storagePredKnown(const void* target)
{
    const std::size_t used = std::min(g_storagePredCount.load(std::memory_order_acquire),
                                      kStoragePredMax);
    for (std::size_t i = 0; i < used; ++i) {
        if (g_storagePredTarget[i] == target) {
            return true;
        }
    }
    return false;
}

bool queueStoragePred(void* target)
{
    if (target == nullptr) {
        return false;
    }
    if (!writes::allowed(std::string_view("SubChunkStoragePredicate"))) {
        return false;
    }
    const std::size_t used = std::min(g_storagePredCount.load(std::memory_order_acquire),
                                      kStoragePredMax);
    for (std::size_t i = 0; i < used; ++i) {
        if (g_storagePredTarget[i] == target) {
            return false;
        }
    }
    if (used >= kStoragePredMax) {
        static std::atomic<bool> told{false};
        if (!told.exchange(true, std::memory_order_relaxed)) {
            log().warn(L"Schematica: ran out of slots for the storage emptiness predicate "
                       L"({}); wider chunks will not show the ghost",
                       kStoragePredMax);
        }
        return false;
    }
    g_storagePredTarget[used] = target;
    HookManager& hooks = HookManager::instance();
    if (!hooks.create(target, kStoragePredDetours[used],
                      reinterpret_cast<void**>(&g_storagePredOriginal[used]),
                      L"SubChunkStoragePredicate", HookGroup::Ghost)) {
        g_storagePredTarget[used] = nullptr;
        return false;
    }
    g_storagePredCount.store(used + 1, std::memory_order_release);
    return true;
}

int g_storageArmBatch = 0;
bool g_storageArmQueued = false;
struct DeferredStorageNote {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
    int layer = 0;
    void* storage = nullptr;
};
std::vector<DeferredStorageNote> g_storageArmNotes;

void flushStoragePreds()
{
    if (!g_storageArmQueued) {
        return;
    }
    g_storageArmQueued = false;
    HookManager::instance().applyQueued();
}

using BlockSourceGetBlockFn = const void*(__fastcall*)(void*, const void*);
BlockSourceGetBlockFn g_blockSourceGetBlock = nullptr;

constexpr int kOverlaySizeUnits = -8;

std::atomic<std::size_t> g_overlayStacked{0};
std::atomic<std::size_t> g_overlayMissed{0};

using BlockSourceGetExtraFn = const void*(__fastcall*)(void*, const void*);
BlockSourceGetExtraFn g_blockSourceGetExtra = nullptr;

thread_local int t_meshBuildDepth = 0;

struct MeshBuildWindow {
    MeshBuildWindow() { ++t_meshBuildDepth; }
    ~MeshBuildWindow() { --t_meshBuildDepth; }
    MeshBuildWindow(const MeshBuildWindow&) = delete;
    MeshBuildWindow& operator=(const MeshBuildWindow&) = delete;
};

bool drawingSide(unsigned long tid)
{
    return t_meshBuildDepth > 0 && blocks::isMeshThread(tid);
}

thread_local bool t_meshThreadAnnounced = false;

inline void announceMeshThreadOnce()
{
    if (t_meshThreadAnnounced) {
        return;
    }
    const unsigned long id = GetCurrentThreadId();
    blocks::noteMeshThread(id);
    t_meshThreadAnnounced = blocks::isMeshThread(id);
}

const void* __fastcall detourBlockSourceGetExtra(void* region, const void* pos)
{
    if (pos != nullptr && blocks::ghostOn() && drawingSide(GetCurrentThreadId())) {
        const auto* const p = static_cast<const std::int32_t*>(pos);
        if (const void* const ghost = blocks::ghostBlockAt(p[0], p[1], p[2], 1);
            ghost != nullptr) {
            return ghost;
        }
    }
    return g_blockSourceGetExtra != nullptr ? g_blockSourceGetExtra(region, pos) : nullptr;
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
    TSUKUYOMI_HOOK_COUNT("HitResultAssign");
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
    if (countingHotPaths()) {
        g_getBlockCalls.fetch_add(1, std::memory_order_relaxed);
    }
    if (pos != nullptr && blocks::ghostOn()) {
        const auto* const p = static_cast<const std::int32_t*>(pos);
        if (const void* const ghost = blocks::ghostBlockAt(p[0], p[1], p[2]);
            ghost != nullptr) {
            if (drawingSide(GetCurrentThreadId())) {
                blocks::noteGhostHit(blocks::GhostHook::Layer);
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
    TSUKUYOMI_HOOK_COUNT("BlockSourceSetBlock");
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

thread_local std::uint32_t t_buildBoxTries = 0;
thread_local std::uint32_t t_buildBoxStacked = 0;

inline std::uintptr_t realLayerByte(void* real)
{
    return reinterpret_cast<std::uintptr_t>(real) & 0xFF;
}

void* __fastcall detourBlockRenderLookup(void* block, void* context, void* pos)
{
    announceMeshThreadOnce();

    void* const real =
        g_blockRenderLookup != nullptr ? g_blockRenderLookup(block, context, pos) : nullptr;

    if (pos != nullptr && blocks::ghostOn()) {
        const auto* const p = static_cast<const std::int32_t*>(pos);
        const void* const ghostHere = blocks::ghostBlockAt(p[0], p[1], p[2]);
        const bool overlayOn = blocks::ghostOverMismatchOn();
        const blocks::DiffColor pushColor = (ghostHere == nullptr && overlayOn)
                                                ? blocks::diffCellAt(p[0], p[1], p[2])
                                                : blocks::DiffColor::None;
        const bool pushForOverlay =
            overlayOn
            && (pushColor == blocks::DiffColor::Wrong || pushColor == blocks::DiffColor::State)
            && blocks::wantBlockAt(p[0], p[1], p[2]) != nullptr;
        if (pushForOverlay) {
            if (const auto value = realLayerByte(real); value < 32) {
                blocks::noteRealLayer(block, static_cast<int>(value));
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

using ChunkMeshBuildFn = void*(__fastcall*)(void*, void*, void*, void*, void*, void*);
ChunkMeshBuildFn g_chunkMeshBuild = nullptr;

bool readChunkOrigin(const void* recV, std::int32_t* out)
{
    __try {
        const auto* const rec = static_cast<const unsigned char*>(recV);
        std::memcpy(&out[0], rec + 0x34, sizeof(std::int32_t));
        std::memcpy(&out[1], rec + 0x38, sizeof(std::int32_t));
        std::memcpy(&out[2], rec + 0x3c, sizeof(std::int32_t));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* __fastcall detourChunkMeshBuild(void* ctx, void* rec, void* a3, void* a4, void* a5,
                                      void* a6)
{
    announceMeshThreadOnce();
    const long long perfBegan = perf::now();
    t_buildBoxTries = 0;
    t_buildBoxStacked = 0;
    const std::uint64_t buildSeq = nextBuildSeq();
    void* ret = nullptr;
    {
        const MeshBuildWindow window;
        ret = g_chunkMeshBuild != nullptr ? g_chunkMeshBuild(ctx, rec, a3, a4, a5, a6)
                                          : nullptr;
    }
    perf::add(perf::Slot::ChunkBuild, perfBegan);
    noteChunkBoxTries(rec, t_buildBoxTries, t_buildBoxStacked, buildSeq);
    noteChunkBuiltForRebuilds(rec, buildSeq);
    return ret;
}

void __fastcall detourBlockTessellate(void* self, void* a, void* graphics, void* record,
                                      void* arg5)
{
    constexpr std::size_t kVertexColorBegin = 0x70;
    constexpr std::size_t kVertexColorEnd = 0x78;
    constexpr std::size_t kVertexColorSanity = 1u << 20;

    announceMeshThreadOnce();

    const void* overlayWant = nullptr;
    if (record != nullptr && self != nullptr && a != nullptr && blocks::ghostOn()
        && blocks::ghostOverMismatchOn()) {
        const auto* const p = static_cast<const std::int32_t*>(record);
        const blocks::DiffColor here = blocks::diffCellAt(p[0], p[1], p[2]);
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
    if ((isGhost || overlayWant != nullptr) && self != nullptr
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
    if (overlayWant != nullptr && ghostBucket) {
        ++t_buildBoxTries;
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
                const float kScale =
                    1.0F + static_cast<float>(kOverlaySizeUnits) / 512.0F;
                for (unsigned char* q = begin + posBefore; q + 12 <= begin + posAfter;
                     q += 12) {
                    for (int k = 0; k < 3; ++k) {
                        float v = 0.0F;
                        std::memcpy(&v, q + k * 4, sizeof(v));
                        v = center[k] + (v - center[k]) * kScale;
                        std::memcpy(q + k * 4, &v, sizeof(v));
                    }
                }
            }
        }
    }

    if (g_blockTessellate != nullptr && !skipReal) {
        g_blockTessellate(self, a, graphics, record, arg5);
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
    TSUKUYOMI_HOOK_COUNT("SetGameMode");
    GameModeSwitch::instance().onSetGameMode(self, mode, extra);
    CommandRequest::instance().onEntityContext(self);

    if (g_setGameMode != nullptr) {
        g_setGameMode(self, mode, extra);
    }
}

void __fastcall detourNotifyInventoryOpen(void* client, int which)
{
    TSUKUYOMI_HOOK_COUNT("NotifyInventoryOpen");
    ItemStackRequest::instance().onNotifyInventoryOpen(client);

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
    TSUKUYOMI_HOOK_COUNT("InputGather");
    noteSneakInput(out);
    FreeCamera::instance().onInputGatherBefore(out);

    void* const result = (g_inputGather != nullptr) ? g_inputGather(a1, out, a3, a4) : nullptr;

    FreeCamera::instance().onInputGather(out, a1);
    return result;
}

void* __fastcall detourMoveIntent(void* out, void* input, void* a3, void* a4, std::uint64_t a5, void* a6)
{
    TSUKUYOMI_HOOK_COUNT("MoveIntentFromInput");
    void* const result = (g_moveIntent != nullptr) ? g_moveIntent(out, input, a3, a4, a5, a6) : nullptr;
    FreeCamera::instance().onMoveIntent(out, input);
    return result;
}

void* __fastcall detourMoveInputHandler(void* client, void* a2, void* a3, void* a4)
{
    TSUKUYOMI_HOOK_COUNT("MoveInputHandler");
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
    return Fullbright::instance().onGetEffect(effectId, shown);
}

void __fastcall detourContainerOpenHandle(void* packet, void* result, void* callback,
                                          void* network)
{
    TSUKUYOMI_HOOK_COUNT("ContainerOpenHandle");
    if (g_containerOpenHandle == nullptr) {
        return;
    }
    ItemStackRequest& requests = ItemStackRequest::instance();
    std::byte shell[ItemStackRequest::kOpenShellBytes];
    const bool watching = requests.suppressionPending() && requests.captureOpenShell(packet, shell);
    g_containerOpenHandle(packet, result, callback, network);
    requests.rememberContainerOpenResult(result);
    if (watching) {
        requests.takeInventoryOpen(packet, result, shell);
    }
}

void* __fastcall detourInventoryContentRead(void* packet, void* out, void* stream, void* network)
{
    TSUKUYOMI_HOOK_COUNT("InventoryContentRead");
    if (g_inventoryContentRead == nullptr) {
        return out;
    }
    void* const result = g_inventoryContentRead(packet, out, stream, network);
    OffhandSwap::instance().onInventoryContent(packet);
    return result;
}

void* __fastcall detourContainerOpenRead(void* packet, void* out, void* stream, void* network)
{
    TSUKUYOMI_HOOK_COUNT("ContainerOpenRead");
    if (g_containerOpenRead == nullptr) {
        return out;
    }
    void* const result = g_containerOpenRead(packet, out, stream, network);
    InventoryActionBridge::instance().onContainerOpen(packet);
    return result;
}

void* __fastcall detourInventoryHoveredSlot(void* controller, void* out)
{
    TSUKUYOMI_HOOK_COUNT("InventoryHoveredSlot");
    InventoryScreen::instance().onController(controller);

    return g_inventoryHoveredSlot != nullptr ? g_inventoryHoveredSlot(controller, out) : out;
}

void __fastcall detourInventoryHotbarKey(void* controller, void* collection, int hotbarIndex)
{
    TSUKUYOMI_HOOK_COUNT("InventoryHotbarKey");
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
    TSUKUYOMI_HOOK_COUNT("UiDefLookup");
    uiprobe::onLookup(space, name);
    void* const value = (g_uiDefLookup != nullptr) ? g_uiDefLookup(self, space, name) : nullptr;
    if (void* const extended = uiprobe::extendDefinition(self, space, name, value)) {
        return extended;
    }
    if (void* const swapped = uiprobe::substitute(self, space, name)) {
        return swapped;
    }
    return value;
}

void* __fastcall detourFogSettingsFetch(void* self, void* out, void* src, void* r9)
{
    TSUKUYOMI_HOOK_COUNT("FogSettingsFetch");
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

void* __fastcall detourShulkerContentsText(void* out, const void* tag)
{
    TSUKUYOMI_HOOK_COUNT("ShulkerContentsText");
    void* const result = (g_shulkerContentsText != nullptr) ? g_shulkerContentsText(out, tag) : out;
    ShulkerPreview::instance().onContentsText(out, tag, _ReturnAddress());
    return result;
}

void __fastcall detourHoverRendererRender(void* self, void* ctx, void* client, void* owner, int pass)
{
    TSUKUYOMI_HOOK_COUNT("HoverRendererRender");
    if (g_hoverRendererRender != nullptr) {
        g_hoverRendererRender(self, ctx, client, owner, pass);
    }
    ShulkerPreview::instance().onHoverRender(self, ctx, client, owner);
}

bool __fastcall detourAttackCore(void* gameMode, void* target, bool direct, const void* hitPos)
{
    TSUKUYOMI_HOOK_COUNT("AttackCore");
    return AutoTool::instance().onAttack(gameMode, target, direct, hitPos);
}

void __fastcall detourSendComplexTx(void* player, void** transaction)
{
    TSUKUYOMI_HOOK_COUNT("SendComplexTransaction");
    if (AutoTool::instance().onSendTransaction(player, transaction)) {
        return;
    }
    if (g_sendComplexTx != nullptr) {
        g_sendComplexTx(player, transaction);
    }
}

void __fastcall detourLegacyParticleInsert(void* frameBuilder, void* description)
{
    TSUKUYOMI_HOOK_COUNT("LegacyParticleInsert");
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
    TSUKUYOMI_HOOK_COUNT("OreKeyRowsBuild");
    constexpr bool kAddRowEntry = true;
    const bool substituted = kAddRowEntry && uiprobe::substituteKeyRows(rdx, kKeepRowsAfterConsume);
    void* const result =
        (g_oreKeyRowsBuild != nullptr) ? g_oreKeyRowsBuild(out, rdx, r8) : nullptr;
    if (substituted && !kKeepRowsAfterConsume) {
        uiprobe::popKeyRowSubstitution();
    }
    return result;
}

void* __fastcall detourI18nGet(void* self, void* out, const void* key, void* r9)
{
    TSUKUYOMI_HOOK_COUNT("I18nGet");
    void* const result = (g_i18nGet != nullptr) ? g_i18nGet(self, out, key, r9) : nullptr;
    constexpr bool kOverrideRowName = true;
    if (kOverrideRowName) {
        uiprobe::overrideTranslation(key, out);
    }
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

void* __fastcall detourSettingsGroupRegister(void* registry, const void* idView, void* provider,
                                             void* r9)
{
    TSUKUYOMI_HOOK_COUNT("SettingsGroupRegister");
    uiprobe::onSettingsGroupRegister(registry, idView, provider);
    void* const result = (g_settingsGroupRegister != nullptr)
                             ? g_settingsGroupRegister(registry, idView, provider, r9)
                             : nullptr;
    uiprobe::afterSettingsGroupRegister(registry, idView, provider);
    return result;
}

void* __fastcall detourSettingsProviderCall(void* self, void* out, void* r8, void* r9)
{
    TSUKUYOMI_HOOK_COUNT("SettingsProviderCall");
    void* const result =
        (g_settingsProviderCall != nullptr) ? g_settingsProviderCall(self, out, r8, r9) : nullptr;
    uiprobe::onSettingsProviderCall(self, out);
    return result;
}

void __fastcall detourSettingsGroupInfoUpdate(void* self)
{
    TSUKUYOMI_HOOK_COUNT("SettingsGroupInfoUpdate");
    const bool swapped = uiprobe::beforeSettingsGroupInfoUpdate(self);
    if (g_settingsGroupInfoUpdate != nullptr) {
        g_settingsGroupInfoUpdate(self);
    }
    uiprobe::afterSettingsGroupInfoUpdate(self, swapped);
}

void* __fastcall detourSettingsFindComponent(void* self, void* out, const void* idView, void* r9)
{
    TSUKUYOMI_HOOK_COUNT("SettingsFindComponent");
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
    TSUKUYOMI_HOOK_COUNT("UiSliderPublish");
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
            log().warn(L"Hooks: could not find the tick of the trade screen controller; the auto trade will open "
                       L"the screen normally");
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
    TSUKUYOMI_HOOK_COUNT("SceneStackPush");
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
    TSUKUYOMI_HOOK_COUNT("ContainerScreenTick");
    const freezewatch::Scope freezeScope{"ContainerScreenTick (UI thread)"};
    const std::uint32_t dirty = (g_containerScreenTick != nullptr) ? g_containerScreenTick(self) : 0;
    return dirty | containerui::onScreenTickHook(self);
}

int __fastcall detourTradeHoverInvoke(void* self, void* const* bag)
{
    TSUKUYOMI_HOOK_COUNT("TradeHoverInvoke");
    if (bag != nullptr) {
        tradeui::onHoverInvoke(*bag);
    }
    return (g_tradeHoverInvoke != nullptr) ? g_tradeHoverInvoke(self, bag) : 0;
}

int __fastcall detourTradeSecondaryInvoke(void* self, void* const* bag)
{
    TSUKUYOMI_HOOK_COUNT("TradeSecondaryInvoke");
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
    TSUKUYOMI_HOOK_COUNT("TradeSelParse");
    void* const result = g_tradeSelParse(sel, bag);
    tradeui::afterSelParse(sel);
    return result;
}
int __fastcall detourTradeSelectorTotal(void* self)
{
    TSUKUYOMI_HOOK_COUNT("TradeSelectorTotal");
    const int n = g_tradeSelectorTotal(self);
    return (n > 0 && tradeui::favoriteTierShown(self)) ? n + 1 : n;
}
int __fastcall detourTradeTierTotal(void* self, const int* tier)
{
    TSUKUYOMI_HOOK_COUNT("TradeTierTotal");
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
    TSUKUYOMI_HOOK_COUNT("TradeTierVisible");
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
    TSUKUYOMI_HOOK_COUNT("TradeTierUnlocked");
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
    TSUKUYOMI_HOOK_COUNT("TradeTierName");
    (void)self;
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
    TSUKUYOMI_HOOK_COUNT("TradeCurrentTier");
    const int value = g_tradeCurrentTier(owner);
    return tradeui::overrideTier(owner, value, _ReturnAddress());
}

void* __fastcall detourContainerScreenCtor(void* self, void* a2, void* a3, void* a4)
{
    TSUKUYOMI_HOOK_COUNT("ContainerScreenCtor");
    void* const result = (g_containerScreenCtor != nullptr) ? g_containerScreenCtor(self, a2, a3, a4) : self;
    containerui::onScreenConstructed(self);
    return result;
}

void* __fastcall detourHudScreenCtor(void* self, void* a2, void* a3, void* a4)
{
    TSUKUYOMI_HOOK_COUNT("HudScreenCtor");
    void* const result = (g_hudScreenCtor != nullptr) ? g_hudScreenCtor(self, a2, a3, a4) : self;
    containerui::onHudConstructed(self);
    return result;
}

const void* __fastcall detourContainerGetItem(void* mc, const void* collectionName, int index)
{
    TSUKUYOMI_HOOK_COUNT("ContainerGetItem");
    if (mc != nullptr && mc == InventoryHUD::hudManager()) {
        if (const void* const own = InventoryHUD::offhandStackFor(collectionName, index)) {
            return own;
        }
    }
    return g_containerGetItem(mc, collectionName, index);
}

void* __fastcall detourContainerScreenDtor(void* self)
{
    TSUKUYOMI_HOOK_COUNT("ContainerScreenDtor");
    containerui::onScreenDestroyed(self);
    return (g_containerScreenDtor != nullptr) ? g_containerScreenDtor(self) : self;
}

int __fastcall detourContainerSm(void* sm, std::uint32_t id, int state, const void* coll, int index)
{
    TSUKUYOMI_HOOK_COUNT("ContainerSm");
    int result = 0;
    if (containerui::onSmHandle(sm, id, state, coll, index, result)) {
        return result;
    }
    return (g_containerSm != nullptr) ? g_containerSm(sm, id, state, coll, index) : 0;
}

int __fastcall detourUiEventDispatch(void* self, const void* event, void* r8, void* r9)
{
    TSUKUYOMI_HOOK_COUNT("UiEventDispatch");
    uiprobe::onUiEvent(self, event);
    return (g_uiEventDispatch != nullptr) ? g_uiEventDispatch(self, event, r8, r9) : 0;
}

void __fastcall detourAddRequestAction(void** clientHolder, void** action)
{
    TSUKUYOMI_HOOK_COUNT("AddRequestAction");
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
            if (fn != nullptr && memory::inGameModule(fn) && memory::isExecutable(fn, 16)
                && queueStoragePred(fn)) {
                g_storageArmQueued = true;
            }
        }
    }
    if (g_storageArmBatch == 0) {
        flushStoragePreds();
    }
}

void beginStorageArmBatch()
{
    ++g_storageArmBatch;
}

void endStorageArmBatch()
{
    if (g_storageArmBatch <= 0) {
        return;
    }
    if (--g_storageArmBatch > 0) {
        return;
    }
    flushStoragePreds();
    for (const DeferredStorageNote& one : g_storageArmNotes) {
        noteGhostStorage(one.x, one.y, one.z, one.layer, one.storage);
    }
    g_storageArmNotes.clear();
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
        void* vtable = nullptr;
        if (!readStorageVtable(storage, &vtable) || vtable == nullptr
            || !memory::inGameModule(vtable)
            || !memory::inGameModule(static_cast<const std::uint8_t*>(vtable) + 8 * 4 - 1)) {
            continue;
        }
        for (int slot = 1; slot <= 2; ++slot) {
            void* fn = nullptr;
            std::memcpy(&fn, static_cast<const std::uint8_t*>(vtable) + slot * 8,
                        sizeof(fn));
            if (fn == nullptr || !memory::inGameModule(fn) || storagePredKnown(fn)) {
                continue;
            }
            if (memory::isExecutable(fn, 16) && queueStoragePred(fn)) {
                g_storageArmQueued = true;
            }
        }
        if (g_storageArmBatch > 0 && g_storageArmQueued) {
            g_storageArmNotes.push_back(DeferredStorageNote{baseX, baseY, baseZ, layer, storage});
            continue;
        }
        flushStoragePreds();
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
    g_i18nSelf = const_cast<void*>(reinterpret_cast<const void*>(global));
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

using BeRenderLoopFn = void(__fastcall*)(void*, void*, unsigned int);
BeRenderLoopFn g_beRenderLoop = nullptr;

void __fastcall detourBeRenderLoop(void* renderer, void* ctx, unsigned int shadow)
{
    TSUKUYOMI_HOOK_COUNT("BeRenderLoop");
    void* source = nullptr;
    if (renderer != nullptr) {
        auto* const at = static_cast<unsigned char*>(renderer) + 0x980;
        if (memory::isReadable(at, 8)) {
            std::memcpy(&source, at, sizeof(source));
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

    return false;
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
    if (!g_containerRefusedTold.exchange(true, std::memory_order_relaxed)) {
        static const wchar_t* const kWhy[] = {L"?", L"the vtable is outside the game",
                                              L"what +0x5f0 points at cannot be read",
                                              L"the lock is -1", L"the fields look insane"};
        const int why = g_containerWhy.load(std::memory_order_relaxed);
        log().warn(L"Schematica: the chunk record owner looked broken, so it was not called "
                   L"(reason {} = {} / count {} / range {}..{})",
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

__declspec(noinline) bool probeReadable(const void* at, std::size_t bytes)
{
    if (at == nullptr || bytes == 0) {
        return false;
    }
    __try {
        const auto* const p = static_cast<const volatile std::uint8_t*>(at);
        (void)p[0];
        (void)p[bytes - 1];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

__declspec(noinline) bool sharedIsReadable(void* rec)
{
    __try {
        void* shared = nullptr;
        std::memcpy(&shared, static_cast<std::uint8_t*>(rec) + 0x10, sizeof(shared));
        return shared != nullptr && probeReadable(shared, 0x40);
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

constexpr std::int64_t kAskPerFrame = 96;
struct AskSweep {
    bool active = false;
    bool everything = false;
    bool afterOff = false;
    std::int32_t low[3] = {0, 0, 0};
    std::int32_t high[3] = {0, 0, 0};
    std::int64_t next = 0;
    std::int64_t total = 0;
    std::size_t wanted = 0;
    std::size_t asked = 0;
};
AskSweep g_askSweep;

bool beginAskSweep()
{
    AskSweep sweep;
    if (!blocks::ghostBounds(sweep.low, sweep.high)) {
        if (!blocks::lastGhostBounds(sweep.low, sweep.high)) {
            return false;
        }
        sweep.afterOff = true;
    }
    sweep.everything = g_ghostBuildForced.exchange(false, std::memory_order_relaxed) || sweep.afterOff;
    const std::int64_t ny = (sweep.high[1] >> 4) - (sweep.low[1] >> 4) + 1;
    const std::int64_t nx = (sweep.high[0] >> 4) - (sweep.low[0] >> 4) + 1;
    const std::int64_t nz = (sweep.high[2] >> 4) - (sweep.low[2] >> 4) + 1;
    if (ny <= 0 || nx <= 0 || nz <= 0) {
        return false;
    }
    sweep.total = ny * nx * nz;
    sweep.active = true;
    if (sweep.everything) {
        g_askForcedRounds.fetch_add(1, std::memory_order_relaxed);
    }
    g_askSweep = sweep;
    return true;
}

bool askSweepActive()
{
    return g_askSweep.active;
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
    AskSweep& sweep = g_askSweep;
    if (!sweep.active) {
        return;
    }
    const bool afterOff = sweep.afterOff;
    const bool everything = sweep.everything;
    std::size_t& wanted = sweep.wanted;
    std::size_t& asked = sweep.asked;
    float camera[3] = {0.0F, 0.0F, 0.0F};
    const bool haveCamera = readCameraPos(fields.cameraPos, camera);
    const std::int64_t nx = (sweep.high[0] >> 4) - (sweep.low[0] >> 4) + 1;
    const std::int64_t nz = (sweep.high[2] >> 4) - (sweep.low[2] >> 4) + 1;
    const std::int64_t perFrame = afterOff ? sweep.total : kAskPerFrame;
    const std::int64_t stop = std::min(sweep.total, sweep.next + perFrame);
    for (; sweep.next < stop; ++sweep.next) {
        {
            {
                const std::int64_t index = sweep.next;
                const auto cy = static_cast<std::int32_t>((sweep.low[1] >> 4) + index / (nx * nz));
                const auto cx = static_cast<std::int32_t>((sweep.low[0] >> 4) + (index / nz) % nx);
                const auto cz = static_cast<std::int32_t>((sweep.low[2] >> 4) + index % nz);
                if (!afterOff && !blocks::ghostSubChunkOccupied(cx << 4, cy << 4, cz << 4)) {
                    continue;
                }
                ++wanted;
                const std::int32_t at[3] = {cx, cy, cz};
                for (void** it = fields.containerBegin; it < fields.containerEnd; it += kContainerStride) {
                    void* const rec = callChunkBuildLookup(*it, at);
                    if (rec == nullptr) {
                        continue;
                    }
                    if (!probeReadable(rec, 0x40) || !sharedIsReadable(rec)) {
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
    if (sweep.next < sweep.total) {
        return;
    }
    sweep.active = false;

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

void noteChunkBoxTries(const void* rec, std::uint32_t tries, std::uint32_t stacked,
                       std::uint64_t buildSeq)
{
    if (rec == nullptr || !blocks::ghostOn()) {
        return;
    }
    std::int32_t org[3] = {};
    if (!readChunkOrigin(rec, org)
        || !blocks::ghostBoxTouchesSubChunk(org[0], org[1], org[2])) {
        return;
    }
    const std::int32_t c[3] = {org[0] >> 4, org[1] >> 4, org[2] >> 4};
    std::lock_guard<std::mutex> lock(g_chunkBoxTriesMutex);
    if (g_chunkBoxTries.size() > 65536) {
        g_chunkBoxTries.clear();
    }
    ChunkBoxTries& one = g_chunkBoxTries[rebuildKey(c)];
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
    if (!readChunkOrigin(rec, org)) {
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
        if (rec == nullptr || !probeReadable(rec, 0x40) || !sharedIsReadable(rec)) {
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
    const freezewatch::Scope freezeScope{"LevelBuildDispatch"};
    t_dispatchParams = params;
    if (blocks::ghostOn()) {
        Schematica::instance().earlyLearnFrame();
    }
    std::int32_t lastLow[3] = {0, 0, 0};
    std::int32_t lastHigh[3] = {0, 0, 0};
    if (blocks::ghostOn() || blocks::lastGhostBounds(lastLow, lastHigh)) {
        const unsigned long long now = GetTickCount64();
        if (!blocks::ghostOn() && askSweepActive() && !g_askSweep.afterOff) {
            g_askSweep.active = false;
            g_ghostBuildAskedAt.store(0, std::memory_order_relaxed);
        }
        if (askSweepActive()
            || (now - g_ghostBuildAskedAt.load(std::memory_order_relaxed) >= 1000 && beginAskSweep())) {
            askGhostChunkBuilds(params);
            if (!askSweepActive()) {
                g_ghostBuildAskedAt.store(now, std::memory_order_relaxed);
            }
        }
    } else if (askSweepActive()) {
        g_askSweep.active = false;
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

std::uint64_t requestChunkRebuilds(const std::vector<std::array<std::int32_t, 3>>& chunks)
{
    if (chunks.empty()) {
        return 0;
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
    return seq;
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

void installGhostChunkBuilder()
{
    const Scanner& scanner = Scanner::instance();
    HookManager& hooks = HookManager::instance();
    g_chunkBuildLookup =
        reinterpret_cast<ChunkBuildLookupFn>(scanner.address(Target::ChunkBuildLookup));
    g_scheduleChunkBuild = reinterpret_cast<ScheduleChunkBuildFn>(
        scanner.address(Target::ScheduleChunkBuild));
    hooks.create(scanner.address(Target::LevelBuildDispatch), &detourLevelBuildDispatch,
                 reinterpret_cast<void**>(&g_levelBuildDispatch), L"LevelBuildDispatch", HookGroup::Ghost);
}

namespace {

bool diagFlag(const wchar_t* name, std::atomic<int>& cache,
              std::atomic<unsigned long long>& readAt)
{
    const unsigned long long at = GetTickCount64();
    const unsigned long long last = readAt.load(std::memory_order_acquire);
    if (last != 0 && at - last < 500) {
        return cache.load(std::memory_order_relaxed) == 1;
    }
    readAt.store(at, std::memory_order_release);
    std::error_code ec;
    const bool want = std::filesystem::exists(paths::dataDir() / name, ec);
    cache.store(want ? 1 : 2, std::memory_order_release);
    return want;
}

bool diagHooksAll()
{
    static std::atomic<int> cache{0};
    static std::atomic<unsigned long long> readAt{0};
    return diagFlag(L"diag-hooks-all.txt", cache, readAt);
}

bool diagTraceWanted()
{
    static std::atomic<int> frame{0};
    static std::atomic<unsigned long long> frameAt{0};
    return diagFlag(L"diag-frametrace.txt", frame, frameAt);
}

}

std::atomic<bool> g_groupsFrozen{false};

void freezeHookGroups()
{
    g_groupsFrozen.store(true, std::memory_order_release);
}

void refreshHookGroups()
{
    if (g_groupsFrozen.load(std::memory_order_acquire)) {
        return;
    }

    static bool now[static_cast<std::size_t>(HookGroup::Count)] = {};
    static unsigned long long offSince[static_cast<std::size_t>(HookGroup::Count)] = {};
    constexpr unsigned long long kOffDelayMs = 2000;

    const bool all = diagHooksAll();
    const struct {
        HookGroup group;
        bool on;
    } wants[] = {
        {HookGroup::Ghost, all || Schematica::instance().wantsGhostHooks() || blocks::ghostOn()},
        {HookGroup::Diag, all || diagTraceWanted()},
        {HookGroup::Fullbright, all || Fullbright::instance().enabled() || AntiEffect::instance().enabled()},
        {HookGroup::Ability,
         all || CreativeNoClip::instance().enabled() || FlySpeed::instance().enabled()},
        {HookGroup::Tool, all || AutoTool::instance().enabled()},
        {HookGroup::Fog, all || NoRender::instance().fogSuppressed()},
        {HookGroup::Particles, all || NoRender::instance().particlesSuppressed()},
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

    static std::atomic<int> hotCache{0};
    static std::atomic<unsigned long long> hotAt{0};
    const bool hot = all || diagFlag(L"diag-hotcount.txt", hotCache, hotAt);
    if (hot != g_countHotPaths.load(std::memory_order_relaxed)) {
        g_countHotPaths.store(hot, std::memory_order_relaxed);
        setCountingHooks(hot);
        log().info(L"Hooks: counting the hot paths = {}", hot ? L"on" : L"off");
    }
}

void installAll()
{
    const Scanner& scanner = Scanner::instance();
    HookManager& hooks = HookManager::instance();

    hooks.create(scanner.address(Target::GetDestroySpeed), &detourGetDestroySpeed,
                 reinterpret_cast<void**>(&g_getDestroySpeed), L"GetDestroySpeed", HookGroup::Tool);

    hooks.create(scanner.address(Target::SetSelectedSlot), &detourSetSelectedSlot,
                 reinterpret_cast<void**>(&g_setSelectedSlot), L"SetSelectedSlot");

    hooks.create(scanner.address(Target::SubChunkSetBlock), &detourSubChunkSetBlock,
                 reinterpret_cast<void**>(&g_subChunkSetBlock), L"SubChunkSetBlock", HookGroup::Ghost);

    installGhostChunkBuilder();

    hooks.create(scanner.address(Target::ChunkVisibilityScan), &detourChunkVisibilityScan,
                 reinterpret_cast<void**>(&g_chunkVisibilityScan), L"ChunkVisibilityScan", HookGroup::Ghost);

    hooks.create(scanner.address(Target::VisibilityGate), &detourVisibilityGate,
                 reinterpret_cast<void**>(&g_visibilityGate), L"VisibilityGate", HookGroup::Ghost);

    hooks.create(scanner.address(Target::ChunkCoordinatorFrame), &detourChunkCoordinatorFrame,
                 reinterpret_cast<void**>(&g_chunkCoordinatorFrame), L"ChunkCoordinatorFrame");

    hooks.create(scanner.address(Target::BlockRenderLookup), &detourBlockRenderLookup,
                 reinterpret_cast<void**>(&g_blockRenderLookup), L"BlockRenderLookup", HookGroup::Ghost);
    hooks.create(scanner.address(Target::BlockTessellate), &detourBlockTessellate,
                 reinterpret_cast<void**>(&g_blockTessellate), L"BlockTessellate", HookGroup::Ghost);

    if (!hooks.create(scanner.address(Target::ChunkMeshBuild), &detourChunkMeshBuild,
                      reinterpret_cast<void**>(&g_chunkMeshBuild), L"ChunkMeshBuild",
                      HookGroup::Ghost)) {
        log().warn(L"Schematica: the chunk mesh build could not be hooked, so schematic blocks "
                   L"will not be drawn (they are only answered while a chunk mesh is being built)");
    }

    hooks.create(scanner.address(Target::HitResultAssign), &detourHitAssign,
                 reinterpret_cast<void**>(&g_hitAssign), L"HitResultAssign");

    hooks.create(scanner.address(Target::BeRenderLoop), &detourBeRenderLoop,
                 reinterpret_cast<void**>(&g_beRenderLoop), L"BeRenderLoop");

    hooks.create(scanner.address(Target::BlockSourceSetBlock), &detourBlockSourceSetBlock,
                 reinterpret_cast<void**>(&g_blockSourceSetBlock), L"BlockSourceSetBlock");

    hooks.create(scanner.address(Target::BuildBlock), &detourBuildBlock,
                 reinterpret_cast<void**>(&g_buildBlock), L"buildBlock");

    hooks.create(scanner.address(Target::ViewVector), &detourViewVector,
                 reinterpret_cast<void**>(&g_viewVector), L"ViewVector");

    hooks.create(scanner.address(Target::AbilitiesAccess), &detourAbilitiesAccess,
                 reinterpret_cast<void**>(&g_abilitiesAccess), L"AbilitiesAccess", HookGroup::Ability);

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

    hooks.create(scanner.address(Target::GetActorEffect), &detourGetActorEffect,
                 reinterpret_cast<void**>(&g_getActorEffect), L"GetActorEffect", HookGroup::Fullbright);

    g_openInventoryScreen =
        scanner.addressAs<OpenInventoryScreenFn>(Target::OpenInventoryScreen);

    hooks.create(scanner.address(Target::HandleItemStackResponse),
                 &detourHandleItemStackResponse,
                 reinterpret_cast<void**>(&g_handleItemStackResponse),
                 L"HandleItemStackResponse");

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
            log().warn(L"Hooks: could not locate the trade screen push site; the auto trade will open the screen");
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
    if (scanner.found(Target::HudGetItemSite) && scanner.found(Target::ContainerGetItem)
        && hooks.create(scanner.address(Target::ContainerGetItem), &detourContainerGetItem,
                        reinterpret_cast<void**>(&g_containerGetItem), L"ContainerGetItem")) {
        containerui::setHudGetItemSite(scanner.address(Target::HudGetItemSite),
                                       scanner.address(Target::ContainerGetItem));
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

    hooks.create(scanner.address(Target::InventoryHotbarKey), &detourInventoryHotbarKey,
                 reinterpret_cast<void**>(&g_inventoryHotbarKey), L"InventoryHotbarKey");

    hooks.create(scanner.address(Target::UiDefLookup), &detourUiDefLookup,
                 reinterpret_cast<void**>(&g_uiDefLookup), L"UiDefLookup");

    g_uiBagFind = scanner.addressAs<UiBagFindFn>(Target::UiBagFind);

    g_keyDisplayName = scanner.addressAs<KeyDisplayNameFn>(Target::KeyDisplayName);

    hooks.create(scanner.address(Target::SettingsGroupRegister), &detourSettingsGroupRegister,
                 reinterpret_cast<void**>(&g_settingsGroupRegister), L"SettingsGroupRegister");
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

    hooks.create(scanner.address(Target::UiEventDispatch), &detourUiEventDispatch,
                 reinterpret_cast<void**>(&g_uiEventDispatch), L"UiEventDispatch");

    hooks.create(scanner.address(Target::UiSliderPublish), &detourUiSliderPublish,
                 reinterpret_cast<void**>(&g_uiSliderPublish), L"UiSliderPublish");
    resolveCtlBag(scanner.address(Target::UiSliderPublish));

    hooks.create(scanner.address(Target::FogSettingsFetch), &detourFogSettingsFetch,
                 reinterpret_cast<void**>(&g_fogSettingsFetch), L"FogSettingsFetch", HookGroup::Fog);
    hooks.create(scanner.address(Target::ShulkerContentsText), &detourShulkerContentsText,
                 reinterpret_cast<void**>(&g_shulkerContentsText), L"ShulkerContentsText");
    hooks.create(scanner.address(Target::HoverRendererRender), &detourHoverRendererRender,
                 reinterpret_cast<void**>(&g_hoverRendererRender), L"HoverRendererRender");
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

void* uiBagSetFunction()
{
    return reinterpret_cast<void*>(g_uiBagSet);
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

bool offstackScreenAvailable()
{
    return g_scenePush != nullptr && g_tradePushReturn != nullptr;
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
