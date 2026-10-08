#include "modules/ItemScroller.h"

#include "config/Config.h"
#include "core/Logger.h"
#include "core/Paths.h"
#include "core/Strings.h"
#include "game/ItemStackOps.h"
#include "game/SettingsCommand.h"
#include "hooks/Detours.h"
#include "input/Foreground.h"
#include "input/GameButtons.h"
#include "input/GameInput.h"
#include "input/Keys.h"
#include "modules/ShulkerPreview.h"

#include <Windows.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace tsukuyomi {

namespace {

namespace cui = containerui;

constexpr int kShift = VK_SHIFT;
constexpr int kCtrl = VK_CONTROL;
constexpr int kAlt = VK_MENU;
constexpr int kLmb = VK_LBUTTON;
constexpr int kRmb = VK_RBUTTON;
constexpr int kMmb = VK_MBUTTON;

std::vector<std::string> splitList(const std::wstring& text)
{
    std::vector<std::string> out;
    std::string cur;
    for (wchar_t c : text) {
        if (c == L',' || c == L';' || c == L'\n') {
            while (!cur.empty() && cur.back() == ' ') {
                cur.pop_back();
            }
            if (!cur.empty()) {
                out.push_back(cur);
            }
            cur.clear();
            continue;
        }
        if (cur.empty() && c == L' ') {
            continue;
        }
        const std::string u = toUtf8(std::wstring(1, c));
        cur += u;
    }
    while (!cur.empty() && cur.back() == ' ') {
        cur.pop_back();
    }
    if (!cur.empty()) {
        out.push_back(cur);
    }
    return out;
}

std::string normalizeItemName(std::string name)
{
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (!name.empty() && name.find(':') == std::string::npos) {
        name = "minecraft:" + name;
    }
    return name;
}

class StackCopy {
public:
    StackCopy() = default;
    explicit StackCopy(const void* src) { copyFrom(src); }
    ~StackCopy() { reset(); }
    StackCopy(const StackCopy&) = delete;
    StackCopy& operator=(const StackCopy&) = delete;

    bool copyFrom(const void* src)
    {
        reset();
        if (src == nullptr || !ItemStackOps::instance().available()) {
            return false;
        }
        m_has = ItemStackOps::instance().cloneTo(m_buf, src);
        return m_has;
    }
    void reset()
    {
        if (m_has) {
            ItemStackOps::instance().destroyClone(m_buf);
            m_has = false;
        }
    }
    const void* get() const { return m_has ? static_cast<const void*>(m_buf) : nullptr; }

private:
    alignas(16) std::byte m_buf[ItemStackOps::kStackBytes]{};
    bool m_has = false;
};

std::wstring readSortList(const nlohmann::json& section, const char* key, const std::wstring& current)
{
    const auto it = section.find(key);
    if (it == section.end()) {
        return current;
    }
    std::wstring raw;
    if (it->is_string()) {
        raw = toUtf16(it->get<std::string>());
    } else if (it->is_array()) {
        for (const auto& v : *it) {
            if (v.is_string()) {
                raw += toUtf16(v.get<std::string>());
                raw += L',';
            }
        }
    } else {
        return current;
    }
    std::wstring out;
    for (const std::string& s : splitList(raw)) {
        if (!out.empty()) {
            out += L',';
        }
        out += toUtf16(s);
    }
    return out;
}

nlohmann::json sortListJson(const std::wstring& text)
{
    nlohmann::json out = nlohmann::json::array();
    for (const std::string& s : splitList(text)) {
        out.push_back(s);
    }
    return out;
}

bool nameContains(const std::string& name, const char* part)
{
    return name.find(part) != std::string::npos;
}

bool isShulkerName(const std::string& name)
{
    return nameContains(name, "shulker_box");
}

bool isBundleName(const std::string& name)
{
    return nameContains(name, "bundle");
}

}

struct ItemScroller::StackRef {
    explicit StackRef(const void* src) : copy(src) {}
    const void* get() const { return copy.get(); }
    StackCopy copy;
};

ItemScroller& ItemScroller::instance()
{
    static ItemScroller module;
    return module;
}

bool ItemScroller::available() const
{
    return cui::available();
}

std::array<ItemScroller::KeySetting, ItemScroller::kKeyCount> ItemScroller::makeKeys()
{
    std::array<KeySetting, kKeyCount> k{};
    auto set = [&k](KeyId id, const wchar_t* name, std::vector<int> def) {
        k[id].name = name;
        k[id].keys = def;
        k[id].defaults = std::move(def);
    };
    set(kCraftEverything, L"craftEverything", {kCtrl, 'C'});
    set(kDropAllMatching, L"dropAllMatching", {kCtrl, kShift, 'Q'});
    set(kMassCraft, L"massCraft", {kCtrl, kAlt, 'C'});
    set(kMoveCraftResults, L"moveCraftResults", {kCtrl, 'M'});
    set(kRecipeView, L"recipeView", {'A'});
    set(kStoreRecipe, L"storeRecipe", {kMmb});
    set(kThrowCraftResults, L"throwCraftResults", {kCtrl, 'T'});
    set(kVillagerTradeFavorites, L"villagerTradeFavorites", {});
    set(kModifierMoveEverything, L"modifierMoveEverything", {kAlt, kShift});
    set(kModifierMoveMatching, L"modifierMoveMatching", {kAlt});
    set(kModifierMoveStack, L"modifierMoveStack", {kShift});
    set(kModifierToggleVillagerGlobalFavorite, L"modifierToggleVillagerGlobalFavorite", {kShift});
    set(kKeyDragMoveStacks, L"keyDragMoveStacks", {kShift, kLmb});
    set(kKeyDragMoveLeaveOne, L"keyDragMoveLeaveOne", {kShift, kRmb});
    set(kKeyDragMoveMatching, L"keyDragMoveMatching", {kAlt, kLmb});
    set(kKeyDragMoveOne, L"keyDragMoveOne", {kCtrl, kLmb});
    set(kKeyDragDropLeaveOne, L"keyDragDropLeaveOne", {kShift, 'Q', kRmb});
    set(kKeyDragDropSingle, L"keyDragDropSingle", {'Q', kLmb});
    set(kKeyDragDropStacks, L"keyDragDropStacks", {kShift, 'Q', kLmb});
    set(kKeyMoveEverything, L"keyMoveEverything", {kAlt, kShift, kLmb});
    set(kWsMoveDownLeaveOne, L"wsMoveDownLeaveOne", {'S', kRmb});
    set(kWsMoveDownMatching, L"wsMoveDownMatching", {kAlt, 'S', kLmb});
    set(kWsMoveDownSingle, L"wsMoveDownSingle", {'S', kLmb});
    set(kWsMoveDownStacks, L"wsMoveDownStacks", {kShift, 'S', kLmb});
    set(kWsMoveUpLeaveOne, L"wsMoveUpLeaveOne", {'W', kRmb});
    set(kWsMoveUpMatching, L"wsMoveUpMatching", {kAlt, 'W', kLmb});
    set(kWsMoveUpSingle, L"wsMoveUpSingle", {'W', kLmb});
    set(kWsMoveUpStacks, L"wsMoveUpStacks", {kShift, 'W', kLmb});
    set(kSortInventory, L"sortInventory", {kCtrl, 'R'});
    static_assert(kKeyCount == kHotkeyCount);
    return k;
}

MenuItem ItemScroller::buildMenu()
{
    std::vector<MenuItem> c;
    c.push_back(enabledItem());

    auto tgl = [&c](const wchar_t* title, bool* value) {
        c.push_back(menu::toggle(title, [value] { return *value; }, [value] { *value = !*value; }));
    };

    tgl(L"villagerTradeUnlockAllTiers", &m_villagerTradeUnlockAllTiers);
    tgl(L"villagerTradeFavoritesOnOpen", &m_villagerTradeFavoritesOnOpen);
    tgl(L"villagerTradeOnOpenThrowResults", &m_villagerTradeOnOpenThrowResults);
    c.push_back(toggleKeyItem());

    MenuItem item = menu::submenu(name(), std::move(c));
    item.available = [this] { return available(); };
    item.isOn = [this] { return enabled(); };
    return item;
}

