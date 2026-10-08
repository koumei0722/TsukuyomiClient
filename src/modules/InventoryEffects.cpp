#include "modules/InventoryEffects.h"

#include "core/Logger.h"
#include "core/Notice.h"
#include "core/Strings.h"
#include "game/GameData.h"
#include "game/GameString.h"
#include "game/InventoryEffectsLayout.h"
#include "game/InventoryEffectsLogic.h"
#include "game/MobEffectList.h"
#include "game/UiProbe.h"
#include "hooks/Detours.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"
#include "memory/Signatures.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace tsukuyomi {

namespace {

namespace cui = containerui;

constexpr std::size_t kEffectIconName = 0xb0;
constexpr std::size_t kEffectIconOverride = 0xd0;

constexpr const char* kInfiniteKey = "effect.duration.infinite";
constexpr const char* kBackground = "textures/ui/effect_background";
constexpr const char* kBackgroundAmbient = "textures/ui/effect_background_ambient";

bool takeGameString(void* str, std::string& out)
{
    const bool read = gamestring::read(str, out);
    gamestring::release(str);
    return read;
}

using RawEffect = mobeffects::Effect;

}

InventoryEffects& InventoryEffects::instance()
{
    static InventoryEffects module;
    return module;
}

bool InventoryEffects::available() const
{
    return m_ready && cui::available();
}

void InventoryEffects::onScansReady()
{
    auto& scanner = Scanner::instance();
    mobeffects::resolve();
    const bool fields = scanner.found(Target::MobEffectIconNameSite) && scanner.found(Target::MobEffectColorStore);
    cui::onScansReady();
    m_ready = mobeffects::ready() && fields && cui::available();
    if (!m_ready) {
        log().warn(L"InventoryEffects: NOT usable (list {} / name {} / duration {} / fields {} / strings {} / screens {})",
                   scanner.found(Target::MobEffectScreenListLoop), scanner.found(Target::MobEffectInstanceDisplayName),
                   scanner.found(Target::MobEffectDurationText), fields, gamestring::available(), cui::available());
        return;
    }
    cui::addObserver(this);
    uiprobe::registerDefExtension("crafting", "recipe_inventory_screen_content", "", invfx::layoutJson());
    uiprobe::registerDefProperty("crafting", "inventory_screen", invfx::kIgnoreVariable, "false");
    uiprobe::registerDefProperty("crafting", "crafting_screen", invfx::kIgnoreVariable, "true");
    log().info(L"InventoryEffects: ready");
}

void InventoryEffects::shutdown()
{
    cui::removeObserver(this);
}

void InventoryEffects::onScreenCreated(void* ctrl)
{
    clearRows();
    m_names.clear();
    m_icons.clear();
    m_infinite.clear();
    int bound = cui::bindBool(ctrl, invfx::kBindOn, &InventoryEffects::listOn, 0) ? 1 : 0;
    for (int row = 0; row < invfx::kMaxRows; ++row) {
        const auto arg = static_cast<std::uintptr_t>(row);
        bound += cui::bindBool(ctrl, invfx::rowBinding(row).c_str(), &InventoryEffects::rowOn, arg) ? 1 : 0;
        bound += cui::bindFloat(ctrl, invfx::yBinding(row).c_str(), &InventoryEffects::rowY, arg) ? 1 : 0;
        bound += cui::bindLongText(ctrl, invfx::nameBinding(row).c_str(), &InventoryEffects::rowName, arg) ? 1 : 0;
        bound += cui::bindLongText(ctrl, invfx::timeBinding(row).c_str(), &InventoryEffects::rowTime, arg) ? 1 : 0;
        bound += cui::bindLongText(ctrl, invfx::iconBinding(row).c_str(), &InventoryEffects::rowIcon, arg) ? 1 : 0;
        bound += cui::bindLongText(ctrl, invfx::bgBinding(row).c_str(), &InventoryEffects::rowBg, arg) ? 1 : 0;
        bound += cui::bindLongText(ctrl, invfx::tipBinding(row).c_str(), &InventoryEffects::rowTip, arg) ? 1 : 0;
    }
    constexpr int kExpected = 1 + invfx::kMaxRows * 7;
    static int logs = 0;
    if (logs < 3) {
        ++logs;
        if (bound == kExpected) {
            log().info(L"InventoryEffects: {} bindings were registered on the screen", bound);
        } else {
            log().warn(L"InventoryEffects: only {} of {} bindings were registered (float bindings {})", bound, kExpected,
                       cui::floatBindingsAvailable() ? L"available" : L"NOT available");
        }
    }
}

void InventoryEffects::onScreenTick()
{
    refresh();
}

void InventoryEffects::onScreenLost()
{
    clearRows();
}

void InventoryEffects::clearRows()
{
    m_rows.clear();
    m_shownIds.clear();
    m_shownOn = false;
}

const std::string& InventoryEffects::nameOf(int id, int amplifier)
{
    const int key = (id << 8) | (amplifier & 0xff);
    if (const auto it = m_names.find(key); it != m_names.end()) {
        return it->second;
    }
    std::string text;
    if (!mobeffects::displayName(id, amplifier, text)) {
        notice::failOnce("InventoryEffects:name", L"InventoryEffects: MobEffectInstance::getDisplayName failed; names are not shown",
                         "InventoryEffects could not read effect names");
        text.clear();
    }
    return m_names.emplace(key, std::move(text)).first->second;
}

