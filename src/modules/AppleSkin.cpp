#include "modules/AppleSkin.h"

#include "config/Config.h"
#include "core/Logger.h"
#include "core/Notice.h"
#include "core/Paths.h"
#include "core/Strings.h"
#include "game/AppleSkinLayout.h"
#include "game/ContainerUi.h"
#include "game/GameData.h"
#include "game/GameString.h"
#include "game/PlayerListLayout.h"
#include "game/PngWrite.h"
#include "game/TooltipReserve.h"
#include "game/UiDrawContext.h"
#include "game/UiProbe.h"
#include "game/PlayerListMemory.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"
#include "memory/Signatures.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace tsukuyomi {
namespace {
constexpr std::uint32_t kAttributesComponent = 0xFD3B0613u;
constexpr std::size_t kAttributesSize = 0x50;
constexpr std::size_t kAttributeId = 4;
constexpr std::ptrdiff_t kStackItem = 0x08;
constexpr std::ptrdiff_t kMainhandStack = 0xE28;
constexpr std::size_t kIconStride = 12;
constexpr std::size_t kHungerJitterStore = 0xDF8;
constexpr std::size_t kHeartVectorLea = 0x4E9;

bool readBytes(const void* at, void* out, std::size_t size)
{
    return at != nullptr && memory::copyGuarded(at, out, size);
}

bool readPointer(const void* base, std::ptrdiff_t offset, const void*& out)
{
    out = nullptr;
    return base != nullptr && readBytes(static_cast<const char*>(base) + offset, &out, sizeof(out))
        && memory::plausiblePointer(out);
}

bool startsWith(const std::byte* at, std::initializer_list<unsigned char> bytes)
{
    unsigned char have[8]{};
    if (bytes.size() > sizeof(have) || !readBytes(at, have, bytes.size())) return false;
    std::size_t i = 0;
    for (unsigned char b : bytes) {
        if (have[i++] != b) return false;
    }
    return true;
}

std::int32_t readDisp(const std::byte* at)
{
    std::int32_t disp = 0;
    readBytes(at, &disp, sizeof(disp));
    return disp;
}

bool readAttribute(const void* actor, const void* attribute, float& current, float& maximum)
{
    std::uint32_t id = 0;
    std::uint8_t attributes[kAttributesSize]{};
    return actor != nullptr && attribute != nullptr
        && memory::copyGuarded(static_cast<const char*>(attribute) + kAttributeId, &id, sizeof(id))
        && GameData::copyComponent(actor, kAttributesComponent, sizeof(attributes), attributes)
        && playerlist::mem::readAttribute(attributes, id, current, maximum);
}

struct FoodValues : appleskin::Food {
    const void* component = nullptr;
    const void* vtable = nullptr;
};
using GetFoodFn = const void*(__fastcall*)(const void* item);
using IntGetter = int(__fastcall*)(const void* self);
using FloatGetter = float(__fastcall*)(const void* self);
using BoolGetter = bool(__fastcall*)(const void* self);

int queryFoodGuarded(const void* item, int getFoodSlot, FoodValues& out)
{
    __try {
        void* const* const itemVt = *static_cast<void* const* const*>(item);
        const void* const component = reinterpret_cast<GetFoodFn>(itemVt[getFoodSlot / 8])(item);
        if (component == nullptr) return 0;
        void* const* const vt = *static_cast<void* const* const*>(component);
        out.component = component;
        out.vtable = vt;
        out.nutrition = reinterpret_cast<IntGetter>(vt[1])(component);
        out.saturationModifier = reinterpret_cast<FloatGetter>(vt[2])(component);
        out.alwaysEat = reinterpret_cast<BoolGetter>(vt[3])(component);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
}

AppleSkin& AppleSkin::instance()
{
    static AppleSkin module;
    return module;
}

void AppleSkin::onScansReady()
{
    Scanner& scanner = Scanner::instance();
    if (std::byte* const isFood = scanner.address(Target::ItemIsFood)) {
        const std::int32_t slot = readDisp(isFood + 10);
        if (slot > 0 && slot < 0x1000 && slot % 8 == 0) m_getFoodSlot = slot;
    }
    if (std::byte* const site = scanner.address(Target::FoodAttributeSite)) {
        m_getEffect = memory::ripTarget(site, 9);
        m_hungerAttribute = memory::ripTarget(site, 23);
        if (startsWith(site + 93, {0x4C, 0x8D, 0x05})) m_saturationAttribute = memory::ripTarget(site, 96);
    }
    if (std::byte* const site = scanner.address(Target::ExhaustionAttributeSite)) {
        m_exhaustionAttribute = memory::ripTarget(site, 29);
    }
    if (std::byte* const fn = scanner.address(Target::HungerRendererUpdate)) {
        if (startsWith(fn + kHungerJitterStore, {0xC7, 0x86})) {
            const std::int32_t disp = readDisp(fn + kHungerJitterStore + 2);
            if (disp > 0x104 && disp < 0x4000) m_hungerIcons = disp - 4;
        }
    }
    if (std::byte* const fn = scanner.address(Target::HeartRendererUpdate)) {
        if (startsWith(fn + kHeartVectorLea, {0x48, 0x8D, 0x86})) {
            const std::int32_t disp = readDisp(fn + kHeartVectorLea + 3);
            if (disp > 0x100 && disp < 0x8000) m_heartIcons = disp;
        }
    }
    std::uint32_t ids[3] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    const void* attrs[3] = {m_hungerAttribute, m_saturationAttribute, m_exhaustionAttribute};
    for (int i = 0; i < 3; ++i) {
        if (attrs[i] != nullptr) memory::copyGuarded(static_cast<const char*>(attrs[i]) + kAttributeId, &ids[i], 4);
    }
    if (std::byte* const site = scanner.address(Target::HealthAttributeLoad)) {
        m_healthAttribute = memory::ripTarget(site, 8);
    }
    if (std::byte* const site = scanner.address(Target::DifficultySite)) {
        m_levelOffset = readDisp(site + 3);
        m_difficultySlot = readDisp(site + 13);
    }
    if (std::byte* const site = scanner.address(Target::GameRulesSite)) {
        const int offset = readDisp(site + 7);
        if (m_levelOffset < 0) m_levelOffset = offset;
        if (offset == m_levelOffset) m_rulesSlot = readDisp(site + 17);
    }
    auto slotOk = [](int slot) { return slot > 0 && slot < 0x1000 && slot % 8 == 0; };
    if (!slotOk(m_difficultySlot)) m_difficultySlot = -1;
    if (!slotOk(m_rulesSlot)) m_rulesSlot = -1;
    if (m_levelOffset <= 0 || m_levelOffset > 0x4000) m_levelOffset = -1;
    uidraw::resolve();
    prepareHud();
    log().info(L"AppleSkin: getFood slot +{:#x} / attribute ids hunger {} saturation {} exhaustion {} / getEffect {} health {} / "
               L"hunger icons +{:#x} / heart icons +{:#x} / difficulty {} rules {} / HUD {} tooltip {}",
               m_getFoodSlot, ids[0], ids[1], ids[2], m_getEffect != nullptr, m_healthAttribute != nullptr,
               m_hungerIcons, m_heartIcons, m_difficultySlot > 0, m_rulesSlot > 0, m_hudReady.load(), uidraw::api().textFieldsOk);
    m_ready.store(m_getFoodSlot > 0 && m_hungerAttribute != nullptr && m_saturationAttribute != nullptr
                      && m_exhaustionAttribute != nullptr,
                  std::memory_order_relaxed);
    m_on.store(enabled());
}

namespace {
namespace as = appleskin;
namespace cui = containerui;
struct Setting { const char* key; const wchar_t* label; };
constexpr Setting kSettings[] = {
    {"showFoodValuesInTooltip", L"Show food values in tooltip"},
    {"showFoodValuesInTooltipAlways", L"Always show food values in tooltip"},
    {"showSaturationHudOverlay", L"Show saturation overlay"},
    {"showFoodValuesHudOverlay", L"Show hunger restored from held food"},
    {"showFoodExhaustionHudUnderlay", L"Show exhaustion underlay"},
    {"showFoodHealthHudOverlay", L"Show estimated health overlay"},
    {"showVanillaAnimationsOverlay", L"Animate HUD icons to match Minecraft"},
};
constexpr int kRuleSize = 0x118, kNaturalRegenRule = 20;
constexpr int kRulesBegin = 0x18, kRulesEnd = 0x20, kRuleValue = 4, kRuleType = 8;
constexpr int kLegacyEffectsBegin = 0x80, kLegacyEffectsEnd = 0x88, kLegacyEffectSize = 0x58;

void writeFloat(int slot, float value)
{
    if (volatile float* const p = cui::persistentFloat(slot)) *p = value;
}
void writeOn(bool value)
{
    if (volatile std::uint8_t* const p = cui::persistentBool(as::kOnSlot)) *p = value ? 1 : 0;
}
void clearHud()
{
    writeOn(false);
    for (int i = as::kHungerSlot; i <= as::kExhaustionAlphaSlot; ++i) writeFloat(i, 0);
}
bool effectsGuarded(const FoodValues& values, as::Food& food)
{
    __try {
        auto* const vt = static_cast<void* const*>(values.vtable);
        const unsigned char* const fn = static_cast<const unsigned char*>(vt[3]);
        if (fn[0] != 0x0F || fn[1] != 0xB6 || fn[2] != 0x41 || fn[3] != 0x7C || fn[4] != 0xC3) return true;
        const auto* const bytes = static_cast<const std::byte*>(values.component);
        const auto begin = *reinterpret_cast<const std::byte* const*>(bytes + kLegacyEffectsBegin);
        const auto end = *reinterpret_cast<const std::byte* const*>(bytes + kLegacyEffectsEnd);
        const auto first = reinterpret_cast<std::uintptr_t>(begin), last = reinterpret_cast<std::uintptr_t>(end);
        if (last < first || (last - first) % kLegacyEffectSize != 0 || (last - first) / kLegacyEffectSize > 16) return false;
        food.effectCount = static_cast<int>((last - first) / kLegacyEffectSize);
        for (int i = 0; i < food.effectCount; ++i) {
            const std::byte* const effect = begin + i * kLegacyEffectSize;
            as::Effect& out = food.effects[i];
            out.id = *reinterpret_cast<const int*>(effect);
            out.duration = *reinterpret_cast<const int*>(effect + 0x48);
            out.amplifier = *reinterpret_cast<const int*>(effect + 0x4C);
            out.chance = *reinterpret_cast<const float*>(effect + 0x50);
            if (out.id < 1 || out.id > 40 || out.duration < 0 || out.amplifier < 0
                || !std::isfinite(out.chance) || out.chance < 0 || out.chance > 1) return false;
            food.rotten = food.rotten || as::harmfulEffect(out.id);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool rulesGuarded(const void* player, int levelOffset, int difficultySlot, int rulesSlot, bool& peaceful, bool& regen)
{
    __try {
        if (levelOffset < 0) return false;
        const void* const level = *reinterpret_cast<const void* const*>(static_cast<const char*>(player) + levelOffset);
        if (level == nullptr) return false;
        void* const* const vt = *static_cast<void* const* const*>(level);
        bool ok = true;
        if (difficultySlot > 0) peaceful = reinterpret_cast<IntGetter>(vt[difficultySlot / 8])(level) == 0;
        else ok = false;
        if (rulesSlot > 0) {
            const auto* const rules = static_cast<const std::byte*>(reinterpret_cast<GetFoodFn>(vt[rulesSlot / 8])(level));
            if (rules == nullptr) return false;
            const auto begin = *reinterpret_cast<const std::byte* const*>(rules + kRulesBegin);
            const auto end = *reinterpret_cast<const std::byte* const*>(rules + kRulesEnd);
            const auto first = reinterpret_cast<std::uintptr_t>(begin), last = reinterpret_cast<std::uintptr_t>(end);
            if (begin == nullptr || last < first || (last - first) % kRuleSize != 0
                || (last - first) / kRuleSize <= kNaturalRegenRule) return false;
            const auto* const rule = begin + kRuleSize * kNaturalRegenRule;
            if (rule[kRuleType] != std::byte{1}) return false;
            regen = rule[kRuleValue] != std::byte{0};
        } else ok = false;
        return ok;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool playerEffectsGuarded(void* actor, void* getEffect, bool& regen, bool& poison, bool& wither)
{
    __try {
        if (getEffect == nullptr) return false;
        using EffectFn = const void*(__fastcall*)(void*, int);
        const auto fn = reinterpret_cast<EffectFn>(getEffect);
        regen = fn(actor, 10) != nullptr;
        poison = fn(actor, 19) != nullptr;
        wither = fn(actor, 20) != nullptr;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool heartPositionsGuarded(void* self, std::ptrdiff_t offset, float* positions, int& count)
{
    __try {
        if (self == nullptr || offset < 0) return false;
        const auto range = reinterpret_cast<const float* const*>(static_cast<const char*>(self) + offset);
        const auto begin = reinterpret_cast<std::uintptr_t>(range[0]), end = reinterpret_cast<std::uintptr_t>(range[1]);
        if (end < begin || (end - begin) % kIconStride != 0 || (end - begin) / kIconStride > 4096) return false;
        count = static_cast<int>((end - begin) / kIconStride);
        if (count == 10) std::memcpy(positions, range[0], 10 * kIconStride);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}

void AppleSkin::prepareHud()
{
    clearHud();
    if (!cui::bindingsAvailable() || !cui::floatBindingsAvailable() || m_hungerIcons < 0 || m_heartIcons < 0
        || cui::persistentFloat(as::kExhaustionAlphaSlot) == nullptr || cui::persistentBool(as::kOnSlot) == nullptr) return;
    const auto directory = paths::dataDir() / L"appleskin";
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    bool failed = static_cast<bool>(ec);
    auto save = [&](const std::vector<std::uint8_t>& rgba, int width, int height) -> std::string {
        if (ec) return {};
        const auto file = directory / as::textureFileName(rgba);
        if (!std::filesystem::exists(file, ec) && !ec) {
            const auto bytes = png::encodeRgba(rgba.data(), width, height);
            std::ofstream stream(file, std::ios::binary);
            stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!stream || bytes.empty()) { failed = true; return {}; }
        }
        if (ec) { failed = true; return {}; }
        return toUtf8(file.generic_wstring());
    };
    as::Textures textures;
    for (int i = 0; i < 4; ++i) textures.saturation[i] = save(as::saturationPixels(i + 1), 9, 9);
    textures.exhaustion = save(as::exhaustionPixels(), as::kExhaustionWidth, 9);
    if (failed) log().warn(L"AppleSkin: some HUD textures could not be written; those groups are hidden");
    const as::Layout layout = as::layoutJson(textures);
    uiprobe::registerDefExtension("hud", "centered_gui_elements_at_bottom_middle", layout.front, layout.back);
    cui::addHudObserver(&AppleSkin::onHudCreated);
    m_hudReady.store(true);
}

bool AppleSkin::readFood(const void* stack, as::Food& food)
{
    const void* ref = nullptr;
    const void* item = nullptr;
    if (m_getFoodSlot < 0 || !readPointer(stack, kStackItem, ref) || !readPointer(ref, 0, item)) return false;
    FoodValues values;
    const int kind = queryFoodGuarded(item, m_getFoodSlot, values);
    if (kind < 0 && !m_loggedFoodFault.exchange(true)) log().warn(L"AppleSkin: getFood faulted; unreadable food is skipped");
    if (kind != 1 || values.nutrition < -128 || values.nutrition > 127 || !std::isfinite(values.saturationModifier)
        || !std::isfinite(as::saturationGain(values)) || std::fabs(as::saturationGain(values)) > 3276.7f) return false;
    food = values;
    if (!effectsGuarded(values, food)) {
        food.effectCount = 0;
        food.rotten = false;
        if (!m_loggedEffects.exchange(true)) log().warn(L"AppleSkin: legacy food effects could not be validated; effects are omitted");
    }
    return true;
}

void AppleSkin::onPlayerViewUpdate()
{
    auto invalidate = [this] { m_statsValid.store(false); m_hasFood.store(false); };
    if (!available() || m_stopping.load()) { invalidate(); return; }
    void* const player = GameData::instance().player();
    as::Input in;
    float maximum = 0;
    if (player == nullptr || !readAttribute(player, m_hungerAttribute, in.hunger, maximum)
        || !readAttribute(player, m_saturationAttribute, in.saturation, maximum)
        || !readAttribute(player, m_exhaustionAttribute, in.exhaustion, maximum)
        || !std::isfinite(in.hunger) || !std::isfinite(in.saturation) || !std::isfinite(in.exhaustion)) { invalidate(); return; }
    m_foodHunger.store(static_cast<int>(std::clamp(in.hunger, 0.0f, 20.0f)));
    m_foodSaturation.store(in.saturation);
    m_foodExhaustion.store(in.exhaustion);
    if (!readAttribute(player, m_healthAttribute, in.health, in.maxHealth)) in.health = in.maxHealth = 0;
    in.hasFood = readFood(static_cast<const char*>(player) + kMainhandStack, in.food);
    const bool rulesRead =
        rulesGuarded(player, m_levelOffset, m_difficultySlot, m_rulesSlot, in.peaceful, in.naturalRegen);
    if (!rulesRead) {
        in.peaceful = false;
        notice::failOnce("AppleSkin.rules",
                         L"AppleSkin: difficulty or natural regeneration could not be read; the health preview is off",
                         "AppleSkin health preview is off: the difficulty or natural regeneration could not be read");
    }
    if (!playerEffectsGuarded(player, m_getEffect, in.regeneration, in.poison, in.wither) || !rulesRead) in.regeneration = true;
    in.heartCount = m_heartCount.load();
    const as::States states = as::calculate(in);
    for (int i = 0; i < as::kIcons; ++i) {
        m_hunger[i].store(states.hunger[i]); m_saturation[i].store(states.saturation[i]);
        m_gain[i].store(states.gain[i]); m_health[i].store(states.health[i]);
    }
    m_exhaustionRatio.store(states.exhaustionRatio);
    m_hasFood.store(in.hasFood);
    m_statsValid.store(true);
}

bool AppleSkin::hudActive() const
{
    return m_on.load() && available() && m_hudReady.load() && !m_stopping.load()
        && m_statsValid.load() && GameData::instance().msSinceView() < 500;
}
void AppleSkin::onHungerRendererUpdate(void* self)
{
    if (!hudActive()) { clearHud(); return; }
    float positions[30]{};
    if (self != nullptr) readBytes(static_cast<const char*>(self) + m_hungerIcons, positions, sizeof(positions));
    const bool animate = m_settings[6].load();
    for (int i = 0; i < as::kIcons; ++i) {
        const int shake = animate && positions[i * 3 + 1] < -0.5f ? 8 : 0;
        auto code = [shake](std::uint8_t state) { return static_cast<float>(state == 0 ? 0 : state + shake); };
        writeFloat(as::kHungerSlot + i, m_settings[3].load() ? code(m_hunger[i].load()) : 0);
        writeFloat(as::kSaturationSlot + i, m_settings[2].load() ? code(m_saturation[i].load()) : 0);
        writeFloat(as::kGainSlot + i, m_settings[2].load() && m_settings[3].load() ? code(m_gain[i].load()) : 0);
    }
    const auto now = GetTickCount64();
    float alpha = 0;
    if (!m_hasFood.load()) { m_flash = {}; m_flashTick = now; }
    else {
        if (m_flashTick == 0) m_flashTick = now;
        const auto ticks = (now - m_flashTick) / 50;
        m_flashTick += ticks * 50;
        alpha = as::advanceFlash(m_flash, ticks, m_maxAlpha.load());
    }
    writeFloat(as::kFlashSlot, alpha);
    const float ratio = as::exhaustionShown(m_exhaustionRatio.load());
    writeFloat(as::kExhaustionSlot, 1 - ratio);
    writeFloat(as::kExhaustionAlphaSlot, m_settings[4].load() ? 0.75f : 0);
    writeOn(hudActive());
}
void AppleSkin::onHeartRendererUpdate(void* self)
{
    float positions[30]{};
    int count = 0;
    if (!heartPositionsGuarded(self, m_heartIcons, positions, count)) count = 0;
    m_heartCount.store(count);
    if (!hudActive()) { clearHud(); return; }
    const bool active = hudActive() && count == 10 && m_settings[5].load();
    for (int i = 0; i < as::kIcons; ++i) {
        const int state = active ? m_health[i].load() : 0;
        const int shake = m_settings[6].load() && positions[i * 3 + 1] < -0.5f ? 8 : 0;
        writeFloat(as::kHealthSlot + i, static_cast<float>(state == 0 ? 0 : state + shake));
    }
}
bool AppleSkin::foodStats(int& hunger, float& saturation, float& exhaustion) const
{
    if (!m_statsValid.load() || m_stopping.load() || GameData::instance().msSinceView() >= 500) return false;
    hunger = m_foodHunger.load(); saturation = m_foodSaturation.load(); exhaustion = m_foodExhaustion.load();
    return true;
}
void AppleSkin::onHudCreated(void* ctrl)
{
    int floats = 0;
    for (int i = 0; i < 10; ++i) {
        const std::string suffix = std::to_string(i);
        floats += cui::bindPersistentFloat(ctrl, ("#tk_as_h" + suffix).c_str(), as::kHungerSlot + i) ? 1 : 0;
        floats += cui::bindPersistentFloat(ctrl, ("#tk_as_s" + suffix).c_str(), as::kSaturationSlot + i) ? 1 : 0;
        floats += cui::bindPersistentFloat(ctrl, ("#tk_as_g" + suffix).c_str(), as::kGainSlot + i) ? 1 : 0;
        floats += cui::bindPersistentFloat(ctrl, ("#tk_as_p" + suffix).c_str(), as::kHealthSlot + i) ? 1 : 0;
    }
    floats += cui::bindPersistentFloat(ctrl, "#tk_as_flash", as::kFlashSlot) ? 1 : 0;
    floats += cui::bindPersistentFloat(ctrl, "#tk_as_exh", as::kExhaustionSlot) ? 1 : 0;
    floats += cui::bindPersistentFloat(ctrl, "#tk_as_exh_alpha", as::kExhaustionAlphaSlot) ? 1 : 0;
    const bool bound = cui::bindPersistentBool(ctrl, "#tk_as_on", as::kOnSlot);
    static bool told = false;
    if (!told) { told = true; log().info(L"AppleSkin: HUD bindings float {}/43 bool {}/1", floats, bound ? 1 : 0); }
}
void AppleSkin::onEnabledChanged(bool value)
{
    m_on.store(value && !isWriteBlocked());
}
void AppleSkin::shutdown()
{
    m_stopping.store(true); m_on.store(false);
    clearHud();
}
MenuItem AppleSkin::buildMenu()
{
    std::vector<MenuItem> children{enabledItem(), toggleKeyItem()};
    for (std::size_t i = 0; i < m_settings.size(); ++i) {
        children.push_back(menu::toggle(kSettings[i].label, [this, i] { return m_settings[i].load(); },
            [this, i] { m_settings[i].store(!m_settings[i].load()); }));
    }
    children.push_back(menu::number(L"Max alpha of flashing HUD icons", [this] { return m_maxAlpha.load(); },
        [this](float value) { m_maxAlpha.store(std::clamp(value, 0.0f, 1.0f)); }, false, 0, 1));
    auto item = menu::submenu(name(), std::move(children));
    item.available = [this] { return available(); };
    item.isOn = [this] { return enabled(); };
    return item;
}
void AppleSkin::loadConfig(const nlohmann::json& section)
{
    Module::loadConfig(section);
    for (std::size_t i = 0; i < m_settings.size(); ++i) m_settings[i].store(Config::getBool(section, kSettings[i].key, true));
    const float alpha = Config::getFloat(section, "maxHudOverlayFlashAlpha", 0.65f);
    m_maxAlpha.store(std::isfinite(alpha) ? std::clamp(alpha, 0.0f, 1.0f) : 0.65f);
    m_on.store(enabled());
}
void AppleSkin::saveConfig(nlohmann::json& section) const
{
    Module::saveConfig(section);
    for (std::size_t i = 0; i < m_settings.size(); ++i) section[kSettings[i].key] = m_settings[i].load();
    section["maxHudOverlayFlashAlpha"] = m_maxAlpha.load();
}

namespace {
constexpr std::string_view kTooltipMarker = "\xC2\xA7r\xC2\xA7o\xC2\xA7r";
constexpr float kTooltipHeight = 17;
struct TooltipRow {
    int count = 0;
    bool multiplier = false;
    std::string text;
    float width = 0;
};
TooltipRow tooltipRow(float amount, bool saturation)
{
    TooltipRow row;
    row.count = static_cast<int>(std::ceil(std::fabs(amount) / 2));
    row.multiplier = row.count > 10 || (saturation && row.count == 0);
    if (row.multiplier) {
        row.text = "x" + std::to_string(row.count);
        row.count = 1;
    }
    const int step = saturation ? 7 : 9;
    row.width = static_cast<float>(row.count * step)
        + (row.multiplier ? static_cast<float>(playerlist::estimateWidth(row.text)) * 0.75f + 4 : 0);
    return row;
}
bool drawRuns(void* ctx, const std::vector<as::PixelRun>& runs, float x, float y, float alpha)
{
    for (const auto& run : runs) {
        const float color[4] = {static_cast<float>((run.rgb >> 16) & 255) / 255,
            static_cast<float>((run.rgb >> 8) & 255) / 255, static_cast<float>(run.rgb & 255) / 255, 1};
        const float rect[4] = {x + static_cast<float>(run.x), x + static_cast<float>(run.x + run.width),
            y + static_cast<float>(run.y), y + static_cast<float>(run.y + 1)};
        if (!uidraw::fillGuarded(ctx, rect, color, alpha)) return false;
    }
    return true;
}
bool drawMultiplier(void* ctx, void* font, const TooltipRow& row, float x, float y, float alpha)
{
    if (!row.multiplier) return true;
    struct ShortString { char text[16]{}; std::uint64_t size = 0, cap = 15; } text;
    static_assert(sizeof(ShortString) == 32);
    std::memcpy(text.text, row.text.data(), row.text.size());
    text.size = row.text.size();
    struct Measure { float size = 0.75f, padding = 0; bool shadow = true, symbols = false, hyphen = false; } measure;
    struct Caret { int position = -1; bool render = false; } caret;
    constexpr float gray[4] = {170.0f / 255, 170.0f / 255, 170.0f / 255, 1};
    const float rect[4] = {x, x + row.width + 9, y, y + 9};
    return uidraw::drawTextGuarded(ctx, font, rect, &text, gray, alpha, 0, &measure, &caret);
}
bool drawHungerImages(void* ctx, const std::vector<as::TooltipIcon>& icons, bool rotten, float x, float y, float alpha)
{
    static const char* const kNormal[3] = {"textures/ui/hunger_background", "textures/ui/hunger_full", "textures/ui/hunger_half"};
    static const char* const kRotten[3] = {"textures/ui/hunger_background", "textures/ui/hunger_effect_full",
                                           "textures/ui/hunger_effect_half"};
    const char* const* const paths = rotten ? kRotten : kNormal;
    bool ok = true;
    for (int layer = 0; layer < 3 && ok; ++layer) {
        const int want = layer == 1 ? as::kHungerFull : as::kHungerHalf;
        int count = 0;
        for (const auto& icon : icons) count += layer == 0 || icon.image == want ? 1 : 0;
        if (count == 0) continue;
        uidraw::ResourceLocation location;
        if (!uidraw::makeLocation(paths[layer], location)) return false;
        uidraw::TexturePtr texture;
        if (!uidraw::getTextureGuarded(ctx, location, texture)) return false;

        for (const auto& icon : icons) {
            if (!ok) break;
            if (layer != 0 && icon.image != want) continue;
            ok = uidraw::drawImageGuarded(ctx, texture, x + static_cast<float>(icon.x), y, 9, 9);
        }
        if (!uidraw::flushImagesGuarded(ctx, alpha)) ok = false;
        if (!uidraw::releaseTextureGuarded(texture)) ok = false;
    }
    return ok;
}
}

void AppleSkin::onItemHoverText(const void* stack, void* out)
{
    if (!m_on.load() || !available() || m_stopping.load() || m_tooltipBroken.load() || !uidraw::api().textFieldsOk
        || !gamestring::available() || !(m_settings[1].load() || (m_settings[0].load() && (GetAsyncKeyState(VK_SHIFT) & 0x8000)))) return;
    as::Food food;
    if (!readFood(stack, food)) return;
    std::string text;
    if (!gamestring::read(out, text)) return;
    int line = 0, lines = 0, reserved = 0, spaces = 0;
    std::uint32_t previous = 0;
    if (tooltipreserve::findMarker(text, kTooltipMarker, line, lines, reserved, spaces, previous)) return;
    text += '\n';
    text += tooltipreserve::reserveText(kTooltipMarker, std::max(1, m_reserveRows.load() - 1), m_reserveSpaces.load(),
        as::packFood(food.nutrition, as::saturationGain(food), food.rotten));
    if (!gamestring::assign(out, text)) return;
    if (!m_loggedReserve) { m_loggedReserve = true; log().info(L"AppleSkin: food values reserved in the vanilla tooltip"); }
}

void AppleSkin::onHoverRender(void* self, void* ctx, void* client, void* owner)
{
    (void)owner;
    const auto& api = uidraw::api();
    if (!m_on.load() || m_stopping.load() || m_tooltipBroken.load() || self == nullptr || ctx == nullptr
        || client == nullptr || !api.textFieldsOk || api.boxW < 0 || api.boxH < 0) return;
    std::string text;
    if (!gamestring::read(static_cast<char*>(self) + 0x30, text) || text.empty()) { m_shiftKnown = false; return; }
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (m_shiftKnown && shift != m_previousShift && m_settings[0].load() && !m_settings[1].load()) cui::requestRefresh();
    m_previousShift = shift; m_shiftKnown = true;
    if (!(m_settings[1].load() || (m_settings[0].load() && shift))) return;
    int line = 0, lines = 0, reserved = 0, spaces = 0, nutrition = 0;
    float gain = 0;
    bool rotten = false;
    std::uint32_t id = 0;
    if (!tooltipreserve::findMarker(text, kTooltipMarker, line, lines, reserved, spaces, id)
        || !as::unpackFood(id, nutrition, gain, rotten)) return;
    float position[4]{}, alpha = 0, bw = 0, bh = 0;
    const auto* const box = static_cast<const char*>(self);
    if (!readBytes(box + 0x50, position, sizeof(position)) || !readBytes(box + 0x08, &alpha, 4)
        || !readBytes(box + api.boxW, &bw, 4) || !readBytes(box + api.boxH, &bh, 4)) return;
    const float lineH = (bh - 8) / static_cast<float>(lines);
    const float x = std::floor(position[0] + position[2] + 5);
    const float y = std::floor(position[1] + position[3] + 5 + static_cast<float>(line) * lineH);
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(alpha) || !std::isfinite(bw)
        || !(lineH >= 4 && lineH <= 64) || bw <= 9 || alpha <= 0) return;
    const TooltipRow hungerRow = tooltipRow(static_cast<float>(nutrition), false), satRow = tooltipRow(gain, true);
    const float width = std::max(hungerRow.width, satRow.width);
    const int wantRows = tooltipreserve::rowsFor(lineH, kTooltipHeight + 1);
    const int wantSpaces = std::min(tooltipreserve::spacesFor(spaces, bw - 9, width + 2), 400);
    m_reserveRows.store(wantRows); m_reserveSpaces.store(wantSpaces);
    if (bw - 9 < width || static_cast<float>(reserved) * lineH < kTooltipHeight) {
        if (wantRows != reserved || wantSpaces != spaces) cui::requestRefresh();
        return;
    }
    if (!uidraw::checkContext(ctx)) return;
    if (uidraw::context().fill == nullptr || !api.tintReady) return;
    if (!uidraw::imagesReady()) {
        m_tooltipBroken.store(true);
        notice::failOnce("AppleSkin.tooltipImages",
            L"AppleSkin: the UI render context has no getTexture/drawImage/flushImages; food tooltips are turned off",
            "AppleSkin: food tooltips are off (UI image functions not found)");
        return;
    }
    bool queueEmpty = false;
    if (!uidraw::imageQueueEmptyGuarded(ctx, queueEmpty)) return;
    if (!queueEmpty) {
        if (!m_loggedBusyQueue) { m_loggedBusyQueue = true; log().warn(L"AppleSkin: the UI image queue was not empty; tooltip icons skipped once"); }
        return;
    }
    float saved[4]{};
    if (!uidraw::readShaderColorGuarded(ctx, saved)) return;
    struct RestoreColor {
        void* ctx; float* saved; std::atomic<bool>& broken;
        ~RestoreColor()
        {
            if (!uidraw::writeShaderColorGuarded(ctx, saved) && !broken.exchange(true))
                log().error(L"AppleSkin: restoring the tooltip color faulted; food tooltips are turned off");
        }
    } restore{ctx, saved, m_tooltipBroken};
    bool ok = drawHungerImages(ctx, as::tooltipHungerIcons(nutrition, hungerRow.count), rotten, x, y, alpha);
    for (const auto& icon : as::tooltipSaturationIcons(gain, satRow.count)) {
        if (!ok) break;
        ok = drawRuns(ctx, as::tooltipSaturationRuns(icon.image, gain < 0), x + static_cast<float>(icon.x), y + 10,
            icon.faded ? alpha * 0.5f : alpha);
    }
    if (ok && (hungerRow.multiplier || satRow.multiplier)) {
        if (uidraw::context().drawText == nullptr || uidraw::context().flushText == nullptr
            || api.clientFontSlot < 0 || api.fontOfHandle == nullptr || api.fontHandleDtor == nullptr) return;
        void* begin = nullptr; void* end = nullptr;
        const bool empty = readBytes(static_cast<char*>(ctx) + 0x80, &begin, 8)
            && readBytes(static_cast<char*>(ctx) + 0x88, &end, 8) && begin == end;
        alignas(16) std::byte handle[0x80]{};
        void* const font = uidraw::fontGuarded(client, handle);
        if (font == nullptr) ok = false;
        else {
            ok = drawMultiplier(ctx, font, hungerRow, x + 9 + 1.5f, y + 1.5f, alpha)
                && drawMultiplier(ctx, font, satRow, x + 7 + 1.5f, y + 10 + 0.75f, alpha);
            if (empty && !uidraw::flushTextGuarded(ctx)) ok = false;
            if (!uidraw::fontHandleDtorGuarded(handle)) ok = false;
        }
    }
    if (!ok) {
        m_tooltipBroken.store(true);
        log().error(L"AppleSkin: tooltip drawing faulted; food tooltips are turned off");
        return;
    }
    if (!m_loggedDraw) { m_loggedDraw = true; log().info(L"AppleSkin: food values drawn inside the vanilla tooltip"); }
}

}
