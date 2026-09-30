#include "modules/ShulkerPreview.h"

#include "config/WriteSwitches.h"
#include "core/Logger.h"
#include "core/Strings.h"
#include "game/DurabilityBar.h"
#include "game/GameString.h"
#include "game/TooltipReserve.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>

namespace tsukuyomi {

namespace {

namespace cui = containerui;
namespace tr = tooltipreserve;

using CtorFn = void(__fastcall*)(void* stack);
using LoadFn = void(__fastcall*)(void* stack, const void* tag);
using ItemStackFn = std::uintptr_t(__fastcall*)(const void* item, void* stack);
using DtorFn = void(__fastcall*)(void* stack, int flags);
using DeleteFn = gamestring::DeleteFn;
using GetFontHandleFn = void*(__fastcall*)(void* client, void* out);
using FontOfHandleFn = void*(__fastcall*)(void* handle);
using FontHandleDtorFn = void(__fastcall*)(void* handle);
using UiDrawItemFn = void(__fastcall*)(void* ctx, void* client, void* elem, float x, float y, float scale,
                                       int layer);
using FillFn = void(__fastcall*)(void* ctx, const float* rect, const float* color, float alpha);
using DrawTextFn = void(__fastcall*)(void* ctx, void* font, const float* rect, void* text, const float* color,
                                     float alpha, int align, const void* measure, const void* caret);
using FlushTextFn = void(__fastcall*)(void* ctx, float obfuscation, std::uint64_t alphaOverride);
using TagHashFn = std::uint64_t(__fastcall*)(const void* tag);
using MaxDamageFn = short(__fastcall*)(const void* item);
using DamageValueFn = short(__fastcall*)(const void* stack);

constexpr std::size_t kTagHashSlot = 0x50 / 8;

constexpr std::size_t kElemGlint = 0xBC;
constexpr std::ptrdiff_t kStackItem = 0x08;
constexpr std::ptrdiff_t kStackAux = 0x20;

constexpr float kCell = 18.0f;
constexpr float kGridW = 9 * kCell;
constexpr float kGridH = 3 * kCell;
constexpr float kTextInset = 5.0f;
constexpr float kBoxExtraW = 9.0f;
constexpr float kBoxExtraH = 8.0f;
constexpr float kDurBarX = 3.0f;
constexpr float kDurBarY = 13.7f;
constexpr float kDurBarWidth = 12.0f;
constexpr float kDurBarHeight = 1.0f;
constexpr float kDurShadowExtra = 1.0f;

struct Api {
    CtorFn ctor = nullptr;
    LoadFn load = nullptr;
    int fixupSlot = -1;
    int glintSlot = -1;
    int maxDamageSlot = -1;
    DamageValueFn damageValue = nullptr;
    bool durReady = false;
    bool durBroken = false;
    const void* appendRet = nullptr;
    void** allocatorAt = nullptr;
    DeleteFn gameDelete = nullptr;
    UiDrawItemFn drawItem = nullptr;
    int boxW = -1;
    int boxH = -1;
    int clientFontSlot = -1;
    FontOfHandleFn fontOfHandle = nullptr;
    FontHandleDtorFn fontHandleDtor = nullptr;
    bool textFieldsOk = false;
    TagHashFn tagHash = nullptr;
    int screenInCtx = -1;
    int shaderColor = -1;
    int shaderColorDirty = -1;
    bool tintReady = false;
    bool ready = false;
    bool broken = false;
};
Api g_api;

struct CtxApi {
    const void* vtable = nullptr;
    FillFn fill = nullptr;
    DrawTextFn drawText = nullptr;
    FlushTextFn flushText = nullptr;
};
CtxApi g_ctx;

std::byte* findInFunction(std::byte* fn, std::size_t fallback, std::string_view pattern)
{
    if (fn == nullptr) {
        return nullptr;
    }
    std::size_t size = memory::functionSize(fn);
    if (size == 0) {
        size = fallback;
    }
    if (!memory::isReadable(fn, size)) {
        return nullptr;
    }
    const ScanHit hit = scanRange(std::span<std::byte>(fn, size), pattern);
    return hit.count == 1 ? hit.address : nullptr;
}

bool startsWith(const void* at, std::string_view pattern, std::size_t length)
{
    if (at == nullptr || !memory::inGameModule(at) || !memory::isReadable(at, length)) {
        return false;
    }
    auto* const p = static_cast<std::byte*>(const_cast<void*>(at));
    return scanRange(std::span<std::byte>(p, length), pattern).address == p;
}

void* callTarget(const std::byte* e8)
{
    std::int32_t rel = 0;
    std::memcpy(&rel, e8 + 1, 4);
    void* const target = const_cast<std::byte*>(e8 + 5 + rel);
    return memory::inGameModule(target) ? target : nullptr;
}

int readDisp32(const std::byte* at)
{
    std::int32_t disp = 0;
    std::memcpy(&disp, at, 4);
    return disp;
}

bool readGuarded(const void* at, void* out, std::size_t size)
{
    __try {
        std::memcpy(out, at, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool buildStackGuarded(void* elem, const void* tag)
{
    __try {
        std::memset(elem, 0, 0xC0);
        g_api.ctor(elem);
        g_api.load(elem, tag);
        auto* const bytes = static_cast<std::byte*>(elem);
        void* weak = *reinterpret_cast<void**>(bytes + kStackItem);
        void* item = (weak != nullptr) ? *static_cast<void**>(weak) : nullptr;
        if (item != nullptr) {
            void** const vt = *static_cast<void***>(item);
            reinterpret_cast<ItemStackFn>(vt[g_api.fixupSlot / 8])(item, elem);
        }
        auto* const aux = reinterpret_cast<std::uint16_t*>(bytes + kStackAux);
        if (*aux == 0x7FFF) {
            *aux = 0;
        }
        bool glint = false;
        if (item != nullptr) {
            void** const vt = *static_cast<void***>(item);
            glint = (reinterpret_cast<ItemStackFn>(vt[g_api.glintSlot / 8])(item, elem) & 0xFF) != 0;
        }
        bytes[kElemGlint] = static_cast<std::byte>(glint ? 1 : 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool durabilityGuarded(const void* stack, float& ratio, bool& shown)
{
    __try {
        shown = false;
        auto* const bytes = static_cast<const std::byte*>(stack);
        void* const weak = *reinterpret_cast<void* const*>(bytes + kStackItem);
        void* const item = weak != nullptr ? *static_cast<void* const*>(weak) : nullptr;
        if (item == nullptr) {
            return true;
        }
        void** const vt = *static_cast<void***>(item);
        const int max = reinterpret_cast<MaxDamageFn>(vt[g_api.maxDamageSlot / 8])(item);
        const int damage = g_api.damageValue(stack);
        if (max > 0 && damage > 0) {
            shown = true;
            ratio = std::clamp(static_cast<float>(max - damage) / static_cast<float>(max), 0.0f, 1.0f);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool destroyStackGuarded(void* elem)
{
    __try {
        void** const vt = *static_cast<void***>(elem);
        reinterpret_cast<DtorFn>(vt[0])(elem, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool tagHashGuarded(const void* tag, std::uint64_t& out)
{
    __try {
        void* const* const vt = *static_cast<void* const* const*>(tag);
        if (vt == nullptr || vt[kTagHashSlot] != reinterpret_cast<void*>(g_api.tagHash)) {
            return false;
        }
        out = g_api.tagHash(tag);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

float* shaderColorOf(void* ctx)
{
    std::byte* const screen = *reinterpret_cast<std::byte**>(static_cast<std::byte*>(ctx) + g_api.screenInCtx);
    return screen != nullptr ? *reinterpret_cast<float**>(screen + g_api.shaderColor) : nullptr;
}

bool readShaderColorGuarded(void* ctx, float* out4)
{
    __try {
        const float* const color = shaderColorOf(ctx);
        if (color == nullptr) {
            return false;
        }
        std::memcpy(out4, color, 16);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool writeShaderColorGuarded(void* ctx, const float* in4)
{
    __try {
        float* const color = shaderColorOf(ctx);
        if (color == nullptr) {
            return false;
        }
        std::memcpy(color, in4, 16);
        reinterpret_cast<std::byte*>(color)[g_api.shaderColorDirty] = std::byte{1};
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool fillGuarded(void* ctx, const float* rect, const float* color, float alpha)
{
    __try {
        g_ctx.fill(ctx, rect, color, alpha);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool drawItemGuarded(void* ctx, void* client, void* elem, float x, float y, int layer)
{
    __try {
        g_api.drawItem(ctx, client, elem, x, y, 1.0f, layer);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* fontGuarded(void* client, void* handle)
{
    __try {
        void** const vt = *static_cast<void***>(client);
        reinterpret_cast<GetFontHandleFn>(vt[g_api.clientFontSlot / 8])(client, handle);
        return g_api.fontOfHandle(handle);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool fontHandleDtorGuarded(void* handle)
{
    __try {
        g_api.fontHandleDtor(handle);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool drawTextGuarded(void* ctx, void* font, const float* rect, void* text, const float* color, float alpha,
                     int align, const void* measure, const void* caret)
{
    __try {
        g_ctx.drawText(ctx, font, rect, text, color, alpha, align, measure, caret);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool flushTextGuarded(void* ctx)
{
    __try {
        g_ctx.flushText(ctx, 0.0f, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void checkContext(void* ctx)
{
    const void* vt = nullptr;
    if (!readGuarded(ctx, &vt, 8) || vt == nullptr || !memory::inGameModule(vt)) {
        return;
    }
    if (vt == g_ctx.vtable) {
        return;
    }
    g_ctx = CtxApi{};
    g_ctx.vtable = vt;
    void* slots[16]{};
    if (!readGuarded(vt, slots, sizeof(slots))) {
        return;
    }
    if (startsWith(slots[15], "41 56 56 57 53 48 83 EC 68 0F 29 74 24 50 0F 28 F3 4C 89 C3 48 89 D7 48 89 CE 48 8B 05",
                   30)) {
        g_ctx.fill = reinterpret_cast<FillFn>(slots[15]);
    }
    if (startsWith(slots[5],
                   "41 56 56 57 53 48 83 EC 68 4C 89 C0 49 89 D0 48 8B B4 24 D0 00 00 00 4C 8B 9C 24 C8 00 00 00 "
                   "4C 8B 94 24 B0 00 00 00 48 8B 91 88 00 00 00 48 3B",
                   48)) {
        g_ctx.drawText = reinterpret_cast<DrawTextFn>(slots[5]);
    }
    if (startsWith(slots[6], "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC", 15)
        && findInFunction(static_cast<std::byte*>(slots[6]), 0x800,
                          "4C 89 85 ? ? ? ? F3 0F 11 8D ? ? ? ? 48 89 8D ? ? ? ? 48 8B 41 10")
               != nullptr) {
        g_ctx.flushText = reinterpret_cast<FlushTextFn>(slots[6]);
    }
    log().info(L"ShulkerPreview: UI render context: fill {} / drawText {} / flushText {}", g_ctx.fill != nullptr,
               g_ctx.drawText != nullptr, g_ctx.flushText != nullptr);
}

struct ShortString {
    char text[16]{};
    std::uint64_t size = 0;
    std::uint64_t cap = 15;
};
static_assert(sizeof(ShortString) == 32);

}

void ShulkerPreview::resolveTooltip()
{
    Scanner& scanner = Scanner::instance();
    Api api;
    std::byte* const contents = scanner.address(Target::ShulkerContentsText);
    std::byte* const append = scanner.address(Target::ShulkerHoverAppend);
    std::byte* const render = scanner.address(Target::HoverRendererRender);
    std::byte* const boxSize = scanner.address(Target::HoverBoxSizeStore);
    std::byte* const glintSite = scanner.address(Target::ItemGlintSlotSite);
    std::byte* const maxDamageSite = scanner.address(Target::ItemMaxDamageSlotSite);
    api.drawItem = scanner.addressAs<UiDrawItemFn>(Target::UiDrawItem);
    if (std::byte* at = findInFunction(contents, 0xC00, "48 8D 4D ? E8 ? ? ? ? 48 8D 4D ? 48 89 F2 E8 ? ? ? ?")) {
        api.ctor = reinterpret_cast<CtorFn>(callTarget(at + 4));
        api.load = reinterpret_cast<LoadFn>(callTarget(at + 16));
    }
    if (std::byte* at = findInFunction(contents, 0xC00, "48 8B 01 48 8B 80 ? ? ? ? 48 8D 55")) {
        api.fixupSlot = readDisp32(at + 6);
    }
    if (glintSite != nullptr && memory::isReadable(glintSite + 41, 4)) {
        api.glintSlot = readDisp32(glintSite + 41);
    }
    if (maxDamageSite != nullptr && memory::isReadable(maxDamageSite + 6, 4)) {
        api.maxDamageSlot = readDisp32(maxDamageSite + 6);
    }
    api.damageValue = scanner.addressAs<DamageValueFn>(Target::ItemStackDamageValue);
    if (append != nullptr && contents != nullptr && memory::isReadable(append, 0x80)) {
        for (std::size_t i = 0; i + 5 <= 0x80; ++i) {
            if (static_cast<std::uint8_t>(append[i]) == 0xE8 && callTarget(append + i) == contents) {
                api.appendRet = append + i + 5;
                break;
            }
        }
    }
    if (std::byte* at = findInFunction(append, 0x400, "4D 8D 7C 24 01 48 8B 0D ? ? ? ? 48 8B 01 48 8B 40 08 4C 89 FA FF 15")) {
        api.allocatorAt = static_cast<void**>(memory::ripTarget(at, 8));
    }
    if (append != nullptr) {
        const std::size_t size = memory::functionSize(append) != 0 ? memory::functionSize(append) : 0x400;
        if (memory::isReadable(append, size)) {
            const ScanHit hit = scanRange(std::span<std::byte>(append, size), "48 83 C0 28 48 89 C2 4C 89 C1 E8");
            if (hit.address != nullptr) {
                api.gameDelete = reinterpret_cast<DeleteFn>(callTarget(hit.address + 10));
            }
        }
    }
    if (boxSize != nullptr && memory::isReadable(boxSize, 10)) {
        api.boxW = static_cast<std::uint8_t>(boxSize[4]);
        api.boxH = static_cast<std::uint8_t>(boxSize[9]);
    }
    if (std::byte* at = findInFunction(render, 0x400,
                                       "48 8B 80 ? ? ? ? 4C 8D 75 ? 48 89 D9 4C 89 F2 FF 15 ? ? ? ? 4C 89 F1 E8")) {
        api.clientFontSlot = readDisp32(at + 3);
        api.fontOfHandle = reinterpret_cast<FontOfHandleFn>(callTarget(at + 26));
    }
    if (std::byte* at = findInFunction(render, 0x400, "48 8D 4D ? E8 ? ? ? ? 0F 28 B5")) {
        api.fontHandleDtor = reinterpret_cast<FontHandleDtorFn>(callTarget(at + 4));
    }
    api.textFieldsOk = findInFunction(render, 0x400, "48 8B 4F 40 48 83 7F 48 10 72 ? 48 8B 7F 30") != nullptr;
    api.tagHash = scanner.addressAs<TagHashFn>(Target::CompoundTagHash);
    std::byte* const fillSite = scanner.address(Target::ShaderColorFillSite);
    std::byte* const tintSite = scanner.address(Target::GlintTintSite);
    if (fillSite != nullptr && tintSite != nullptr && memory::isReadable(fillSite, 14) && memory::isReadable(tintSite, 8)
        && tintSite[7] == fillSite[3] && fillSite[13] == std::byte{1}) {
        api.shaderColor = static_cast<std::uint8_t>(fillSite[3]);
        api.shaderColorDirty = static_cast<std::uint8_t>(fillSite[12]);
    }
    if (std::byte* at = findInFunction(reinterpret_cast<std::byte*>(api.drawItem), 0x400,
                                       "48 8B 56 ? 4C 8D 7D ? 4C 89 F9 4D 89 F0 49 89 C1 E8")) {
        api.screenInCtx = static_cast<std::uint8_t>(at[3]);
    }
    api.tintReady = api.shaderColor > 0 && api.shaderColorDirty > 0 && api.screenInCtx > 0;

    auto slotOk = [](int slot) { return slot > 0 && slot < 0x1000 && slot % 8 == 0; };
    api.durReady = slotOk(api.maxDamageSlot) && api.damageValue != nullptr;
    api.ready = api.ctor != nullptr && api.load != nullptr && slotOk(api.fixupSlot) && slotOk(api.glintSlot)
                && api.appendRet != nullptr && api.allocatorAt != nullptr && api.gameDelete != nullptr
                && api.drawItem != nullptr && api.boxW > 0x40 && api.boxH > 0x40 && slotOk(api.clientFontSlot)
                && api.fontOfHandle != nullptr && api.fontHandleDtor != nullptr && api.textFieldsOk
                && render != nullptr;
    g_api = api;
    gamestring::configure(api.allocatorAt, api.gameDelete);
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    auto rva = [base](const void* p) { return p != nullptr ? reinterpret_cast<std::uintptr_t>(p) - base : 0; };
    log().info(L"ShulkerPreview: tooltip parts {} (ctor {:#x} load {:#x} fixup +{:#x} glint +{:#x} ret {:#x} "
               L"alloc {:#x} delete {:#x} draw {:#x} box +{:#x}/+{:#x} font +{:#x} {:#x}/{:#x} text {} tag hash {:#x} "
               L"glint tint {} +{:#x}/+{:#x}/+{:#x} durability {} slot +{:#x} damage {:#x})",
               api.ready ? L"ready" : L"NOT usable", rva(reinterpret_cast<const void*>(api.ctor)),
               rva(reinterpret_cast<const void*>(api.load)), api.fixupSlot, api.glintSlot, rva(api.appendRet),
               rva(api.allocatorAt), rva(reinterpret_cast<const void*>(api.gameDelete)),
               rva(reinterpret_cast<const void*>(api.drawItem)), api.boxW, api.boxH, api.clientFontSlot,
               rva(reinterpret_cast<const void*>(api.fontOfHandle)),
               rva(reinterpret_cast<const void*>(api.fontHandleDtor)), api.textFieldsOk,
               rva(reinterpret_cast<const void*>(api.tagHash)), api.tintReady, api.screenInCtx, api.shaderColor,
               api.shaderColorDirty, api.durReady, api.maxDamageSlot,
               rva(reinterpret_cast<const void*>(api.damageValue)));
}

bool ShulkerPreview::tooltipAvailable() const
{
    return g_api.ready && !g_api.broken && Scanner::instance().found(Target::ShulkerContentsText)
           && Scanner::instance().found(Target::HoverRendererRender) && !writes::blocked("ShulkerPreview:tooltip");
}

void ShulkerPreview::freeSetLocked(ContentSet& set)
{
    for (int i = 0; i < kSlots; ++i) {
        const auto at = static_cast<std::size_t>(i);
        if (!set.live[at]) {
            continue;
        }
        if (!destroyStackGuarded(set.elems[at].bytes)) {
            g_api.broken = true;
            log().error(L"ShulkerPreview: destroying a preview item faulted; the tooltip preview is turned off");
        }
        set.live[at] = false;
    }
    set.durRatio.fill(0.0f);
    set.durShown.fill(false);
    set.id = 0;
    set.key.clear();
    set.usedAt = 0;
}

void ShulkerPreview::freeStacksLocked()
{
    for (ContentSet& set : m_sets) {
        freeSetLocked(set);
    }
    m_missingId = 0;
}

void ShulkerPreview::freeStacks()
{
    const std::lock_guard<std::mutex> lock(m_stacksLock);
    freeStacksLocked();
}

void ShulkerPreview::forgetSetKeys()
{
    const std::lock_guard<std::mutex> lock(m_stacksLock);
    for (ContentSet& set : m_sets) {
        set.key.clear();
    }
}

ShulkerPreview::ContentSet* ShulkerPreview::findSetLocked(std::uint32_t id)
{
    if (id == 0) {
        return nullptr;
    }
    for (ContentSet& set : m_sets) {
        if (set.id == id) {
            return &set;
        }
    }
    return nullptr;
}

ShulkerPreview::ContentSet* ShulkerPreview::acquireSetLocked(const std::string& key,
                                                            const std::vector<containerui::NbtItem>& items)
{
    for (ContentSet& set : m_sets) {
        if (set.id != 0 && set.key == key) {
            set.usedAt = ++m_setUseSeq;
            return &set;
        }
    }
    ContentSet* victim = &m_sets[0];
    for (ContentSet& set : m_sets) {
        if (set.id == 0) {
            victim = &set;
            break;
        }
        if (set.usedAt < victim->usedAt) {
            victim = &set;
        }
    }
    freeSetLocked(*victim);
    int built = 0;
    for (const cui::NbtItem& one : items) {
        if (one.slot < 0 || one.slot >= kSlots || one.elem == nullptr || victim->live[static_cast<std::size_t>(one.slot)]) {
            continue;
        }
        const auto at = static_cast<std::size_t>(one.slot);
        if (!buildStackGuarded(victim->elems[at].bytes, one.elem)) {
            g_api.broken = true;
            log().error(L"ShulkerPreview: building a preview item faulted; the tooltip preview is turned off");
            victim->live.fill(false);
            return nullptr;
        }
        victim->live[at] = true;
        victim->counts[at] = one.count;
        if (g_api.durReady && !g_api.durBroken
            && !durabilityGuarded(victim->elems[at].bytes, victim->durRatio[at], victim->durShown[at])) {
            g_api.durBroken = true;
            log().error(L"ShulkerPreview: reading item durability faulted; durability bars are turned off");
        }
        ++built;
    }
    if (built == 0) {
        return nullptr;
    }
    victim->id = m_nextSetId++;
    if (m_nextSetId == 0) {
        m_nextSetId = 1;
    }
    victim->key = key;
    victim->usedAt = ++m_setUseSeq;
    return victim;
}

void ShulkerPreview::onContentsText(void* out, const void* tag, const void* returnAddress)
{
    if (!tooltipAvailable() || out == nullptr || tag == nullptr || returnAddress != g_api.appendRet) {
        return;
    }
    if (!wantPreview() || (cui::available() && !m_screenOpen.load(std::memory_order_acquire))) {
        return;
    }
    std::vector<cui::NbtItem> items;
    if (!cui::nbtItemsOfTag(tag, items) || items.empty()) {
        return;
    }
    std::string key;
    key.reserve(items.size() * 48);
    bool allHashed = true;
    for (const cui::NbtItem& one : items) {
        key += std::to_string(one.slot) + ':' + one.name + ':' + std::to_string(one.count) + ':'
               + std::to_string(one.aux) + (one.enchanted ? "e" : "");
        std::uint64_t hash = 0;
        if (g_api.tagHash != nullptr && one.elem != nullptr && tagHashGuarded(one.elem, hash)) {
            key += '#' + std::to_string(hash);
        } else {
            allHashed = false;
            key += '@' + std::to_string(reinterpret_cast<std::uintptr_t>(one.elem));
        }
        key += ';';
    }
    if (!allHashed && !m_addressKeyed.exchange(true)) {
        log().warn(L"ShulkerPreview: the NBT content hash could not be used; the tooltip contents are keyed by "
                   L"address instead (the keys are forgotten whenever an item is moved)");
    }
    std::uint32_t id = 0;
    {
        const std::lock_guard<std::mutex> lock(m_stacksLock);
        const ContentSet* const set = g_api.broken ? nullptr : acquireSetLocked(key, items);
        if (set == nullptr || g_api.broken) {
            return;
        }
        id = set->id;
    }
    const std::string text = tr::reserveText(m_reserveRows.load(), m_reserveSpaces.load(), id);
    if (!gamestring::assign(out, text)) {
        static bool told = false;
        if (!told) {
            told = true;
            log().warn(L"ShulkerPreview: could not write the reserved tooltip lines; the vanilla list stays");
        }
        return;
    }
    if (!m_loggedTooltip) {
        m_loggedTooltip = true;
        log().info(L"ShulkerPreview: reserved {} line(s) x {} space(s) in the tooltip for {} item stack(s)",
                   m_reserveRows.load(), m_reserveSpaces.load(), items.size());
    }
}

void ShulkerPreview::onHoverRender(void* self, void* ctx, void* client, void* owner)
{
    if (!g_api.ready || g_api.broken || self == nullptr || ctx == nullptr || client == nullptr || owner == nullptr
        || !wantPreview()) {
        return;
    }
    auto* const box = static_cast<std::byte*>(self);
    std::uint64_t size = 0;
    std::uint64_t cap = 0;
    if (!readGuarded(box + 0x40, &size, 8) || !readGuarded(box + 0x48, &cap, 8) || size == 0 || size > cap
        || size > 0x4000) {
        return;
    }
    const char* data = reinterpret_cast<const char*>(box + 0x30);
    if (cap >= 16 && !readGuarded(box + 0x30, &data, 8)) {
        return;
    }
    std::string text(static_cast<std::size_t>(size), '\0');
    if (data == nullptr || !readGuarded(data, text.data(), text.size())) {
        return;
    }
    int line = 0;
    int lines = 0;
    int reservedRows = 0;
    int shownSpaces = 0;
    std::uint32_t id = 0;
    if (!tr::findMarker(text, line, lines, reservedRows, shownSpaces, id)) {
        return;
    }
    float f[4]{};
    float alpha = 1.0f;
    float bw = 0.0f;
    float bh = 0.0f;
    if (!readGuarded(box + 0x50, f, sizeof(f)) || !readGuarded(box + 0x08, &alpha, 4)
        || !readGuarded(box + g_api.boxW, &bw, 4) || !readGuarded(box + g_api.boxH, &bh, 4)) {
        return;
    }
    const float x = f[0] + f[2];
    const float y = f[1] + f[3];
    if (!std::isfinite(x) || !std::isfinite(y) || !(bw > 0.0f) || !(bh > kBoxExtraH) || lines <= 0) {
        return;
    }
    const float lineH = (bh - kBoxExtraH) / static_cast<float>(lines);
    if (!(lineH >= 4.0f && lineH <= 64.0f)) {
        return;
    }
    const std::lock_guard<std::mutex> lock(m_stacksLock);
    ContentSet* const set = findSetLocked(id);
    if (set == nullptr) {
        const unsigned long long now = GetTickCount64();
        if (id != m_missingId && now - m_missingAt >= 500) {
            m_missingId = id;
            m_missingAt = now;
            cui::requestRefresh();
            static int told = 0;
            if (told < 3) {
                ++told;
                log().info(L"ShulkerPreview: the tooltip names contents set {} that was already dropped; "
                           L"asking the game to rebuild the text",
                           id);
            }
        }
        return;
    }
    set->usedAt = ++m_setUseSeq;
    const int wantRows = std::max(tr::rowsFor(lineH, kGridH + 2.0f), 1);
    const float innerW = bw - kBoxExtraW;
    const int wantSpaces = std::min(tr::spacesFor(shownSpaces, innerW, kGridW + 4.0f), 400);
    m_reserveRows.store(wantRows);
    m_reserveSpaces.store(std::max(wantSpaces, 1));
    if (innerW < kGridW + 2.0f || static_cast<float>(reservedRows) * lineH < kGridH + 1.0f) {
        if (wantRows != reservedRows || wantSpaces != shownSpaces) {
            cui::requestRefresh();
        }
        return;
    }
    checkContext(ctx);
    int layer = 0;
    readGuarded(static_cast<std::byte*>(owner) + 0x64, &layer, 4);

    float savedColor[4]{};
    const bool colorSaved = g_api.tintReady && readShaderColorGuarded(ctx, savedColor);
    struct RestoreColor {
        void* ctx;
        const float* color;
        bool active;
        ~RestoreColor()
        {
            if (active) {
                writeShaderColorGuarded(ctx, color);
            }
        }
    } restoreColor{ctx, savedColor, colorSaved};

    const float gx = std::floor(x + kTextInset);
    const float gy = std::floor(y + kTextInset + static_cast<float>(line) * lineH) + 1.0f;
    if (g_ctx.fill != nullptr) {
        static const float kDark[4] = {0.216f, 0.216f, 0.216f, 1.0f};
        static const float kLight[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        static const float kMid[4] = {0.545f, 0.545f, 0.545f, 1.0f};
        for (int i = 0; i < kSlots; ++i) {
            const float cx = gx + static_cast<float>(i % 9) * kCell;
            const float cy = gy + static_cast<float>(i / 9) * kCell;
            const float outer[4] = {cx, cx + kCell, cy, cy + kCell};
            const float lower[4] = {cx + 1.0f, cx + kCell, cy + 1.0f, cy + kCell};
            const float inner[4] = {cx + 1.0f, cx + kCell - 1.0f, cy + 1.0f, cy + kCell - 1.0f};
            if (!fillGuarded(ctx, outer, kDark, alpha) || !fillGuarded(ctx, lower, kLight, alpha)
                || !fillGuarded(ctx, inner, kMid, alpha)) {
                g_ctx.fill = nullptr;
                log().error(L"ShulkerPreview: fillRectangle faulted; cells are no longer drawn");
                break;
            }
        }
    }
    if (colorSaved) {
        float before[4]{};
        const bool readBefore = readShaderColorGuarded(ctx, before);
        const float white[4] = {1.0f, 1.0f, 1.0f, alpha};
        if (!writeShaderColorGuarded(ctx, white)) {
            restoreColor.active = false;
        } else if (!m_loggedTint && readBefore) {
            m_loggedTint = true;
            log().info(L"ShulkerPreview: the glint tint was ({:.3f}, {:.3f}, {:.3f}, {:.3f}) after the cells; the items "
                       L"are drawn with (1, 1, 1, {:.3f}) like the inventory",
                       before[0], before[1], before[2], before[3], alpha);
        }
    }
    for (int i = 0; i < kSlots; ++i) {
        if (!set->live[static_cast<std::size_t>(i)]) {
            continue;
        }
        const float cx = gx + static_cast<float>(i % 9) * kCell + 1.0f;
        const float cy = gy + static_cast<float>(i / 9) * kCell + 1.0f;
        if (!drawItemGuarded(ctx, client, set->elems[static_cast<std::size_t>(i)].bytes, cx, cy, layer)) {
            g_api.broken = true;
            log().error(L"ShulkerPreview: drawing a preview item faulted; the tooltip preview is turned off");
            return;
        }
    }
    if (g_api.durReady && !g_api.durBroken && g_ctx.fill != nullptr) {
        static const float kBlack[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        for (int i = 0; i < kSlots; ++i) {
            const auto at = static_cast<std::size_t>(i);
            if (!set->live[at] || !set->durShown[at]) {
                continue;
            }
            const float cx = gx + static_cast<float>(i % 9) * kCell;
            const float cy = gy + static_cast<float>(i / 9) * kCell;
            const float x0 = cx + kDurBarX;
            const float y0 = cy + kDurBarY;
            const float shadow[4] = {x0, x0 + kDurBarWidth + kDurShadowExtra,
                                     y0, y0 + kDurBarHeight + kDurShadowExtra};
            const float ground[4] = {x0, x0 + kDurBarWidth, y0, y0 + kDurBarHeight};
            const float width = durabilityBarWidth(set->durRatio[at], kDurBarWidth);
            const float body[4] = {x0, x0 + width, y0, y0 + kDurBarHeight};
            const DurabilityBarColor rgb = durabilityBarColor(set->durRatio[at]);
            const float color[4] = {rgb.r, rgb.g, rgb.b, 1.0f};
            if (!fillGuarded(ctx, shadow, kBlack, alpha) || !fillGuarded(ctx, ground, kBlack, alpha)
                || !fillGuarded(ctx, body, color, alpha)) {
                g_api.durBroken = true;
                log().error(L"ShulkerPreview: fillRectangle faulted; durability bars are no longer drawn");
                break;
            }
        }
    }
    if (g_ctx.drawText == nullptr || g_ctx.flushText == nullptr) {
        return;
    }
    void* queueBegin = nullptr;
    void* queueEnd = nullptr;
    const bool queueEmpty = readGuarded(static_cast<std::byte*>(ctx) + 0x80, &queueBegin, 8)
                            && readGuarded(static_cast<std::byte*>(ctx) + 0x88, &queueEnd, 8) && queueBegin == queueEnd;
    alignas(16) std::byte handle[0x80]{};
    void* const font = fontGuarded(client, handle);
    if (font == nullptr) {
        return;
    }
    struct Measure {
        float fontSize = 1.0f;
        float linePadding = 0.0f;
        bool shadow = true;
        bool showSymbols = false;
        bool hideHyphen = false;
    } measure;
    struct Caret {
        int position = -1;
        bool render = false;
    } caret;
    static const float kWhite[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    constexpr int kAlignRight = 1;
    bool drewText = false;
    for (int i = 0; i < kSlots; ++i) {
        const auto at = static_cast<std::size_t>(i);
        if (!set->live[at] || set->counts[at] < 2) {
            continue;
        }
        const float cx = gx + static_cast<float>(i % 9) * kCell;
        const float cy = gy + static_cast<float>(i / 9) * kCell;
        const float rect[4] = {cx, cx + kCell, cy + 10.0f, cy + kCell + 1.0f};
        ShortString s;
        const std::string n = std::to_string(set->counts[at]);
        std::memcpy(s.text, n.data(), std::min<std::size_t>(n.size(), 15));
        s.size = std::min<std::size_t>(n.size(), 15);
        if (!drawTextGuarded(ctx, font, rect, &s, kWhite, alpha, kAlignRight, &measure, &caret)) {
            g_ctx.drawText = nullptr;
            log().error(L"ShulkerPreview: drawText faulted; item counts are no longer drawn");
            break;
        }
        drewText = true;
    }
    if (drewText && queueEmpty && !flushTextGuarded(ctx)) {
        g_ctx.flushText = nullptr;
        log().error(L"ShulkerPreview: flushText faulted; item counts are no longer drawn");
    }
    fontHandleDtorGuarded(handle);
}

}