void ItemScroller::loadConfig(const nlohmann::json& section)
{
    Module::loadConfig(section);
    auto b = [&section](const char* key, bool& v) { v = Config::getBool(section, key, v); };
    b("villagerTradeUnlockAllTiers", m_villagerTradeUnlockAllTiers);
    b("villagerTradeFavoritesOnOpen", m_villagerTradeFavoritesOnOpen);
    b("villagerTradeOnOpenThrowResults", m_villagerTradeOnOpenThrowResults);
    {
        std::array<std::vector<int>, kHotkeyCount> combos;
        std::array<std::string, kHotkeyCount> text;
        readHotkeys(section, combos, text);
        for (size_t i = 0; i < m_keys.size(); ++i) {
            m_keys[i].keys = combos[i];
        }
        watchKeys();
        for (KeySetting& setting : m_keys) {
            int trigger = 0;
            for (int vk : setting.keys) if (!keys::isModifier(vk)) trigger = vk;
            setting.seenSeq = trigger ? keySeq(trigger) : 0;
            setting.wasDown = true;
        }
        std::lock_guard<std::mutex> lock(m_keysMutex);
        m_keyText = text;
        m_sortTopPriority = readSortList(section, "sortTopPriorityInventory", m_sortTopPriority);
        m_sortCategoryOrder = readSortList(section, "sortCategoryOrder", m_sortCategoryOrder);
    }
    rebuildLists();
}

bool ItemScroller::readHotkeys(const nlohmann::json& section, std::array<std::vector<int>, kHotkeyCount>& out,
                               std::array<std::string, kHotkeyCount>& text) const
{
    bool changed = false;
    const auto hk = section.find("hotkeys");
    for (size_t i = 0; i < m_keys.size(); ++i) {
        std::vector<int> combo = m_keys[i].defaults;
        if (hk != section.end() && hk->is_object()) {
            if (const auto k = hk->find(toUtf8(m_keys[i].name)); k != hk->end()) {
                if (k->is_string()) {
                    std::vector<int> parsed;
                    if (keys::parseCombo(toUtf16(k->get<std::string>()), parsed)) {
                        combo = std::move(parsed);
                    } else {
                        log().warn(L"ItemScroller: hotkeys.{}: cannot read \"{}\" (using {})", m_keys[i].name,
                                   toUtf16(k->get<std::string>()), keys::comboName(combo));
                    }
                }
            }
        }
        changed = changed || combo != out[i];
        out[i] = std::move(combo);
        text[i] = toUtf8(keys::comboName(out[i]));
    }
    return changed;
}

void ItemScroller::watchConfigFile()
{
    const unsigned long long now = GetTickCount64();
    if (now - m_cfgCheckMs < 1000) {
        return;
    }
    m_cfgCheckMs = now;
    const std::filesystem::path path = paths::configFile();
    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return;
    }
    const long long ticks = static_cast<long long>(stamp.time_since_epoch().count());
    if (ticks == m_cfgStamp) {
        return;
    }
    nlohmann::json root;
    try {
        std::ifstream in(path, std::ios::binary);
        in >> root;
    } catch (...) {
        if (!m_cfgWarned) {
            m_cfgWarned = true;
            log().warn(L"ItemScroller: the config file could not be read; hotkeys were not reloaded");
        }
        return;
    }
    m_cfgWarned = false;
    const bool first = (m_cfgStamp == 0);
    m_cfgStamp = ticks;
    if (first) {
        return;
    }
    const auto sec = root.find("ItemScroller");
    if (sec == root.end() || !sec->is_object()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_keysMutex);
        const std::wstring top = readSortList(*sec, "sortTopPriorityInventory", m_sortTopPriority);
        const std::wstring cat = readSortList(*sec, "sortCategoryOrder", m_sortCategoryOrder);
        if (top != m_sortTopPriority || cat != m_sortCategoryOrder) {
            m_sortTopPriority = top;
            m_sortCategoryOrder = cat;
            m_listsPending.store(true, std::memory_order_release);
            log().info(L"ItemScroller: reloaded the sort lists from the config file");
        }
    }
    std::array<std::vector<int>, kHotkeyCount> combos;
    std::array<std::string, kHotkeyCount> text;
    {
        std::lock_guard<std::mutex> lock(m_keysMutex);
        for (size_t i = 0; i < combos.size(); ++i) {
            std::vector<int> parsed;
            if (keys::parseCombo(toUtf16(m_keyText[i]), parsed)) {
                combos[i] = std::move(parsed);
            }
        }
    }
    if (!readHotkeys(*sec, combos, text)) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_keysMutex);
    m_pendingKeys = combos;
    m_keyText = text;
    m_keysPending.store(true, std::memory_order_release);
    log().info(L"ItemScroller: reloaded the hotkeys from the config file");
}

void ItemScroller::applyPendingKeys()
{
    if (m_listsPending.exchange(false, std::memory_order_acq_rel)) {
        rebuildLists();
    }
    if (!m_keysPending.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_keysMutex);
    for (size_t i = 0; i < m_keys.size(); ++i) {
        if (m_keys[i].keys != m_pendingKeys[i]) {
            m_keys[i].keys = m_pendingKeys[i];
            watchKeys();
            int trigger = 0;
            for (int vk : m_keys[i].keys) if (!keys::isModifier(vk)) trigger = vk;
            m_keys[i].seenSeq = trigger ? keySeq(trigger) : 0;
            m_keys[i].wasDown = true;
            log().info(L"ItemScroller: {} = {}", m_keys[i].name, keys::comboName(m_keys[i].keys));
        }
    }
}

bool ItemScroller::autoTradeBypassed() const
{
    if (input::sneakHeldWithin(input::kSneakHoldGraceMs)) {
        return true;
    }
    const GameButtons& buttons = GameButtons::instance();
    const std::uint64_t at = buttons.buttonLastPressMs(m_useButton);
    return at != 0 && GetTickCount64() - at < 3000
        && gamebuttonlogic::sneakHeldAt(at, buttons.buttonLastPressMs(m_sneakButton),
                                        buttons.buttonLastReleaseMs(m_sneakButton), input::kSneakHoldGraceMs);
}

void ItemScroller::saveConfig(nlohmann::json& section) const
{
    Module::saveConfig(section);
    section["villagerTradeUnlockAllTiers"] = m_villagerTradeUnlockAllTiers;
    section["villagerTradeFavoritesOnOpen"] = m_villagerTradeFavoritesOnOpen;
    section["villagerTradeOnOpenThrowResults"] = m_villagerTradeOnOpenThrowResults;
    nlohmann::json hk = nlohmann::json::object();
    {
        std::lock_guard<std::mutex> lock(m_keysMutex);
        for (size_t i = 0; i < m_keys.size(); ++i) {
            hk[toUtf8(m_keys[i].name)] = m_keyText[i];
        }
        section["sortTopPriorityInventory"] = sortListJson(m_sortTopPriority);
        section["sortCategoryOrder"] = sortListJson(m_sortCategoryOrder);
    }
    section["hotkeys"] = std::move(hk);
    saveRecipes();
}

void ItemScroller::rebuildLists()
{
    std::wstring topText;
    std::wstring catText;
    {
        std::lock_guard<std::mutex> lock(m_keysMutex);
        topText = m_sortTopPriority;
        catText = m_sortCategoryOrder;
    }
    m_topPriority.clear();
    m_categoryOrder.clear();
    for (const std::string& s : splitList(topText)) {
        m_topPriority.push_back(normalizeItemName(s));
    }

    for (std::string s : splitList(catText)) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        m_categoryOrder.push_back(s);
    }
}

void ItemScroller::onScansReady()
{
    m_keySlots.fill(-1);
    const char* names[] = {gamebuttonlogic::button::shift, gamebuttonlogic::button::control,
        gamebuttonlogic::button::alt, gamebuttonlogic::button::pointerPressed,
        gamebuttonlogic::button::menuSecondarySelect, gamebuttonlogic::button::menuTertiarySelect};
    for (int i = 0; i < 6; ++i) m_namedButtons[i] = GameButtons::instance().watchButton(names[i]);
    m_useButton = GameButtons::instance().watchButton(gamebuttonlogic::button::buildOrInteract);
    m_sneakButton = GameButtons::instance().watchButton(gamebuttonlogic::button::sneak);
    m_wheelLeftButton = GameButtons::instance().watchButton(gamebuttonlogic::button::inventoryLeft);
    m_wheelRightButton = GameButtons::instance().watchButton(gamebuttonlogic::button::inventoryRight);
    watchKeys();
    cui::onScansReady();
    ItemStackOps::instance().onScansReady();
    cui::setListener(this);
    if (cui::available()) {
        registerUiDefinitions();
    }
    tradeui::onScansReady();
    tradeui::setListener(this);
    settingscommand::addExtra(name(), [this] {
        std::vector<MenuItem> items;
        items.push_back(menu::toggle(L"massCraftHold",
            [this] { return m_massCraftHold.load(std::memory_order_relaxed); },
            [this] {
                const bool on = !m_massCraftHold.load(std::memory_order_relaxed);
                m_massCraftHold.store(on, std::memory_order_relaxed);
                log().info(L"ItemScroller: massCraftHold {}", on ? L"ON" : L"OFF");
            }));
        return items;
    });
    if (!available() && enabled()) {
        log().warn(L"ItemScroller: the container screen could not be located; the module cannot work");
    }
}