std::string InventoryEffects::timeOf(int duration)
{
    if (duration == -1) {
        if (m_infinite.empty()) {
            alignas(8) unsigned char str[0x20]{};
            if (!hooks::translate(str, kInfiniteKey) || !takeGameString(str, m_infinite) || m_infinite.empty()) {
                m_infinite = kInfiniteKey;
            }
        }
        return m_infinite;
    }
    std::string text;
    if (!mobeffects::durationText(duration, text)) {
        notice::failOnce("InventoryEffects:duration", L"InventoryEffects: the duration text function failed; durations are not shown",
                         "InventoryEffects could not format effect durations");
        text.clear();
    }
    return text;
}

const std::string& InventoryEffects::iconOf(int id, const void* effect)
{
    if (const auto it = m_icons.find(id); it != m_icons.end()) {
        return it->second;
    }
    std::string name;
    std::string over;
    auto* const base = static_cast<char*>(const_cast<void*>(effect));
    if (gamestring::read(base + kEffectIconOverride, over) && !over.empty()) {
        name = std::move(over);
    } else if (!gamestring::read(base + kEffectIconName, name)) {
        name.clear();
    }
    std::string path = name.empty() ? std::string() : invfx::iconPath(name);
    return m_icons.emplace(id, std::move(path)).first->second;
}

void InventoryEffects::refresh()
{
    std::vector<RawEffect> raw;
    if (enabled()) {
        if (!mobeffects::read(raw) && !m_failLogged) {
            m_failLogged = true;
            log().warn(L"InventoryEffects: the player's effects could not be read (nothing is shown until they can)");
        }
    }
    std::vector<invfx::EffectKey> keys;
    keys.reserve(raw.size());
    for (const RawEffect& e : raw) {
        keys.push_back({e.id, e.duration, e.ambient, e.color});
    }
    std::vector<std::size_t> order(raw.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&keys](std::size_t a, std::size_t b) {
        return invfx::compareLikeJava(keys[a], keys[b]) < 0;
    });
    if (order.size() > static_cast<std::size_t>(invfx::kMaxRows)) {
        order.resize(invfx::kMaxRows);
    }

    std::vector<Row> rows;
    rows.reserve(order.size());
    std::vector<int> ids;
    for (const std::size_t i : order) {
        const RawEffect& e = raw[i];
        Row row;
        row.id = e.id;
        row.duration = e.duration;
        row.amplifier = e.amplifier;
        row.ambient = e.ambient;
        row.color = e.color;
        row.name = nameOf(e.id, e.amplifier);
        row.time = timeOf(e.duration);
        row.icon = iconOf(e.id, e.effect);
        row.tip = row.name + "\n" + row.time;
        rows.push_back(std::move(row));
        ids.push_back((e.id << 8) | (e.amplifier & 0xff) | (e.ambient ? 0x10000 : 0));
    }
    m_rows = std::move(rows);
    if (!m_listLogged && !m_rows.empty()) {
        m_listLogged = true;
        for (const Row& row : m_rows) {
            log().info(L"InventoryEffects: effect {} amplifier {} duration {} ambient {} -> \"{}\" / \"{}\" / {}", row.id,
                       row.amplifier, row.duration, row.ambient, toUtf16(row.name), toUtf16(row.time), toUtf16(row.icon));
        }
    }
    const bool on = enabled() && !m_rows.empty();
    if (on != m_shownOn || ids != m_shownIds) {
        m_shownOn = on;
        m_shownIds = std::move(ids);
        cui::requestRefresh();
    }
}

bool InventoryEffects::listOn(std::uintptr_t)
{
    const InventoryEffects& self = instance();
    return self.enabled() && !self.m_rows.empty();
}

bool InventoryEffects::rowOn(std::uintptr_t row)
{
    return row < instance().m_rows.size();
}

float InventoryEffects::rowY(std::uintptr_t row)
{
    const int count = static_cast<int>(instance().m_rows.size());
    if (row >= static_cast<std::uintptr_t>(count)) return 0.0f;
    return static_cast<float>(invfx::rowTop(static_cast<int>(row), count)) / static_cast<float>(invfx::kPanelHeight);
}

void InventoryEffects::rowName(std::uintptr_t row, std::string& out)
{
    const auto& rows = instance().m_rows;
    if (row < rows.size()) out = rows[row].name;
}

void InventoryEffects::rowTime(std::uintptr_t row, std::string& out)
{
    const auto& rows = instance().m_rows;
    if (row < rows.size()) out = rows[row].time;
}

void InventoryEffects::rowIcon(std::uintptr_t row, std::string& out)
{
    const auto& rows = instance().m_rows;
    if (row < rows.size()) out = rows[row].icon;
}

void InventoryEffects::rowBg(std::uintptr_t row, std::string& out)
{
    const auto& rows = instance().m_rows;
    out = (row < rows.size() && rows[row].ambient) ? kBackgroundAmbient : kBackground;
}

void InventoryEffects::rowTip(std::uintptr_t row, std::string& out)
{
    const auto& rows = instance().m_rows;
    if (row < rows.size()) out = rows[row].tip;
}

}
