#include "modules/EffectTimer.h"

#include "core/Logger.h"
#include "game/ContainerUi.h"
#include "game/GameString.h"
#include "game/MobEffectList.h"
#include "game/PlayerListLayout.h"
#include "game/UiProbe.h"
#include "hooks/Detours.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"
#include "memory/Signatures.h"

#include <windows.h>

#include <chrono>
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

namespace tsukuyomi {

namespace {

namespace cui = containerui;
namespace et = efxtimer;

static_assert(et::kBoolSlot + et::kRows <= cui::kPersistentSlots);
static_assert(et::kYSlot + et::kRows <= cui::kPersistentSlots);
static_assert(et::kTextSlot + et::kRows <= cui::kPersistentTextSlots);
static_assert(et::kMaxId == mobeffects::kMaxId);

constexpr std::size_t kRecordSize = 0x24;
constexpr std::size_t kMaxRecords = 0x100;
constexpr const char* kInfiniteKey = "effect.duration.infinite";

constexpr std::size_t kOwnerSiteSlot = 7;
constexpr std::size_t kOwnerSiteDirty = 25;
constexpr std::size_t kOwnerSiteX = 41;
constexpr std::size_t kLoopRecords = 6;
constexpr std::size_t kLoopRecordsEnd = 10;
constexpr std::size_t kBackgroundRect = 8;
constexpr std::size_t kPositionStoreX = 14;
constexpr std::size_t kPositionStoreY = 19;
constexpr std::size_t kPositionStoreDirty = 22;
constexpr std::size_t kGuiDataFieldDisp = 3;
constexpr std::size_t kRectsBackgroundX0 = 4;
constexpr std::size_t kRectsBackgroundY0 = 14;
constexpr std::size_t kRectsIconX0 = 73;
constexpr std::size_t kRectsIconY0 = 83;
constexpr std::size_t kLayoutBoundsMinX = 0x08;

using GuiScaleFn = float(__fastcall*)(void* guiData);

LONG accessFilter(DWORD code)
{
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER
                                                                                 : EXCEPTION_CONTINUE_SEARCH;
}

bool callGuiScaleGuarded(void* fn, void* guiData, float& out)
{
    __try {
        out = reinterpret_cast<GuiScaleFn>(fn)(guiData);
        return true;
    } __except (accessFilter(GetExceptionCode())) {
        return false;
    }
}

void writeBool(int slot, bool value)
{
    if (volatile std::uint8_t* const p = cui::persistentBool(slot)) {
        *p = value ? 1 : 0;
    }
}
void writeFloat(int slot, float value)
{
    if (volatile float* const p = cui::persistentFloat(slot)) {
        *p = value;
    }
}

bool readByte(const std::byte* at, std::uint8_t& out)
{
    return at != nullptr && memory::copyGuarded(at, &out, sizeof(out));
}

bool takeGameString(void* str, std::string& out)
{
    const bool read = gamestring::read(str, out);
    gamestring::release(str);
    return read;
}

std::atomic<int> g_bindLogs{0};
constexpr int kMaxBindLogs = 4;
constexpr int kMaxFailLogs = 4;
std::atomic<bool> g_hudReset{false};

}

EffectTimer& EffectTimer::instance()
{
    static EffectTimer module;
    return module;
}

void EffectTimer::onScansReady()
{
    m_shownKey.fill(INT_MIN);
    mobeffects::resolve();
    auto& scanner = Scanner::instance();
    const std::byte* const owner = scanner.address(Target::MobEffectsRendererOwnerSite);
    const std::byte* const loop = scanner.address(Target::MobEffectsRendererLoop);
    const std::byte* const background = scanner.address(Target::MobEffectsRendererBackground);
    const std::byte* const store = scanner.address(Target::UiControlPositionStore);
    const std::byte* const realGui = scanner.address(Target::GuiDataFieldSite);
    m_guiScaleFn = scanner.address(Target::GuiDataGuiScale);
    bool fields = owner != nullptr && loop != nullptr && background != nullptr && store != nullptr && realGui != nullptr
                  && m_guiScaleFn != nullptr;
    std::uint8_t recordsEnd = 0;
    std::uint8_t ownerX = 0;
    std::uint8_t ownerDirty = 0;
    if (fields) {
        fields = memory::copyGuarded(owner + kOwnerSiteSlot, &m_guiDataSlot, sizeof(m_guiDataSlot))
                 && readByte(owner + kOwnerSiteDirty, ownerDirty) && readByte(owner + kOwnerSiteX, ownerX)
                 && readByte(loop + kLoopRecords, m_recordsField) && readByte(loop + kLoopRecordsEnd, recordsEnd)
                 && readByte(background + kBackgroundRect, m_backgroundField)
                 && readByte(store + kPositionStoreX, m_positionX) && readByte(store + kPositionStoreY, m_positionY)
                 && readByte(store + kPositionStoreDirty, m_dirtyField)
                 && memory::copyGuarded(realGui + kGuiDataFieldDisp, &m_realGuiField, sizeof(m_realGuiField));
    }
    fields = fields && m_guiDataSlot > 0 && m_guiDataSlot < 0x2000 && m_guiDataSlot % 8 == 0
             && recordsEnd == m_recordsField + 8 && ownerX == m_positionX && m_positionY == m_positionX + 4
             && ownerDirty == m_dirtyField && m_backgroundField + 16 <= kRecordSize && m_realGuiField > 0
             && m_realGuiField < 0x10000 && m_realGuiField % 8 == 0;
    const bool bindings = cui::bindingsAvailable() && cui::floatBindingsAvailable() && cui::persistentBool(0) != nullptr;
    const bool hook = scanner.found(Target::MobEffectsRendererRender);
    if (!fields || !bindings || !hook || !mobeffects::ready()) {
        log().warn(L"EffectTimer: NOT usable (renderer {} / fields {} / bindings {} / effect list {})", hook, fields,
                   bindings, mobeffects::ready());
        return;
    }
    if (const std::byte* const rects = scanner.address(Target::MobEffectsLayoutRects);
        rects != nullptr && scanner.found(Target::MobEffectsLayout)) {
        std::uint8_t bgX0 = 0;
        std::uint8_t bgY0 = 0;
        std::uint8_t iconY0 = 0;
        m_columnsReady = readByte(rects + kRectsBackgroundX0, bgX0) && readByte(rects + kRectsBackgroundY0, bgY0)
                         && readByte(rects + kRectsIconX0, m_iconField) && readByte(rects + kRectsIconY0, iconY0)
                         && bgX0 == m_backgroundField && bgY0 == bgX0 + 8 && iconY0 == m_iconField + 8
                         && m_iconField + 16 <= kRecordSize;
    }
    if (!m_columnsReady) {
        log().warn(L"EffectTimer: the effect layout cannot be adjusted; with 2 or more columns the timers overlap the next column");
    }
    hideAll();
    cui::addHudObserver(&EffectTimer::onHudCreated);
    uiprobe::registerDefExtension("hud", "mob_effects_renderer", "", et::layoutJson());
    m_ready.store(true, std::memory_order_relaxed);
    log().info(L"EffectTimer: ready (layout slot {:#x}, records +{:#x}, background +{:#x}, control position +{:#x}/+{:#x}, "
               L"GuiData +{:#x}; the timers appear in worlds entered after injection)",
               m_guiDataSlot, m_recordsField, m_backgroundField, m_positionX, m_positionY, m_realGuiField);
}

void EffectTimer::hideAll()
{
    for (int r = 0; r < et::kRows; ++r) {
        writeBool(et::kBoolSlot + r, false);
    }
}

void EffectTimer::onEnabledChanged(bool)
{
    if (!enabled()) hideAll();
}

void EffectTimer::shutdown()
{
    m_stopping.store(true, std::memory_order_release);
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (m_inside.load(std::memory_order_acquire) != 0 && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    hideAll();
}

void EffectTimer::onHudCreated(void* ctrl)
{
    EffectTimer& self = instance();
    if (!self.m_ready.load(std::memory_order_relaxed)) {
        return;
    }
    g_hudReset.store(true, std::memory_order_release);
    self.hideAll();
    int ok = 0;
    for (int r = 0; r < et::kRows; ++r) {
        ok += cui::bindPersistentBool(ctrl, et::visibleBinding(r).c_str(), et::kBoolSlot + r) ? 1 : 0;
        ok += cui::bindPersistentFloat(ctrl, et::xBinding(r).c_str(), et::kXSlot + r) ? 1 : 0;
        ok += cui::bindPersistentFloat(ctrl, et::yBinding(r).c_str(), et::kYSlot + r) ? 1 : 0;
        ok += cui::bindPersistentText(ctrl, et::textBinding(r).c_str(), et::kTextSlot + r) ? 1 : 0;
    }
    constexpr int kWanted = et::kRows * 4;
    if (g_bindLogs.fetch_add(1, std::memory_order_relaxed) < kMaxBindLogs) {
        if (ok == kWanted) {
            log().info(L"EffectTimer: bound {} values to the HUD controller", ok);
        } else {
            log().warn(L"EffectTimer: bound only {} of {} values to the HUD controller", ok, kWanted);
        }
    }
}

void* EffectTimer::guiDataOf(void* client)
{
    const void* vtable = nullptr;
    if (!memory::copyGuarded(client, &vtable, sizeof(vtable)) || !memory::plausiblePointer(vtable)) {
        return nullptr;
    }
    if (vtable != m_clientVtable) {
        m_clientVtable = vtable;
        m_guiDataField = 0;
        const std::byte* getter = nullptr;
        std::uint8_t code[8]{};
        if (memory::copyGuarded(static_cast<const std::byte*>(vtable) + m_guiDataSlot, &getter, sizeof(getter))
            && memory::copyGuarded(getter, code, sizeof(code)) && code[0] == 0x48 && code[1] == 0x8B && code[2] == 0x81
            && code[7] == 0xC3) {
            std::int32_t field = 0;
            std::memcpy(&field, code + 3, sizeof(field));
            if (field > 0 && field < 0x10000 && field % 8 == 0) {
                m_guiDataField = field;
            }
        }
        if (m_guiDataField == 0) {
            log().warn(L"EffectTimer: the GuiData getter of the client is not `mov rax, [rcx+X]; ret`; timers are not shown");
        }
    }
    void* gui = nullptr;
    if (m_guiDataField <= 0
        || !memory::copyGuarded(static_cast<const std::byte*>(client) + m_guiDataField, &gui, sizeof(gui))
        || !memory::plausiblePointer(gui)) {
        return nullptr;
    }
    return gui;
}

bool EffectTimer::guiScaleOf(void* client, float& scale) const
{
    void* gui = nullptr;
    if (!memory::copyGuarded(static_cast<const std::byte*>(client) + m_realGuiField, &gui, sizeof(gui))
        || !memory::plausiblePointer(gui) || !callGuiScaleGuarded(m_guiScaleFn, gui, scale)) {
        return false;
    }
    return std::isfinite(scale) && scale >= 0.25f && scale <= 64.0f;
}

const std::string& EffectTimer::infiniteText()
{
    if (m_infinite.empty()) {
        alignas(8) unsigned char str[0x20]{};
        if (!hooks::translate(str, kInfiniteKey) || !takeGameString(str, m_infinite) || m_infinite.empty()) {
            m_infinite = kInfiniteKey;
        }
    }
    return m_infinite;
}

void EffectTimer::onRendered(void* client, void* owner)
{
    if (!m_ready.load(std::memory_order_relaxed)) {
        return;
    }
    m_inside.fetch_add(1, std::memory_order_acq_rel);
    if (!m_stopping.load(std::memory_order_acquire)) {
        if (!enabled() || client == nullptr || owner == nullptr || !render(client, owner)) {
            hideAll();
        }
    }
    m_inside.fetch_sub(1, std::memory_order_acq_rel);
}

bool EffectTimer::render(void* client, void* owner)
{
    if (g_hudReset.exchange(false, std::memory_order_acq_rel)) {
        m_shownKey.fill(INT_MIN);
        m_infinite.clear();
    }
    auto fail = [this](const wchar_t* what) {
        if (m_failLogs < kMaxFailLogs) {
            ++m_failLogs;
            log().warn(L"EffectTimer: {} (the timers are hidden while it fails)", what);
        }
        return false;
    };
    const auto* const control = static_cast<const std::byte*>(owner);
    std::uint8_t flags = 0;
    float ownerX = 0;
    float ownerY = 0;
    if (!memory::copyGuarded(control + m_dirtyField, &flags, sizeof(flags))
        || !memory::copyGuarded(control + m_positionX, &ownerX, sizeof(ownerX))
        || !memory::copyGuarded(control + m_positionY, &ownerY, sizeof(ownerY))) {
        return fail(L"the renderer's control position could not be read");
    }
    if ((flags & 1) != 0) {
        return true;
    }
    void* const gui = guiDataOf(client);
    if (gui == nullptr) {
        return fail(L"GuiData could not be read");
    }
    std::uintptr_t range[2] = {};
    if (!memory::copyGuarded(static_cast<const std::byte*>(gui) + m_recordsField, range, sizeof(range))) {
        return fail(L"the effect layout records could not be read");
    }
    const std::size_t bytes = range[1] - range[0];
    if (range[1] < range[0] || bytes % kRecordSize != 0 || bytes / kRecordSize > kMaxRecords
        || (bytes != 0 && !memory::plausiblePointer(range[0]))) {
        return fail(L"the effect layout records look wrong");
    }
    const std::size_t records = bytes / kRecordSize;
    std::vector<std::byte> layout(bytes);
    if (bytes != 0 && !memory::copyGuarded(reinterpret_cast<const void*>(range[0]), layout.data(), bytes)) {
        return fail(L"the effect layout records could not be copied");
    }
    float scale = 0;
    if (!guiScaleOf(client, scale)) {
        return fail(L"the GUI scale could not be read");
    }
    std::vector<mobeffects::Effect> effects;
    if (!mobeffects::read(effects)) {
        return false;
    }

    std::array<bool, et::kRows> shown{};
    for (const mobeffects::Effect& e : effects) {
        if (e.index != e.id || e.id < et::kFirstId || e.id > et::kMaxId || static_cast<std::size_t>(e.id) >= records) {
            continue;
        }
        et::Rect bg;
        std::memcpy(&bg, layout.data() + static_cast<std::size_t>(e.id) * kRecordSize + m_backgroundField, sizeof(bg));
        float x = 0;
        float y = 0;
        if (!et::anchorOf(bg, scale, ownerX, ownerY, x, y)) {
            continue;
        }
        const int row = et::rowOf(e.id);
        const int key = et::secondsKey(e.duration);
        if (key != m_shownKey[row]) {
            std::string text;
            if (e.duration < 0) {
                text = infiniteText();
            } else if (!mobeffects::durationText(e.duration, text)) {
                return fail(L"the duration text function failed");
            }
            if (!cui::writePersistentText(et::kTextSlot + row, text.data(), text.size())) {
                return fail(L"the timer text could not be stored");
            }
            m_shownKey[row] = key;
            m_texts[row] = std::move(text);
        }
        writeFloat(et::kXSlot + row, x);
        writeFloat(et::kYSlot + row, y);
        shown[row] = true;
        if (!m_loggedFrame) {
            m_loggedFrame = true;
            log().info(L"EffectTimer: effect {} duration {} background ({}, {})-({}, {}) px, scale {}, control ({}, {}) -> anchor ({}, {})",
                       e.id, e.duration, bg.x0, bg.y0, bg.x1, bg.y1, scale, ownerX, ownerY, x, y);
        }
    }
    int width = 0;
    for (int r = 0; r < et::kRows; ++r) {
        writeBool(et::kBoolSlot + r, shown[r]);
        if (shown[r]) {
            width = (std::max)(width, playerlist::estimateWidth(m_texts[r]));
        }
    }
    m_textWidth.store(width, std::memory_order_relaxed);
    return true;
}

void EffectTimer::beforeLayout(void* layout)
{
    if (!m_columnsReady || layout == nullptr) {
        return;
    }
    m_inside.fetch_add(1, std::memory_order_acq_rel);
    std::uintptr_t range[2] = {};
    const bool sameRecords = m_shiftedRecords != 0
                             && memory::copyGuarded(static_cast<const std::byte*>(layout) + m_recordsField, range, sizeof(range))
                             && range[0] == m_shiftedRecords && range[1] > range[0];
    if (sameRecords) {
        const std::size_t records = (range[1] - range[0]) / kRecordSize;
        for (std::size_t i = 0; i < m_shifted.size() && i < records; ++i) {
            const Shifted& one = m_shifted[i];
            if (!one.on) continue;
            auto* const record = reinterpret_cast<std::byte*>(range[0] + i * kRecordSize);
            float icon[2]{};
            float bg[2]{};
            if (!memory::copyGuarded(record + m_iconField, icon, sizeof(icon))
                || !memory::copyGuarded(record + m_backgroundField, bg, sizeof(bg)) || icon[0] != one.iconX0) {
                continue;
            }
            icon[0] += one.shiftPx;
            icon[1] += one.shiftPx;
            bg[0] += one.shiftPx;
            bg[1] += one.shiftPx;
            memory::writeGuarded(record + m_iconField, icon, sizeof(icon));
            memory::writeGuarded(record + m_backgroundField, bg, sizeof(bg));
        }
    }
    m_shifted.fill({});
    m_shiftedRecords = 0;
    m_inside.fetch_sub(1, std::memory_order_acq_rel);
}

void EffectTimer::afterLayout(void* layout)
{
    if (!m_columnsReady || layout == nullptr || m_stopping.load(std::memory_order_acquire) || !enabled()) {
        return;
    }
    m_inside.fetch_add(1, std::memory_order_acq_rel);
    shiftColumns(layout);
    m_inside.fetch_sub(1, std::memory_order_acq_rel);
}

void EffectTimer::shiftColumns(void* layout)
{
    auto* const base = static_cast<std::byte*>(layout);
    void* client = nullptr;
    std::uintptr_t range[2] = {};
    if (!memory::copyGuarded(base, &client, sizeof(client)) || !memory::plausiblePointer(client)
        || !memory::copyGuarded(base + m_recordsField, range, sizeof(range)) || range[1] <= range[0]
        || (range[1] - range[0]) % kRecordSize != 0 || (range[1] - range[0]) / kRecordSize > kMaxRecords
        || !memory::plausiblePointer(range[0])) {
        return;
    }
    std::vector<std::uint8_t> listed;
    float scale = 0;
    if (!mobeffects::listedByIndex(listed) || !guiScaleOf(client, scale)) {
        return;
    }
    const std::size_t records = (range[1] - range[0]) / kRecordSize;
    const std::size_t count = (std::min)({listed.size(), records, m_shifted.size()});
    std::vector<std::size_t> placed;
    std::vector<float> tops;
    std::vector<float> iconX;
    for (std::size_t i = 0; i < count; ++i) {
        const auto* const record = reinterpret_cast<const std::byte*>(range[0] + i * kRecordSize);
        float progress = 0;
        float bg[4]{};
        float icon[2]{};
        if (!memory::copyGuarded(record, &progress, sizeof(progress))
            || !memory::copyGuarded(record + m_backgroundField, bg, sizeof(bg))
            || !memory::copyGuarded(record + m_iconField, icon, sizeof(icon))) {
            return;
        }
        if (!(progress > 0.0f) || listed[i] == 0) continue;
        placed.push_back(i);
        tops.push_back(bg[2]);
        iconX.push_back(icon[0]);
    }
    const std::vector<int> columns = et::columnsOf(tops);
    if (columns.empty() || columns.back() == 0) {
        return;
    }
    std::size_t previousHead = 0;
    for (std::size_t k = 1; k < placed.size(); ++k) {
        if (columns[k] != columns[k - 1]) {
            if (!(iconX[k] < iconX[previousHead])) return;
            previousHead = k;
        }
    }
    const int width = m_textWidth.load(std::memory_order_relaxed);
    float minX = 0;
    bool haveMin = false;
    for (std::size_t k = 0; k < placed.size(); ++k) {
        const float shift = et::columnShift(columns[k], width) * scale;
        auto* const record = reinterpret_cast<std::byte*>(range[0] + placed[k] * kRecordSize);
        float icon[2]{};
        float bg[2]{};
        if (!memory::copyGuarded(record + m_iconField, icon, sizeof(icon))
            || !memory::copyGuarded(record + m_backgroundField, bg, sizeof(bg))) {
            return;
        }
        if (shift > 0.0f) {
            icon[0] -= shift;
            icon[1] -= shift;
            bg[0] -= shift;
            bg[1] -= shift;
            if (!memory::writeGuarded(record + m_iconField, icon, sizeof(icon))
                || !memory::writeGuarded(record + m_backgroundField, bg, sizeof(bg))) {
                return;
            }
            m_shifted[placed[k]] = {true, shift, icon[0]};
            m_shiftedRecords = range[0];
        }
        minX = haveMin ? (std::min)(minX, bg[0]) : bg[0];
        haveMin = true;
    }
    if (haveMin) {
        memory::writeGuarded(base + kLayoutBoundsMinX, &minX, sizeof(minX));
    }
    if (!m_loggedShift) {
        m_loggedShift = true;
        log().info(L"EffectTimer: {} columns; each column after the first moves left by {} UI (text width {}, scale {})",
                   columns.back() + 1, et::columnShift(1, width), width, scale);
    }
}

}