void ItemScroller::shutdown()
{
    if (hooks::offstackScreenHeld()) {
        m_unloadRelease.store(1, std::memory_order_release);
        for (int waited = 0; waited < 1000 && m_unloadRelease.load(std::memory_order_acquire) != 2; ++waited) {
            Sleep(1);
        }
        if (m_unloadRelease.load(std::memory_order_acquire) != 2 && hooks::offstackScreenHeld()) {
            hooks::releaseOffstackScreen(L"unloading (the game thread did not come)");
        }
    }
    cui::setListener(nullptr);
    tradeui::setListener(nullptr);
    saveFavorites();
    saveRecipes();
}

void ItemScroller::onEnabledChanged(bool on)
{
    if (!on) {
        m_disableRequested.store(true, std::memory_order_release);
    }
}

void ItemScroller::applyDisable()
{
    if (m_disableRequested.exchange(false, std::memory_order_acq_rel)) {
        stopDrag();
        m_jobs.clear();
        tradeui::setUnlockTier(-1);
        tradeui::setFavoriteTier({}, {});
        m_unlockSent = -2;
        m_favTierSet = false;
    }
}

void ItemScroller::onUpdate()
{
    watchConfigFile();
}

namespace {
int mouseIndex(int vk)
{
    return vk == kLmb ? 0 : vk == kRmb ? 1 : vk == kMmb ? 2 : -1;
}
}

bool ItemScroller::keyHeld(int vk) const
{
    const int i = mouseIndex(vk);
    int named = i == 0 ? 3 : i == 1 ? 4 : i == 2 ? 5
        : vk == kShift ? 0 : vk == kCtrl ? 1 : vk == kAlt ? 2 : -1;
    if (named >= 0) return GameButtons::instance().buttonHeld(m_namedButtons[named]);
    return vk >= 0 && vk < static_cast<int>(m_keySlots.size())
        && GameButtons::instance().keyHeld(m_keySlots[vk]);
}

std::uint64_t ItemScroller::keySeq(int vk) const
{
    const int i = mouseIndex(vk);
    const int named = i == 0 ? 3 : i == 1 ? 4 : i == 2 ? 5
        : vk == kShift ? 0 : vk == kCtrl ? 1 : vk == kAlt ? 2 : -1;
    if (named >= 0) return GameButtons::instance().buttonPressSeq(m_namedButtons[named]);
    return vk >= 0 && vk < static_cast<int>(m_keySlots.size())
        ? GameButtons::instance().keyPressSeq(m_keySlots[vk]) : 0;
}

void ItemScroller::resyncKeySeqs()
{
    auto sync = [this](const std::vector<int>& combo, bool& wasDown, std::uint64_t& seen) {
        int trigger = 0;
        for (int vk : combo) if (!keys::isModifier(vk)) trigger = vk;
        seen = trigger ? keySeq(trigger) : 0;
        wasDown = comboHeld(combo, true);
    };
    for (KeySetting& setting : m_keys) sync(setting.keys, setting.wasDown, setting.seenSeq);
    if (!toggleKey().empty()) sync(toggleKey().combo(), m_toggleWasDown, m_toggleSeenSeq);
}

void ItemScroller::watchKeys()
{
    auto watch = [this](int vk) {
        if (vk >= 0 && vk < static_cast<int>(m_keySlots.size()) && mouseIndex(vk) < 0
            && vk != kShift && vk != kCtrl && vk != kAlt) {
            m_keySlots[vk] = GameButtons::instance().watchKey(vk);
        }
    };
    for (const KeySetting& setting : m_keys) for (int vk : setting.keys) watch(vk);
    for (int vk : {'Q', 'W', 'S'}) watch(vk);
    for (int vk = VK_NUMPAD1; vk <= VK_NUMPAD9; ++vk) watch(vk);
    for (int vk : {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT}) watch(vk);
}

int ItemScroller::recentMouseButton() const
{
    return keyHeld(kLmb) ? kLmb : kRmb;
}

bool ItemScroller::comboHeld(const std::vector<int>& combo, bool exactModifiers) const
{
    return comboHeldAt(combo, exactModifiers, 0);
}

bool ItemScroller::comboHeldAt(const std::vector<int>& combo, bool exactModifiers, int pressVk) const
{
    if (combo.empty()) {
        return false;
    }
    auto down = [this, pressVk](int vk) {
        if (vk == pressVk) {
            return true;
        }
        if (pressVk != 0 && mouseIndex(pressVk) >= 0 && mouseIndex(vk) >= 0) {
            return false;
        }
        return keyHeld(vk);
    };
    for (int vk : combo) {
        if (!down(vk)) {
            return false;
        }
    }
    if (!exactModifiers) {
        return true;
    }
    static constexpr int kSignificant[] = {kShift, kCtrl, kAlt, kLmb, kRmb, 'Q', 'W', 'S'};
    for (int vk : kSignificant) {
        if (std::find(combo.begin(), combo.end(), vk) == combo.end() && down(vk)) {
            return false;
        }
    }
    return true;
}

bool ItemScroller::comboEdge(const std::vector<int>& combo, bool& wasDown,
                             std::uint64_t& seenSeq)
{
    int trigger = 0;
    for (int vk : combo) if (!keys::isModifier(vk)) trigger = vk;
    if (trigger != 0) {
        const std::uint64_t now = keySeq(trigger);
        const bool edge = now != seenSeq && comboHeldAt(combo, true, trigger);
        seenSeq = now;
        return edge;
    }
    const bool down = comboHeld(combo, true);
    const bool edge = down && !wasDown;
    wasDown = down;
    return edge;
}

ItemScroller::Group ItemScroller::groupOf(const std::string& coll)
{
    if (coll == "hotbar_items" || coll == "inventory_items"
        || coll == "combined_hotbar_and_inventory_items") {
        return Group::Player;
    }
    if (coll == "container_items") {
        return Group::Storage;
    }
    if (coll == "crafting_input_items") {
        return Group::Grid;
    }
    if (coll == "crafting_output_items") {
        return Group::Output;
    }
    if (coll == "trade2_ingredient1_item" || coll == "trade2_ingredient2_item") {
        return Group::TradeIn;
    }
    if (coll == "trade2_result_item") {
        return Group::TradeOut;
    }
    if (coll == "cursor_items" || coll.empty()) {
        return Group::None;
    }
    return Group::Other;
}

bool ItemScroller::screenHasStorage() const
{
    return cui::collectionSize("container_items") > 0;
}

std::vector<ItemScroller::Slot> ItemScroller::slotsOf(Group group) const
{
    std::vector<Slot> out;
    auto add = [&out](const char* coll) {
        const int n = cui::collectionSize(coll);
        for (int i = 0; i < n; ++i) {
            out.push_back(Slot{coll, i});
        }
    };
    switch (group) {
    case Group::Player:
        if (cui::collectionSize("inventory_items") > 0 || cui::collectionSize("hotbar_items") > 0) {
            add("inventory_items");
            add("hotbar_items");
        } else {
            add("combined_hotbar_and_inventory_items");
        }
        break;
    case Group::Storage:
        add("container_items");
        break;
    case Group::Grid:
        add("crafting_input_items");
        break;
    case Group::TradeIn:
        add("trade2_ingredient1_item");
        add("trade2_ingredient2_item");
        break;
    default:
        break;
    }
    return out;
}

namespace {

int invId(const std::string& coll, bool splitPlayer)
{
    if (coll == "hotbar_items") {
        return splitPlayer ? 2 : 1;
    }
    if (coll == "inventory_items" || coll == "combined_hotbar_and_inventory_items") {
        return 1;
    }
    if (coll == "container_items") {
        return 3;
    }
    if (coll == "crafting_input_items") {
        return 4;
    }
    if (coll == "crafting_output_items") {
        return 5;
    }
    if (coll == "trade2_ingredient1_item" || coll == "trade2_ingredient2_item"
        || coll == "trade2_result_item") {
        return 6;
    }
    return 7;
}

}

std::vector<ItemScroller::Slot> ItemScroller::otherSlotsOf(const Slot& slot) const
{
    const bool split = !screenHasStorage();
    const int mine = invId(slot.coll, split);
    std::vector<Slot> all;
    for (Group g : {Group::Storage, Group::Player}) {
        for (const Slot& s : slotsOf(g)) {
            if (invId(s.coll, split) != mine) {
                all.push_back(s);
            }
        }
    }
    if (groupOf(slot.coll) != Group::Player && groupOf(slot.coll) != Group::Storage) {
        all.erase(std::remove_if(all.begin(), all.end(),
                                 [this](const Slot& s) { return groupOf(s.coll) != Group::Player; }),
                  all.end());
    }
    return all;
}

bool ItemScroller::budgetLeft() const
{
    return m_clicksThisTick < kSafeClicksPerTick;
}

namespace {
bool g_runningJobs = false;
constexpr int kMaxJobTicks = 2400;
}

void ItemScroller::pushJob(std::function<bool()> step)
{
    auto ticks = std::make_shared<int>(0);
    m_jobs.push_back([step = std::move(step), ticks]() mutable {
        if (step()) {
            return true;
        }
        return ++*ticks > kMaxJobTicks;
    });
    if (!g_runningJobs) {
        runJobs();
    }
}

void ItemScroller::runJobs()
{
    if (g_runningJobs) {
        return;
    }
    g_runningJobs = true;
    for (int guard = 0; guard < 256 && !m_jobs.empty() && budgetLeft(); ++guard) {
        std::function<bool()> step = m_jobs.front();
        if (!step()) {
            break;
        }
        if (!m_jobs.empty()) {
            m_jobs.pop_front();
        }
    }
    g_runningJobs = false;
}

void ItemScroller::pushListJob(std::function<std::vector<Slot>()> collect, std::function<bool(const Slot&)> unit)
{
    struct State {
        std::function<std::vector<Slot>()> collect;
        std::function<bool(const Slot&)> unit;
        std::vector<Slot> slots;
        size_t next = 0;
        bool started = false;
    };
    auto st = std::make_shared<State>();
    st->collect = std::move(collect);
    st->unit = std::move(unit);
    pushJob([this, st]() {
        if (!st->started) {
            st->slots = st->collect();
            st->started = true;
        }
        while (st->next < st->slots.size()) {
            if (!budgetLeft()) {
                return false;
            }
            const Slot s = st->slots[st->next++];
            if (!st->unit(s)) {
                return true;
            }
        }
        return true;
    });
}

bool ItemScroller::clickOne(const Slot& s, int kind)
{
    ++m_clicksThisTick;
    const bool ok = (kind < 0) ? cui::dropCursor(true)
                               : cui::click(s.coll, s.index, static_cast<cui::Click>(kind));

    return ok;
}
bool ItemScroller::leftClick(const Slot& s)
{
    return clickOne(s, static_cast<int>(cui::Click::Left));
}
bool ItemScroller::rightClick(const Slot& s)
{
    return clickOne(s, static_cast<int>(cui::Click::Right));
}
bool ItemScroller::shiftClick(const Slot& s)
{
    return clickOne(s, static_cast<int>(cui::Click::Shift));
}
bool ItemScroller::dropOne(const Slot& s)
{
    return clickOne(s, static_cast<int>(cui::Click::DropOne));
}
bool ItemScroller::dropStack(const Slot& s)
{
    return clickOne(s, static_cast<int>(cui::Click::DropAll));
}
bool ItemScroller::dropCursorAll()
{
    return clickOne(Slot{}, -1);
}

const void* ItemScroller::stackOf(const Slot& s) const
{
    return cui::stackAt(s.coll, s.index);
}
bool ItemScroller::isEmptySlot(const Slot& s) const
{
    return cui::isEmpty(stackOf(s));
}
int ItemScroller::countIn(const Slot& s) const
{
    return cui::countOf(stackOf(s));
}
const void* ItemScroller::cursor() const
{
    return cui::cursorStack();
}
bool ItemScroller::cursorEmpty() const
{
    return cui::isEmpty(cursor());
}

bool ItemScroller::shiftClickWithCheck(const Slot& s)
{
    const int before = countIn(s);
    shiftClick(s);
    return isEmptySlot(s) || countIn(s) != before;
}

void ItemScroller::moveItemsToTargets(const Slot& source, const std::vector<Slot>& targets, bool one)
{
    if (!cursorEmpty() || isEmptySlot(source)) {
        return;
    }
    StackCopy ref(stackOf(source));
    const void* item = ref.get() ? ref.get() : stackOf(source);
    std::vector<Slot> order;
    for (const Slot& t : targets) {
        const void* st = stackOf(t);
        if (!cui::isEmpty(st) && cui::sameItem(st, item) && cui::countOf(st) < cui::maxStackOf(st)) {
            order.push_back(t);
        }
    }
    for (const Slot& t : targets) {
        if (isEmptySlot(t)) {
            order.push_back(t);
        }
    }
    if (order.empty()) {
        return;
    }
    leftClick(source);
    if (cursorEmpty()) {
        return;
    }
    for (const Slot& t : order) {
        if (cursorEmpty()) {
            break;
        }
        if (one) {
            rightClick(t);
            break;
        }
        leftClick(t);
    }
    if (!cursorEmpty()) {
        leftClick(source);
    }
}

void ItemScroller::enqueueMoveStacksFrom(const Slot& slot, bool matchingOnly, bool toOther, bool firstOnly)
{
    enqueueMoveStacks(std::make_shared<StackRef>(stackOf(slot)), slot, matchingOnly, toOther, firstOnly);
}

void ItemScroller::enqueueMoveStacks(std::shared_ptr<StackRef> ref, const Slot& slot, bool matchingOnly,
                                     bool toOther, bool firstOnly)
{
    auto collect = [this, slot, toOther]() {
        const bool split = !screenHasStorage();
        const int mine = invId(slot.coll, split);
        std::vector<Slot> all;
        for (Group g : {Group::Storage, Group::Player}) {
            for (const Slot& s : slotsOf(g)) {
                all.push_back(s);
            }
        }
        std::vector<Slot> out;
        for (auto it = all.rbegin(); it != all.rend(); ++it) {
            const Slot& s = *it;
            if (s == slot || (invId(s.coll, split) == mine) != toOther) {
                continue;
            }
            out.push_back(s);
        }
        if (toOther) {
            out.push_back(slot);
        }
        return out;
    };
    auto unit = [this, ref, slot, matchingOnly, firstOnly](const Slot& s) {
        if (!cursorEmpty()) {
            return false;
        }
        const void* st = stackOf(s);
        if (cui::isEmpty(st)) {
            return true;
        }
        if (!(s == slot) && matchingOnly && (ref == nullptr || ref->get() == nullptr || !cui::sameItem(st, ref->get()))) {
            return true;
        }

        (void)shiftClickWithCheck(s);
        return !firstOnly || s == slot;
    };
    pushListJob(collect, unit);
}

bool ItemScroller::tryMoveSingleItemToOtherInventory(const Slot& slot)
{
    if (!cursorEmpty() || isEmptySlot(slot)) {
        return false;
    }
    moveItemsToTargets(slot, otherSlotsOf(slot), true);
    return true;
}

bool ItemScroller::tryMoveAllButOneItemToOtherInventory(const Slot& slot)
{
    if (!cursorEmpty() || countIn(slot) <= 1) {
        return false;
    }
    StackCopy ref(stackOf(slot));
    const std::vector<Slot> others = otherSlotsOf(slot);
    std::vector<Slot> order;
    for (const Slot& t : others) {
        const void* st = stackOf(t);
        if (!cui::isEmpty(st) && ref.get() && cui::sameItem(st, ref.get())
            && cui::countOf(st) < cui::maxStackOf(st)) {
            order.push_back(t);
        }
    }
    for (const Slot& t : others) {
        if (isEmptySlot(t)) {
            order.push_back(t);
        }
    }
    if (order.empty()) {
        return false;
    }
    leftClick(slot);
    rightClick(slot);
    for (const Slot& t : order) {
        if (cursorEmpty()) {
            break;
        }
        leftClick(t);
    }
    if (!cursorEmpty()) {
        leftClick(slot);
    }
    return true;
}

bool ItemScroller::tryMoveSingleItemToThisInventory(const Slot& slot)
{
    const void* here = stackOf(slot);
    if (!cursorEmpty() || cui::isEmpty(here) || cui::countOf(here) >= cui::maxStackOf(here)) {
        return false;
    }
    const std::vector<Slot> others = otherSlotsOf(slot);
    for (auto it = others.rbegin(); it != others.rend(); ++it) {
        const void* st = stackOf(*it);
        if (!cui::isEmpty(st) && cui::sameItem(st, here)) {
            leftClick(*it);
            rightClick(slot);
            if (!cursorEmpty()) {
                leftClick(*it);
            }
            return true;
        }
    }
    return false;
}

void ItemScroller::enqueueDropStacks(std::shared_ptr<StackRef> ref, const Slot& refSlot)
{
    if (ref == nullptr || ref->get() == nullptr) {
        return;
    }
    auto collect = [this, refSlot]() {
        const bool split = false;
        const int mine = invId(refSlot.coll, split);
        std::vector<Slot> out;
        for (Group g : {Group::Storage, Group::Player, Group::Grid, Group::TradeIn}) {
            for (const Slot& s : slotsOf(g)) {
                if (invId(s.coll, split) == mine) {
                    out.push_back(s);
                }
            }
        }
        return out;
    };
    auto unit = [this, ref](const Slot& s) {
        const void* st = stackOf(s);
        if (!cui::isEmpty(st) && cui::sameItem(st, ref->get())) {
            dropStack(s);
        }
        return true;
    };
    pushListJob(collect, unit);
}

bool ItemScroller::shiftPlaceItems(const Slot& slot)
{
    auto ref = std::make_shared<StackRef>(cursor());
    leftClick(slot);
    m_dragged.insert(slot);
    enqueueMoveStacks(ref, slot, true, false, false);
    return true;
}

bool ItemScroller::shiftDropItems()
{
    if (cursorEmpty() || !m_cursorSource.valid()) {
        return false;
    }
    auto ref = std::make_shared<StackRef>(cursor());
    dropCursorAll();
    enqueueDropStacks(ref, m_cursorSource);
    return true;
}

void ItemScroller::dropLeaveOne(const Slot& slot)
{
    if (!cursorEmpty() || countIn(slot) <= 1) {
        return;
    }
    leftClick(slot);
    rightClick(slot);
    dropCursorAll();
}

int ItemScroller::rowOf(const Slot& s) const
{
    const int storage = cui::collectionSize("container_items");
    const int storageRows = storage > 0 ? (storage + 8) / 9 : 0;
    if (s.coll == "container_items") {
        const int width = (storage % 9 == 0) ? 9 : (storage == 5 ? 5 : 3);
        return s.index / width;
    }
    if (s.coll == "inventory_items") {
        return storageRows + 1 + s.index / 9;
    }
    if (s.coll == "hotbar_items") {
        return storageRows + 5;
    }
    return -1000;
}

bool ItemScroller::tryMoveItemsVertically(const Slot& slot, bool up, Amount amount)
{
    if (!cursorEmpty() || isEmptySlot(slot)) {
        return false;
    }
    const int myRow = rowOf(slot);
    auto refHolder = std::make_shared<StackRef>(stackOf(slot));
    const StackRef& ref = *refHolder;
    struct Cand {
        Slot s;
        int row;
    };
    std::vector<Cand> cands;
    for (Group g : {Group::Storage, Group::Player}) {
        for (const Slot& s : slotsOf(g)) {
            if (s == slot) {
                continue;
            }
            const int r = rowOf(s);
            if (r == myRow || (r < myRow) != up) {
                continue;
            }
            const void* st = stackOf(s);
            const bool ok = cui::isEmpty(st)
                            || (ref.get() && cui::sameItem(st, ref.get())
                                && cui::countOf(st) < cui::maxStackOf(st));
            if (ok) {
                cands.push_back(Cand{s, r});
            }
        }
    }

    if (cands.empty()) {
        return false;
    }
    std::stable_sort(cands.begin(), cands.end(),
                     [up](const Cand& a, const Cand& b) { return up ? a.row < b.row : a.row > b.row; });
    std::vector<Slot> targets;
    for (const Cand& c : cands) {
        targets.push_back(c.s);
    }
    switch (amount) {
    case Amount::One:
        moveItemsToTargets(slot, targets, true);
        break;
    case Amount::Stacks:
        moveItemsToTargets(slot, targets, false);
        break;
    case Amount::LeaveOne: {
        if (countIn(slot) <= 1) {
            return false;
        }
        leftClick(slot);
        rightClick(slot);
        for (const Slot& t : targets) {
            if (cursorEmpty()) {
                break;
            }
            const void* st = stackOf(t);
            if (cui::isEmpty(st) || cui::sameItem(st, cursor())) {
                leftClick(t);
            }
        }
        if (!cursorEmpty()) {
            leftClick(slot);
        }
        break;
    }
    case Amount::Matching: {
        auto collect = [this, slot, up, refHolder]() {
            std::vector<Cand> sources;
            const bool split = true;
            const int mine = invId(slot.coll, split);
            for (Group g : {Group::Storage, Group::Player}) {
                for (const Slot& s : slotsOf(g)) {
                    const void* st = stackOf(s);
                    if (invId(s.coll, split) == mine && !cui::isEmpty(st) && refHolder->get()
                        && cui::sameItem(st, refHolder->get())) {
                        sources.push_back(Cand{s, rowOf(s)});
                    }
                }
            }
            std::stable_sort(sources.begin(), sources.end(), [up](const Cand& a, const Cand& b) {
                return up ? a.row > b.row : a.row < b.row;
            });
            std::vector<Slot> out;
            for (const Cand& c : sources) {
                out.push_back(c.s);
            }
            return out;
        };
        auto unit = [this, up, refHolder](const Slot& src) {
            if (!cursorEmpty() || isEmptySlot(src) || !cui::sameItem(stackOf(src), refHolder->get())) {
                return cursorEmpty();
            }
            const int srcRow = rowOf(src);
            std::vector<Cand> dstCands;
            for (Group g : {Group::Storage, Group::Player}) {
                for (const Slot& s : slotsOf(g)) {
                    const int r = rowOf(s);
                    if (s == src || r == srcRow || (r < srcRow) != up) {
                        continue;
                    }
                    const void* st = stackOf(s);
                    if (cui::isEmpty(st)
                        || (cui::sameItem(st, refHolder->get()) && cui::countOf(st) < cui::maxStackOf(st))) {
                        dstCands.push_back(Cand{s, r});
                    }
                }
            }
            std::stable_sort(dstCands.begin(), dstCands.end(),
                             [up](const Cand& a, const Cand& b) { return up ? a.row < b.row : a.row > b.row; });
            std::vector<Slot> dst;
            for (const Cand& c : dstCands) {
                dst.push_back(c.s);
            }
            if (dst.empty()) {
                return false;
            }
            moveItemsToTargets(src, dst, false);
            return true;
        };
        pushListJob(collect, unit);
        break;
    }
    }
    return true;
}

bool ItemScroller::tryMoveItemsByScroll(const Slot& slot, bool scrollingUp)
{
    if (!cursorEmpty()) {
        return false;
    }
    const bool everything = comboHeld(m_keys[kModifierMoveEverything].keys, true);
    const bool matching = comboHeld(m_keys[kModifierMoveMatching].keys, true);
    const bool stacks = comboHeld(m_keys[kModifierMoveStack].keys, true);
    bool toOther = scrollingUp;

    const Group g = groupOf(slot.coll);
    if (g == Group::Output && isCraftingScreen()) {
        return tryMoveItemsCrafting(slot, toOther, stacks, everything);
    }
    if (g == Group::TradeOut) {
        return tryMoveItemsVillager(slot, toOther, stacks);
    }
    if (isEmptySlot(slot)) {
        return false;
    }
    if (isBundleName(cui::itemName(stackOf(slot)))) {
        return false;
    }

    if (everything) {
        enqueueMoveStacksFrom(slot, false, toOther, false);
    } else if (matching) {
        enqueueMoveStacksFrom(slot, true, toOther, false);
    } else if (stacks) {
        enqueueMoveStacksFrom(slot, true, toOther, true);
    } else if (toOther) {
        pushJob([this, slot]() {
            tryMoveSingleItemToOtherInventory(slot);
            return true;
        });
    } else {
        pushJob([this, slot]() {
            tryMoveSingleItemToThisInventory(slot);
            return true;
        });
    }
    return true;
}

ItemScroller::DragAction ItemScroller::matchDrag(int mouseVk) const
{
    struct Pair {
        KeyId key;
        DragAction action;
    };
    static constexpr Pair kPairs[] = {
        {kKeyDragDropStacks, DragAction::DropStacks},   {kKeyDragDropLeaveOne, DragAction::DropLeaveOne},
        {kWsMoveUpStacks, DragAction::UpStacks},        {kWsMoveUpMatching, DragAction::UpMatching},
        {kWsMoveDownStacks, DragAction::DownStacks},    {kWsMoveDownMatching, DragAction::DownMatching},
        {kKeyDragDropSingle, DragAction::DropOne},      {kWsMoveUpLeaveOne, DragAction::UpLeaveOne},
        {kWsMoveUpSingle, DragAction::UpOne},           {kWsMoveDownLeaveOne, DragAction::DownLeaveOne},
        {kWsMoveDownSingle, DragAction::DownOne},       {kKeyDragMoveStacks, DragAction::MoveStacks},
        {kKeyDragMoveLeaveOne, DragAction::MoveLeaveOne}, {kKeyDragMoveMatching, DragAction::MoveMatching},
        {kKeyDragMoveOne, DragAction::MoveOne},
    };
    for (const Pair& p : kPairs) {
        const std::vector<int>& combo = m_keys[p.key].keys;
        if (std::find(combo.begin(), combo.end(), mouseVk) == combo.end()) {
            continue;
        }
        if (comboHeldAt(combo, true, mouseVk)) {
            return p.action;
        }
    }
    return DragAction::None;
}

bool ItemScroller::dragActionHeld(DragAction action) const
{
    KeyId key = kKeyCount;
    switch (action) {
    case DragAction::MoveStacks: key = kKeyDragMoveStacks; break;
    case DragAction::MoveLeaveOne: key = kKeyDragMoveLeaveOne; break;
    case DragAction::MoveOne: key = kKeyDragMoveOne; break;
    case DragAction::MoveMatching: key = kKeyDragMoveMatching; break;
    case DragAction::DropOne: key = kKeyDragDropSingle; break;
    case DragAction::DropLeaveOne: key = kKeyDragDropLeaveOne; break;
    case DragAction::DropStacks: key = kKeyDragDropStacks; break;
    case DragAction::UpStacks: key = kWsMoveUpStacks; break;
    case DragAction::UpMatching: key = kWsMoveUpMatching; break;
    case DragAction::UpLeaveOne: key = kWsMoveUpLeaveOne; break;
    case DragAction::UpOne: key = kWsMoveUpSingle; break;
    case DragAction::DownStacks: key = kWsMoveDownStacks; break;
    case DragAction::DownMatching: key = kWsMoveDownMatching; break;
    case DragAction::DownLeaveOne: key = kWsMoveDownLeaveOne; break;
    case DragAction::DownOne: key = kWsMoveDownSingle; break;
    default: return false;
    }
    return comboHeld(m_keys[key].keys, false);
}

void ItemScroller::dragApply(const Slot& slot, DragAction action)
{

    switch (action) {
    case DragAction::MoveMatching:
        enqueueMoveStacksFrom(slot, true, true, false);
        return;
    case DragAction::UpMatching:
        tryMoveItemsVertically(slot, true, Amount::Matching);
        return;
    case DragAction::DownMatching:
        tryMoveItemsVertically(slot, false, Amount::Matching);
        return;
    default:
        break;
    }
    pushJob([this, slot, action]() {
        if (!cursorEmpty() || isEmptySlot(slot)) {
            return true;
        }
        switch (action) {
        case DragAction::MoveOne: tryMoveSingleItemToOtherInventory(slot); break;
        case DragAction::MoveLeaveOne: tryMoveAllButOneItemToOtherInventory(slot); break;
        case DragAction::MoveStacks: shiftClickWithCheck(slot); break;
        case DragAction::DropOne: dropOne(slot); break;
        case DragAction::DropLeaveOne: dropLeaveOne(slot); break;
        case DragAction::DropStacks: dropStack(slot); break;
        case DragAction::UpStacks: tryMoveItemsVertically(slot, true, Amount::Stacks); break;
        case DragAction::UpLeaveOne: tryMoveItemsVertically(slot, true, Amount::LeaveOne); break;
        case DragAction::UpOne: tryMoveItemsVertically(slot, true, Amount::One); break;
        case DragAction::DownStacks: tryMoveItemsVertically(slot, false, Amount::Stacks); break;
        case DragAction::DownLeaveOne: tryMoveItemsVertically(slot, false, Amount::LeaveOne); break;
        case DragAction::DownOne: tryMoveItemsVertically(slot, false, Amount::One); break;
        default: break;
        }
        return true;
    });
}

void ItemScroller::dragOver(const Slot& slot, bool isStart)
{
    if (m_drag == DragAction::None || !slot.valid()) {
        return;
    }
    std::vector<Slot> path;
    if (!isStart && m_dragLast.valid() && m_dragLast.coll == slot.coll && !(m_dragLast == slot)) {
        const int x0 = m_dragLast.index % 9, y0 = m_dragLast.index / 9;
        const int x1 = slot.index % 9, y1 = slot.index / 9;
        const int steps = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
        for (int i = 1; i < steps; ++i) {
            const int x = x0 + (x1 - x0) * i / steps;
            const int y = y0 + (y1 - y0) * i / steps;
            path.push_back(Slot{slot.coll, y * 9 + x});
        }
    }
    path.push_back(slot);
    const bool oncePerSlot = (m_drag == DragAction::MoveOne || m_drag == DragAction::DropOne
                              || m_drag == DragAction::UpOne || m_drag == DragAction::DownOne);
    for (const Slot& s : path) {
        if (s == m_dragLast && !isStart) {
            continue;
        }
        if (oncePerSlot && m_dragged.count(s) != 0) {
            continue;
        }
        if (!cursorEmpty()) {
            stopDrag();
            return;
        }
        if (!isEmptySlot(s)) {
            dragApply(s, m_drag);
        }
        m_dragged.insert(s);
        m_dragLast = s;
    }
}

void ItemScroller::stopDrag()
{
    if (m_drag != DragAction::None) {
    }
    m_drag = DragAction::None;
    m_dragMouseVk = 0;
    m_dragLast = Slot{};
    m_dragged.clear();
}

bool ItemScroller::onSlotButton(std::uint32_t id, int state, const std::string& coll, int index)
{
    applyDisable();
    if (!enabled()) {
        return false;
    }
    if (m_swallowId != 0 && id == m_swallowId && state != cui::kStatePressed) {
        if (state == cui::kStateReleased) {
            m_swallowId = 0;
        }
        return true;
    }
    const Slot slot{coll, index};

    if (id == cui::button::kHover) {
        if (m_drag != DragAction::None) {
            if (dragActionHeld(m_drag)) {
                dragOver(slot, false);
            } else {
                stopDrag();
            }
        }
        return false;
    }
    if (state != cui::kStatePressed) {
        return false;
    }

    bool swallow = false;
    const bool isAutoPlace = (id == cui::button::kAutoPlace || id == cui::button::kOutputTertiary);
    const bool isLeftish = (id == cui::button::kTakeAllPlaceAll || isAutoPlace
                            || id == cui::button::kCoalesce || id == cui::button::kOutputPrimary);
    const bool isRight = (id == cui::button::kTakeHalfPlaceOne || id == cui::button::kOutputSecondary);
    const bool slotButton = isLeftish || isRight || id == cui::button::kDropOne
                            || id == cui::button::kDropAll;

    if (id == cui::button::kCursorDropAll || id == cui::button::kCursorDropOne) {
        if (keyHeld(kShift) && !cursorEmpty()) {
            swallow = shiftDropItems();
        }
    } else if (slotButton && slot.valid()) {
        const int mouseVk = isRight ? kRmb : recentMouseButton();
        if (cursorEmpty() && (id == cui::button::kTakeAllPlaceAll || isRight)
            && !isOutputSlot(slot)) {
            m_cursorSource = slot;
        }
        if (isRight && isOutputSlot(slot) && isCraftingScreen()) {
            rightClickCraftOneStack(slot);
            swallow = true;
        } else if (isLeftish && comboHeldAt(m_keys[kKeyMoveEverything].keys, true, mouseVk)) {
            if (cursorEmpty() && !isEmptySlot(slot)) {
                enqueueMoveStacksFrom(slot, false, true, false);
            }
            swallow = true;
        } else if (isAutoPlace && !cursorEmpty()
                   && isEmptySlot(slot) && groupOf(slot.coll) != Group::Output) {
            shiftPlaceItems(slot);
            swallow = true;
        } else if (id != cui::button::kDropOne && id != cui::button::kDropAll) {
            const DragAction action = matchDrag(mouseVk);
            if (action != DragAction::None && cursorEmpty()) {
                m_drag = action;
                m_dragMouseVk = mouseVk;
                m_dragged.clear();
                m_dragLast = Slot{};
                dragOver(slot, true);
                swallow = true;
            }
        }
    }
    if (swallow) {
        m_swallowId = id;
    }
    return swallow;
}

void ItemScroller::onScreenLost()
{
    applyDisable();
    m_wheelPrimed = false;
    stopDrag();
    m_swallowId = 0;
    m_jobs.clear();
    tradeui::setClientScreen(nullptr);
    tradeui::setFavoriteTier({}, {});
    tradeui::setUnlockTier(-1);
    m_favTier.clear();
    m_favTierSet = false;
    m_autoTradeCtrl = nullptr;
    m_autoTradeWanted = false;
    m_autoTradeOffstack = false;
    m_autoTradePhase = 0;
    m_autoTradeTicks = 0;
    m_autoTradeGains.clear();
    m_unlockSent = -2;
    m_orderSig.clear();
    m_orderTicks = 0;
    m_recipeViewOpen = false;
    m_viewHover = -1;
    m_viewPress = -1;
    m_tradeJobs.clear();
    m_tradeCtrl = nullptr;
    tradeui::forgetHover();
    saveFavorites();
    m_cursorSource = Slot{};
    resyncWheelSeqs();
    saveRecipes();
}

void ItemScroller::consumeWheel()
{
    const auto& buttons = GameButtons::instance();
    const std::uint64_t left = buttons.buttonPressSeq(m_wheelLeftButton);
    const std::uint64_t right = buttons.buttonPressSeq(m_wheelRightButton);
    const int notches = gamebuttonlogic::wheelNotches(left, m_wheelLeftSeen, right, m_wheelRightSeen);
    m_wheelLeftSeen = left;
    m_wheelRightSeen = right;
    if (notches == 0) {
        return;
    }
    if (m_recipeViewOpen) {
        changeRecipeSelection(m_selectedRecipe + (notches < 0 ? 1 : -1) * std::abs(notches));
        return;
    }
    if (ShulkerPreview::instance().ownsWheel()) {
        return;
    }
    std::string coll;
    int index = -1;
    if (!cui::hovered(coll, index)) {
        return;
    }
    const Slot slot{coll, index};
    const int step = notches > 0 ? 1 : -1;
    for (int n = 0; n < std::abs(notches); ++n) {
        tryMoveItemsByScroll(slot, step > 0);
    }

}

void ItemScroller::resyncWheelSeqs()
{
    const auto& buttons = GameButtons::instance();
    m_wheelLeftSeen = buttons.buttonPressSeq(m_wheelLeftButton);
    m_wheelRightSeen = buttons.buttonPressSeq(m_wheelRightButton);
}

unsigned long long ItemScroller::wheelLastMs() const
{
    const auto& buttons = GameButtons::instance();
    return std::max(buttons.buttonLastPressMs(m_wheelLeftButton),
                    buttons.buttonLastPressMs(m_wheelRightButton));
}

void ItemScroller::pollHotkeys()
{
    std::string coll;
    int index = -1;
    const bool over = cui::hovered(coll, index);
    const Slot hovered{coll, index};

    if (!toggleKey().empty()) {
        for (int vk : toggleKey().combo()) {
            if (vk >= 0 && vk < static_cast<int>(m_keySlots.size()) && m_keySlots[vk] < 0 && mouseIndex(vk) < 0
                && vk != kShift && vk != kCtrl && vk != kAlt) {
                m_keySlots[vk] = GameButtons::instance().watchKey(vk);
                m_toggleSeenSeq = keySeq(vk);
            }
        }
        if (comboEdge(toggleKey().combo(), m_toggleWasDown, m_toggleSeenSeq)) {
            toggleByKey();
            log().info(L"ItemScroller: toggled {}", enabled() ? L"ON" : L"OFF");
            return;
        }
    }
    auto edge = [this](KeyId id) { return comboEdge(m_keys[id].keys, m_keys[id].wasDown,
                                                   m_keys[id].seenSeq); };

    if (edge(kDropAllMatching) && over && !isEmptySlot(hovered)) {
        enqueueDropStacks(std::make_shared<StackRef>(stackOf(hovered)), hovered);
    }
    if (edge(kSortInventory) && over) {
        sortInventory(hovered);
    }
    if (isCraftingScreen()) {
        if (edge(kCraftEverything)) {
            craftEverything();
        }
        if (edge(kThrowCraftResults)) {
            enqueueThrowCraftResults();
        }
        if (edge(kMoveCraftResults)) {
            enqueueMoveCraftResults();
        }
    }
    if (edge(kVillagerTradeFavorites) && isTradeScreen()) {
        tradeFavorites();
    }
}

void ItemScroller::onScreenTick()
{
    applyDisable();
    if (!m_wheelPrimed) {
        resyncKeySeqs();
        resyncWheelSeqs();
        m_wheelPrimed = true;
        m_wheelCarry = GetTickCount64() - wheelLastMs() < kWheelCarryGapMs;

    } else if (m_wheelCarry) {
        resyncWheelSeqs();
        if (GetTickCount64() - wheelLastMs() >= kWheelCarryGapMs) {
            m_wheelCarry = false;
        }
    }
    if (m_autoTradeOffstack) {
        resyncWheelSeqs();
        if (!enabled()) {
            return;
        }
        m_clicksThisTick = 0;
        runJobs();
        tradeScreenTick(true);
        runJobs();
        return;
    }
    if (!enabled() || !input::isGameForeground()) {
        resyncWheelSeqs();
        m_recipeViewOpen = false;
        updateRecipeView();
        return;
    }
    m_clicksThisTick = 0;
    applyPendingKeys();
    if (m_drag != DragAction::None && (!keyHeld(m_dragMouseVk) || !dragActionHeld(m_drag))) {
        stopDrag();
    }
    runJobs();
    const bool viewWasOpen = m_recipeViewOpen;
    m_recipeViewOpen = isCraftingScreen()
                       && comboHeld(m_keys[kRecipeView].keys, false);
    if (m_recipeViewOpen) {
        if (!viewWasOpen) {
            for (int k = 0; k < 9; ++k) m_viewKeysSeq[k] = keySeq(VK_NUMPAD1 + k);
            const int arrows[] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT};
            for (int k = 0; k < 4; ++k) m_viewKeysSeq[k + 9] = keySeq(arrows[k]);
        }
        onRecipeViewKeys();
    }
    handleViewInput();
    consumeWheel();
    pollHotkeys();
    applyDisable();
    if (!enabled()) {
        m_recipeViewOpen = false;
        updateRecipeView();
        return;
    }
    if (isCraftingScreen() && m_jobs.empty()) {
        massCraftTick();
    }
    tradeScreenTick(false);
    updateRecipeView();
    runJobs();
}

int ItemScroller::customPriority(const void* stack) const
{
    if (cui::isEmpty(stack)) {
        return -1;
    }
    const std::string id = cui::itemName(stack);
    auto find = [&id](const std::vector<std::string>& list) {
        for (size_t i = 0; i < list.size(); ++i) {
            if (list[i] == id) {
                return static_cast<int>(i);
            }
        }
        return -1;
    };
    const int top = find(m_topPriority);
    if (top >= 0) {
        return -static_cast<int>(m_topPriority.size()) + top - 2;
    }

    return -1;
}

const char* ItemScroller::categoryName(const void* stack)
{
    switch (cui::creativeCategoryOf(stack)) {
    case 1: return "construction";
    case 2: return "nature";
    case 3: return "equipment";
    case 4: return "items";
    default: return "other";
    }
}

int ItemScroller::categoryIndex(const void* stack) const
{
    const std::string cat = categoryName(stack);
    int other = -1;
    for (size_t i = 0; i < m_categoryOrder.size(); ++i) {
        if (m_categoryOrder[i] == cat) {
            return static_cast<int>(i);
        }
        if (m_categoryOrder[i] == "other") {
            other = static_cast<int>(i);
        }
    }
    return other >= 0 ? other : static_cast<int>(m_categoryOrder.size());
}

ItemScroller::SortExtra ItemScroller::sortExtraOf(const void* stack) const
{
    SortExtra x;
    if (cui::isEmpty(stack)) {
        return x;
    }
    const std::string name = cui::itemName(stack);

    if (isShulkerName(name)) {
        int used = 0;
        int total = 0;
        if (cui::nbtContents(stack, used, total)) {
            x.boxSlots = used;
        }
    } else if (isBundleName(name)) {
        int current = 0;
        int capacity = 0;
        if (cui::storageFill(stack, current, capacity)) {
            x.bundleFill = current;
        }
    }
    return x;
}

int ItemScroller::compareStacks(const void* a, const void* b, const SortExtra& xa, const SortExtra& xb) const
{
    auto keyOf = [this](const void* stack, const SortExtra& extra) {
        itemscrollerlogic::SortKey key;
        key.empty = cui::isEmpty(stack);
        if (key.empty) return key;
        key.name = cui::itemName(stack);
        key.box = isShulkerName(key.name);
        key.bundle = isBundleName(key.name);
        key.priority = customPriority(stack);
        key.category = categoryIndex(stack);
        key.count = cui::countOf(stack);
        key.aux = cui::auxOf(stack);
        key.fill = key.box ? extra.boxSlots : extra.bundleFill;
        return key;
    };
    return itemscrollerlogic::compare(keyOf(a, xa), keyOf(b, xb));
}

void ItemScroller::sortInventory(const Slot& focused)
{
    const Group g = groupOf(focused.coll);
    if (g != Group::Player && g != Group::Storage) {
        return;
    }
    struct Sort {
        std::vector<Slot> region;
        int phase = 0;
        size_t i = 0;
        size_t j = 0;
        std::vector<int> cls;
        std::vector<int> target;
        std::vector<bool> clsIsBundle;
        int pass = 0;
        bool moved = false;
        int safety = 0;
    };
    auto st = std::make_shared<Sort>();
    const int n = cui::collectionSize(focused.coll);
    for (int i = 0; i < n; ++i) {
        st->region.push_back(Slot{focused.coll, i});
    }
    if (st->region.size() < 2) {
        return;
    }

    pushJob([this, st]() {
        Sort& s = *st;
        const size_t count = s.region.size();
        if (s.phase == 0) {
            if (!cursorEmpty()) {
                for (const Slot& r : s.region) {
                    if (isEmptySlot(r)) {
                        leftClick(r);
                        break;
                    }
                }
                if (!cursorEmpty()) {
                    return true;
                }
            }
            s.phase = 1;
            s.i = 0;
            s.j = count;
        }
        auto mergeStacks = [this, &s, count]() {
            for (; s.i < count; ++s.i, s.j = count) {
                const void* si = stackOf(s.region[s.i]);
                if (cui::isEmpty(si) || cui::countOf(si) >= cui::maxStackOf(si)) {
                    continue;
                }
                while (s.j-- > s.i + 1) {
                    si = stackOf(s.region[s.i]);
                    if (cui::countOf(si) >= cui::maxStackOf(si)) {
                        break;
                    }
                    const void* sj = stackOf(s.region[s.j]);
                    if (!cui::isEmpty(sj) && cui::sameItem(si, sj)) {
                        if (!budgetLeft()) {
                            ++s.j;
                            return false;
                        }
                        leftClick(s.region[s.j]);
                        leftClick(s.region[s.i]);
                        if (!cursorEmpty()) {
                            leftClick(s.region[s.j]);
                        }
                    }
                }
            }
            return true;
        };
        if (s.phase == 1) {
            if (!mergeStacks()) {
                return false;
            }
            s.phase = 2;
        }
        if (s.phase == 2) {
            std::vector<const void*> live(count);
            std::vector<SortExtra> extra(count);
            std::vector<int> order(count);
            for (size_t i = 0; i < count; ++i) {
                live[i] = stackOf(s.region[i]);
                extra[i] = sortExtraOf(live[i]);
                order[i] = static_cast<int>(i);
            }
            std::stable_sort(order.begin(), order.end(), [this, &live, &extra](int x, int y) {
                const auto ux = static_cast<size_t>(x);
                const auto uy = static_cast<size_t>(y);
                return compareStacks(live[ux], live[uy], extra[ux], extra[uy]) < 0;
            });
            s.cls.assign(count, 0);
            s.clsIsBundle.assign(1, false);
            std::vector<int> reps;
            for (size_t i = 0; i < count; ++i) {
                const void* item = live[i];
                if (cui::isEmpty(item)) {
                    continue;
                }
                int found = 0;
                if (cui::maxStackOf(item) > 1) {
                    for (size_t r = 0; r < reps.size(); ++r) {
                        const void* rep = live[static_cast<size_t>(reps[r])];
                        if (cui::maxStackOf(rep) > 1 && cui::sameItem(item, rep)) {
                            found = static_cast<int>(r) + 1;
                            break;
                        }
                    }
                }
                if (found == 0) {
                    reps.push_back(static_cast<int>(i));
                    found = static_cast<int>(reps.size());
                    s.clsIsBundle.push_back(isBundleName(cui::itemName(item)));
                }
                s.cls[i] = found;
            }
            s.target.assign(count, 0);
            for (size_t pos = 0; pos < count; ++pos) {
                s.target[pos] = s.cls[static_cast<size_t>(order[pos])];
            }
            s.phase = 3;
            s.pass = 0;
            s.i = 0;
            s.moved = false;
            s.safety = static_cast<int>(count) * 8 + 32;
        }
        auto swapSafe = [&s](int carry, int there) {
            return there == 0
                   || (!s.clsIsBundle[static_cast<size_t>(carry)] && !s.clsIsBundle[static_cast<size_t>(there)]);
        };
        auto putDownCursor = [this, &s]() {
            if (cursorEmpty()) {
                return;
            }
            for (const Slot& r : s.region) {
                if (isEmptySlot(r)) {
                    leftClick(r);
                    break;
                }
            }
        };
        for (; s.phase == 3 && s.pass < 4 && s.safety > 0; ++s.pass, s.i = 0, s.moved = false) {
            for (; s.i < count && s.safety > 0; ++s.i) {
                if (s.cls[s.i] == s.target[s.i] || s.cls[s.i] == 0) {
                    continue;
                }
                if (!budgetLeft()) {
                    return false;
                }
                if (!cursorEmpty() || isEmptySlot(s.region[s.i])) {
                    putDownCursor();
                    return true;
                }
                leftClick(s.region[s.i]);
                s.moved = true;
                int carry = s.cls[s.i];
                s.cls[s.i] = 0;
                while (carry != 0 && s.safety-- > 0) {
                    size_t dst = count;
                    for (size_t j = 0; j < count; ++j) {
                        if (s.target[j] == carry && s.cls[j] != carry && s.cls[j] != 0 && swapSafe(carry, s.cls[j])) {
                            dst = j;
                            break;
                        }
                    }
                    if (dst == count) {
                        for (size_t j = 0; j < count; ++j) {
                            if (s.target[j] == carry && s.cls[j] == 0) {
                                dst = j;
                                break;
                            }
                        }
                    }
                    if (dst == count) {
                        for (size_t j = 0; j < count; ++j) {
                            if (s.cls[j] == 0 && s.target[j] == 0) {
                                dst = j;
                                break;
                            }
                        }
                        if (dst == count) {
                            for (size_t j = 0; j < count; ++j) {
                                if (s.cls[j] == 0) {
                                    dst = j;
                                    break;
                                }
                            }
                        }
                    }
                    if (dst == count) {
                        break;
                    }
                    leftClick(s.region[dst]);
                    const int prev = s.cls[dst];
                    s.cls[dst] = carry;
                    carry = prev;
                }
                if (carry != 0) {
                    leftClick(s.region[s.i]);
                    s.cls[s.i] = carry;
                }
            }
            if (!s.moved) {
                break;
            }
        }
        if (s.phase == 3) {
            putDownCursor();
            s.phase = 4;
            s.i = 0;
            s.j = count;
        }
        if (s.phase == 4 && cursorEmpty() && !mergeStacks()) {
            return false;
        }
        putDownCursor();

        return true;
    });
}

}
