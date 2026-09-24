#include "modules/ItemScroller.h"

#include "config/Config.h"
#include "core/Logger.h"
#include "core/Paths.h"
#include "core/Strings.h"
#include "game/ItemStackOps.h"
#include "input/Foreground.h"
#include "input/GameInput.h"
#include "input/Keys.h"
#include "input/LowLevelHook.h"

#include <Windows.h>

#include <algorithm>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <sstream>

namespace tsukuyomi {

ItemScroller* ItemScroller::s_hookOwner = nullptr;

namespace {

namespace cui = containerui;

constexpr int kShift = VK_SHIFT;
constexpr int kCtrl = VK_CONTROL;
constexpr int kAlt = VK_MENU;
constexpr int kLmb = VK_LBUTTON;
constexpr int kRmb = VK_RBUTTON;
constexpr int kMmb = VK_MBUTTON;

constexpr wchar_t kDefaultTopPriority[] =
    L"minecraft:diamond_sword,minecraft:diamond_spear,minecraft:diamond_pickaxe,"
    L"minecraft:diamond_axe,minecraft:diamond_shovel,minecraft:diamond_hoe,"
    L"minecraft:netherite_sword,minecraft:netherite_spear,minecraft:netherite_pickaxe,"
    L"minecraft:netherite_axe,minecraft:netherite_shovel,minecraft:netherite_hoe";

constexpr wchar_t kDefaultCategoryOrder[] = L"construction,equipment,items,nature,other";

bool keyDown(int vk)
{
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

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
    set(kMassCraftToggle, L"massCraftToggle", {});
    set(kMoveCraftResults, L"moveCraftResults", {kCtrl, 'M'});
    set(kRecipeView, L"recipeView", {'A'});
    set(kSlotDebug, L"slotDebug", {kCtrl, kAlt, kShift, 'I'});
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
    c.push_back(menu::back());
    c.push_back(enabledItem());
    c.push_back(toggleKeyItem());

    auto tgl = [&c](const wchar_t* title, bool* value) {
        c.push_back(menu::toggle(title, [value] { return *value; }, [value] { *value = !*value; }));
    };
    auto num = [&c](const wchar_t* title, int* value, int lo, int hi) {
        c.push_back(menu::number(
            title, [value] { return static_cast<float>(*value); },
            [value, lo, hi](float v) { *value = std::clamp(static_cast<int>(v + 0.5f), lo, hi); },
            true, static_cast<float>(lo), static_cast<float>(hi)));
    };
    auto txt = [this, &c](const wchar_t* title, std::wstring* value) {
        c.push_back(menu::text(
            title, [value] { return *value; },
            [this, value](std::wstring v) {
                *value = std::move(v);
                rebuildLists();
            }));
    };

    tgl(L"enableScrollingSingle", &m_enableScrollingSingle);
    tgl(L"enableScrollingStacks", &m_enableScrollingStacks);
    tgl(L"enableScrollingMatchingStacks", &m_enableScrollingMatchingStacks);
    tgl(L"enableScrollingEverything", &m_enableScrollingEverything);
    tgl(L"enableScrollingVillager", &m_enableScrollingVillager);
    tgl(L"enableShiftPlaceItems", &m_enableShiftPlaceItems);
    tgl(L"enableShiftDropItems", &m_enableShiftDropItems);
    tgl(L"enableDropkeyDropMatching", &m_enableDropkeyDropMatching);
    tgl(L"enableItemMovingFallback", &m_enableItemMovingFallback);
    tgl(L"enableCraftingFeatures", &m_enableCraftingFeatures);
    tgl(L"enableRightClickCraftingOneStack", &m_enableRightClickCraftingOneStack);
    tgl(L"enableVillagerTradeFeatures", &m_enableVillagerTradeFeatures);

    tgl(L"reverseScrollDirectionSingle", &m_reverseScrollDirectionSingle);
    tgl(L"reverseScrollDirectionStacks", &m_reverseScrollDirectionStacks);
    tgl(L"useSlotPositionAwareScrollDirection", &m_useSlotPositionAwareScrollDirection);
    tgl(L"craftingRenderRecipeItems", &m_craftingRenderRecipeItems);
    tgl(L"craftingRecipesSaveToFile", &m_craftingRecipesSaveToFile);
    num(L"massCraftInterval", &m_massCraftInterval, 1, 60);
    num(L"massCraftIterations", &m_massCraftIterations, 1, 256);
    tgl(L"massCraftUseRecipeBook", &m_massCraftUseRecipeBook);
    tgl(L"massCraftHold", &m_massCraftHold);
    num(L"recipeBookFailureLimit", &m_recipeBookFailureLimit, 0, 512);
    tgl(L"rateLimitClickPackets", &m_rateLimitClickPackets);
    num(L"packetRateLimit", &m_packetRateLimit, 1, 1024);
    tgl(L"villagerTradeUseGlobalFavorites", &m_villagerTradeUseGlobalFavorites);
    tgl(L"villagerTradeSortFavoritesFirst", &m_villagerTradeSortFavoritesFirst);
    tgl(L"villagerTradeUnlockAllTiers", &m_villagerTradeUnlockAllTiers);
    tgl(L"villagerTradeFavoritesOnOpen", &m_villagerTradeFavoritesOnOpen);
    tgl(L"villagerTradeOnOpenThrowResults", &m_villagerTradeOnOpenThrowResults);
    tgl(L"sortInventoryToggle", &m_sortInventoryToggle);
    c.push_back(menu::choice(
        L"sortMethodDefault",
        {L"Category Name", L"Category Count", L"Category Rarity", L"Category RawID", L"Item Name",
         L"Item Count", L"Item Rarity", L"Item Raw ID"},
        [this] { return m_sortMethod; },
        [this](int v) { m_sortMethod = std::clamp(v, 0, static_cast<int>(SortMethod::Count) - 1); }));
    txt(L"sortCategoryOrder", &m_sortCategoryOrder);
    txt(L"sortTopPriorityInventory", &m_sortTopPriority);
    txt(L"sortBottomPriorityInventory", &m_sortBottomPriority);
    tgl(L"sortShulkerBoxesAtEnd", &m_sortShulkerBoxesAtEnd);
    tgl(L"sortShulkerBoxesInverted", &m_sortShulkerBoxesInverted);
    tgl(L"sortBundlesAtEnd", &m_sortBundlesAtEnd);
    tgl(L"sortBundlesInverted", &m_sortBundlesInverted);
    txt(L"guiBlacklist", &m_guiBlacklist);
    txt(L"slotBlacklist", &m_slotBlacklist);
    c.push_back(menu::toggle(L"debugMessages", [this] { return m_debugMessages; }, [this] {
        m_debugMessages = !m_debugMessages;
        if (m_debugMessages) {
            m_logs.store(0);
        }
    }));

    MenuItem item = menu::submenu(name(), std::move(c));
    item.available = [this] { return available(); };
    item.isOn = [this] { return enabled(); };
    return item;
}

void ItemScroller::loadConfig(const nlohmann::json& section)
{
    Module::loadConfig(section);
    auto b = [&section](const char* key, bool& v) { v = Config::getBool(section, key, v); };
    auto n = [&section](const char* key, int& v, int lo, int hi) {
        v = std::clamp(Config::getInt(section, key, v), lo, hi);
    };
    auto s = [&section](const char* key, std::wstring& v, const wchar_t* def) {
        v = def;
        if (const auto it = section.find(key); it != section.end() && it->is_string()) {
            v = toUtf16(it->get<std::string>());
        }
    };
    b("enableCraftingFeatures", m_enableCraftingFeatures);
    b("enableDropkeyDropMatching", m_enableDropkeyDropMatching);
    b("enableItemMovingFallback", m_enableItemMovingFallback);
    b("enableRightClickCraftingOneStack", m_enableRightClickCraftingOneStack);
    b("enableScrollingEverything", m_enableScrollingEverything);
    b("enableScrollingMatchingStacks", m_enableScrollingMatchingStacks);
    b("enableScrollingSingle", m_enableScrollingSingle);
    b("enableScrollingStacks", m_enableScrollingStacks);
    b("enableScrollingVillager", m_enableScrollingVillager);
    b("enableShiftDropItems", m_enableShiftDropItems);
    b("enableShiftPlaceItems", m_enableShiftPlaceItems);
    b("enableVillagerTradeFeatures", m_enableVillagerTradeFeatures);
    n("massCraftInterval", m_massCraftInterval, 1, 60);
    n("massCraftIterations", m_massCraftIterations, 1, 256);
    b("massCraftSwapsOnly", m_massCraftSwapsOnly);
    b("massCraftUseRecipeBook", m_massCraftUseRecipeBook);
    b("massCraftHold", m_massCraftHold);
    n("packetRateLimit", m_packetRateLimit, 1, 1024);
    b("rateLimitClickPackets", m_rateLimitClickPackets);
    n("recipeBookFailureLimit", m_recipeBookFailureLimit, 0, 512);
    b("craftingRecipesSaveToFile", m_craftingRecipesSaveToFile);
    b("craftingRecipesSaveFileIsGlobal", m_craftingRecipesSaveFileIsGlobal);
    b("craftingRenderRecipeItems", m_craftingRenderRecipeItems);
    b("debugMessages", m_debugMessages);
    b("reverseScrollDirectionSingle", m_reverseScrollDirectionSingle);
    b("reverseScrollDirectionStacks", m_reverseScrollDirectionStacks);
    b("useSlotPositionAwareScrollDirection", m_useSlotPositionAwareScrollDirection);
    b("villagerTradeUseGlobalFavorites", m_villagerTradeUseGlobalFavorites);
    b("villagerTradeListRememberScrollPosition", m_villagerTradeListRememberScrollPosition);
    b("villagerTradeSortFavoritesFirst", m_villagerTradeSortFavoritesFirst);
    b("villagerTradeUnlockAllTiers", m_villagerTradeUnlockAllTiers);
    b("villagerTradeFavoritesOnOpen", m_villagerTradeFavoritesOnOpen);
    b("villagerTradeOnOpenThrowResults", m_villagerTradeOnOpenThrowResults);
    b("sortInventoryToggle", m_sortInventoryToggle);
    b("sortAssumeEmptyBoxStacks", m_sortAssumeEmptyBoxStacks);
    b("sortShulkerBoxesAtEnd", m_sortShulkerBoxesAtEnd);
    b("sortShulkerBoxesInverted", m_sortShulkerBoxesInverted);
    b("sortBundlesAtEnd", m_sortBundlesAtEnd);
    b("sortBundlesInverted", m_sortBundlesInverted);
    n("sortMethodDefault", m_sortMethod, 0, static_cast<int>(SortMethod::Count) - 1);
    s("sortTopPriorityInventory", m_sortTopPriority, kDefaultTopPriority);
    s("sortBottomPriorityInventory", m_sortBottomPriority, L"");
    s("sortCategoryOrder", m_sortCategoryOrder, kDefaultCategoryOrder);
    s("guiBlacklist", m_guiBlacklist, L"");
    s("slotBlacklist", m_slotBlacklist, L"");
    {
        std::array<std::vector<int>, kHotkeyCount> combos;
        std::array<std::string, kHotkeyCount> text;
        readHotkeys(section, combos, text, true);
        for (size_t i = 0; i < m_keys.size(); ++i) {
            m_keys[i].keys = combos[i];
        }
        std::lock_guard<std::mutex> lock(m_keysMutex);
        m_keyText = text;
    }
    rebuildLists();
}

bool ItemScroller::readHotkeys(const nlohmann::json& section, std::array<std::vector<int>, kHotkeyCount>& out,
                               std::array<std::string, kHotkeyCount>& text, bool warn) const
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
                    } else if (warn) {
                        log().warn(L"ItemScroller: hotkeys.{}: cannot read \"{}\" (using {})", m_keys[i].name,
                                   toUtf16(k->get<std::string>()), keys::comboName(combo));
                    }
                } else if (k->is_array()) {
                    combo.clear();
                    for (const auto& v : *k) {
                        if (v.is_number_integer()) {
                            combo.push_back(v.get<int>());
                        }
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
    if (!readHotkeys(*sec, combos, text, true)) {
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
    if (!m_keysPending.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_keysMutex);
    for (size_t i = 0; i < m_keys.size(); ++i) {
        if (m_keys[i].keys != m_pendingKeys[i]) {
            m_keys[i].keys = m_pendingKeys[i];
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
    constexpr unsigned long long kWindowMs = 3000;
    const unsigned long long at = m_rightDownMs.load(std::memory_order_acquire);
    return m_rightDownSneak.load(std::memory_order_acquire) && at != 0 && GetTickCount64() - at < kWindowMs;
}

void ItemScroller::saveConfig(nlohmann::json& section) const
{
    Module::saveConfig(section);
    section["enableCraftingFeatures"] = m_enableCraftingFeatures;
    section["enableDropkeyDropMatching"] = m_enableDropkeyDropMatching;
    section["enableItemMovingFallback"] = m_enableItemMovingFallback;
    section["enableRightClickCraftingOneStack"] = m_enableRightClickCraftingOneStack;
    section["enableScrollingEverything"] = m_enableScrollingEverything;
    section["enableScrollingMatchingStacks"] = m_enableScrollingMatchingStacks;
    section["enableScrollingSingle"] = m_enableScrollingSingle;
    section["enableScrollingStacks"] = m_enableScrollingStacks;
    section["enableScrollingVillager"] = m_enableScrollingVillager;
    section["enableShiftDropItems"] = m_enableShiftDropItems;
    section["enableShiftPlaceItems"] = m_enableShiftPlaceItems;
    section["enableVillagerTradeFeatures"] = m_enableVillagerTradeFeatures;
    section["massCraftInterval"] = m_massCraftInterval;
    section["massCraftIterations"] = m_massCraftIterations;
    section["massCraftSwapsOnly"] = m_massCraftSwapsOnly;
    section["massCraftUseRecipeBook"] = m_massCraftUseRecipeBook;
    section["massCraftHold"] = m_massCraftHold;
    section["packetRateLimit"] = m_packetRateLimit;
    section["rateLimitClickPackets"] = m_rateLimitClickPackets;
    section["recipeBookFailureLimit"] = m_recipeBookFailureLimit;
    section["craftingRecipesSaveToFile"] = m_craftingRecipesSaveToFile;
    section["craftingRecipesSaveFileIsGlobal"] = m_craftingRecipesSaveFileIsGlobal;
    section["craftingRenderRecipeItems"] = m_craftingRenderRecipeItems;
    section["debugMessages"] = m_debugMessages;
    section["reverseScrollDirectionSingle"] = m_reverseScrollDirectionSingle;
    section["reverseScrollDirectionStacks"] = m_reverseScrollDirectionStacks;
    section["useSlotPositionAwareScrollDirection"] = m_useSlotPositionAwareScrollDirection;
    section["villagerTradeUseGlobalFavorites"] = m_villagerTradeUseGlobalFavorites;
    section["villagerTradeListRememberScrollPosition"] = m_villagerTradeListRememberScrollPosition;
    section["villagerTradeSortFavoritesFirst"] = m_villagerTradeSortFavoritesFirst;
    section["villagerTradeUnlockAllTiers"] = m_villagerTradeUnlockAllTiers;
    section["villagerTradeFavoritesOnOpen"] = m_villagerTradeFavoritesOnOpen;
    section["villagerTradeOnOpenThrowResults"] = m_villagerTradeOnOpenThrowResults;
    section["sortInventoryToggle"] = m_sortInventoryToggle;
    section["sortAssumeEmptyBoxStacks"] = m_sortAssumeEmptyBoxStacks;
    section["sortShulkerBoxesAtEnd"] = m_sortShulkerBoxesAtEnd;
    section["sortShulkerBoxesInverted"] = m_sortShulkerBoxesInverted;
    section["sortBundlesAtEnd"] = m_sortBundlesAtEnd;
    section["sortBundlesInverted"] = m_sortBundlesInverted;
    section["sortMethodDefault"] = m_sortMethod;
    section["sortTopPriorityInventory"] = toUtf8(m_sortTopPriority);
    section["sortBottomPriorityInventory"] = toUtf8(m_sortBottomPriority);
    section["sortCategoryOrder"] = toUtf8(m_sortCategoryOrder);
    section["guiBlacklist"] = toUtf8(m_guiBlacklist);
    section["slotBlacklist"] = toUtf8(m_slotBlacklist);
    nlohmann::json hk = nlohmann::json::object();
    {
        std::lock_guard<std::mutex> lock(m_keysMutex);
        for (size_t i = 0; i < m_keys.size(); ++i) {
            hk[toUtf8(m_keys[i].name)] = m_keyText[i];
        }
    }
    section["hotkeys"] = std::move(hk);
    if (m_recipesDirty) {
        saveRecipes();
    }
}

void ItemScroller::rebuildLists()
{
    m_topPriority.clear();
    m_bottomPriority.clear();
    m_categoryOrder.clear();
    for (const std::string& s : splitList(m_sortTopPriority)) {
        m_topPriority.push_back(normalizeItemName(s));
    }
    for (const std::string& s : splitList(m_sortBottomPriority)) {
        m_bottomPriority.push_back(normalizeItemName(s));
    }
    for (std::string s : splitList(m_sortCategoryOrder)) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        m_categoryOrder.push_back(s);
    }
}

void ItemScroller::onScansReady()
{
    cui::onScansReady();
    ItemStackOps::instance().onScansReady();
    cui::setListener(this);
    if (cui::available()) {
        registerUiDefinitions();
    }
    tradeui::onScansReady();
    tradeui::setListener(this);
    if (!available() && enabled()) {
        log().warn(L"ItemScroller: the container screen could not be located; the module cannot work");
    }
}

void ItemScroller::shutdown()
{
    cui::setListener(nullptr);
    tradeui::setListener(nullptr);
    saveFavorites();
    removeMouseHook();
    if (m_recipesDirty) {
        saveRecipes();
    }
}

void ItemScroller::onEnabledChanged(bool on)
{
    if (!on) {
        stopDrag();
        m_jobs.clear();
        removeMouseHook();
        tradeui::setUnlockTier(-1);
        tradeui::setFavoriteTier({}, {});
        m_unlockSent = -2;
        m_favTierSet = false;
    }
}

void ItemScroller::onUpdate()
{
    watchConfigFile();
    if (enabled() && available()) {
        installMouseHook();
    } else {
        removeMouseHook();
    }
}

void ItemScroller::installMouseHook()
{
    if (m_mouseHook != nullptr || m_mouseHookFailed) {
        return;
    }
    s_hookOwner = this;
    const input::LowLevelHook hook = input::installLowLevelHook(WH_MOUSE_LL, &ItemScroller::mouseHookProc);
    m_mouseHook = hook.hook;
    if (m_mouseHook == nullptr) {
        s_hookOwner = nullptr;
        m_mouseHookFailed = true;
        log().warn(L"ItemScroller: could not grab the mouse wheel (error {}, {} with the module); "
                   L"wheel features are off",
                   hook.errorWithoutModule, hook.errorWithModule);
    }
}

void ItemScroller::removeMouseHook()
{
    if (m_mouseHook != nullptr) {
        UnhookWindowsHookEx(m_mouseHook);
        m_mouseHook = nullptr;
    }
    s_hookOwner = nullptr;
    m_mouseHookFailed = false;
}

LRESULT CALLBACK ItemScroller::mouseHookProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && s_hookOwner != nullptr) {
        int button = -1;
        bool down = false;
        switch (wParam) {
        case WM_LBUTTONDOWN: button = 0; down = true; break;
        case WM_LBUTTONUP: button = 0; break;
        case WM_RBUTTONDOWN: button = 1; down = true; break;
        case WM_RBUTTONUP: button = 1; break;
        case WM_MBUTTONDOWN: button = 2; down = true; break;
        case WM_MBUTTONUP: button = 2; break;
        default: break;
        }
        if (button >= 0) {
            s_hookOwner->m_mouseDown[button].store(down, std::memory_order_release);
            if (down) {
                s_hookOwner->m_mouseDownMs[button].store(GetTickCount64(), std::memory_order_release);
            }
        }
        if (button == 1 && down) {
            s_hookOwner->m_rightDownSneak.store(input::sneakHeldWithin(input::kSneakHoldGraceMs),
                                                std::memory_order_release);
            s_hookOwner->m_rightDownMs.store(GetTickCount64(), std::memory_order_release);
        }
    }
    if (code == HC_ACTION && wParam == WM_MOUSEWHEEL && s_hookOwner != nullptr) {
        const auto* info = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        const int delta = GET_WHEEL_DELTA_WPARAM(info->mouseData);
        if (delta != 0) {
            s_hookOwner->m_wheel.fetch_add(delta > 0 ? 1 : -1, std::memory_order_acq_rel);
            s_hookOwner->m_wheelSeen.fetch_add(1, std::memory_order_relaxed);
            s_hookOwner->m_wheelLastMs.store(GetTickCount64(), std::memory_order_release);
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

namespace {
int mouseIndex(int vk)
{
    return vk == kLmb ? 0 : vk == kRmb ? 1 : vk == kMmb ? 2 : -1;
}
}

bool ItemScroller::mouseHeld(int vk) const
{
    const int i = mouseIndex(vk);
    if (i < 0) {
        return keyDown(vk);
    }
    return (m_mouseHook != nullptr) ? m_mouseDown[i].load(std::memory_order_acquire) : keyDown(vk);
}

int ItemScroller::recentMouseButton() const
{
    const unsigned long long l = m_mouseDownMs[0].load(std::memory_order_acquire);
    const unsigned long long r = m_mouseDownMs[1].load(std::memory_order_acquire);
    if (m_mouseHook == nullptr) {
        return (keyDown(kRmb) && !keyDown(kLmb)) ? kRmb : kLmb;
    }
    return (r > l) ? kRmb : kLmb;
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
        if (pressVk != 0 && mouseIndex(vk) >= 0) {
            return false;
        }
        return mouseHeld(vk);
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

bool ItemScroller::comboEdge(const std::vector<int>& combo, bool& wasDown, bool exactModifiers)
{
    const bool down = comboHeld(combo, exactModifiers);
    const bool edge = down && !wasDown;
    if (down) {
        wasDown = true;
        return edge;
    }
    bool anyTrigger = false;
    bool triggerUp = false;
    for (int vk : combo) {
        if (keys::isModifier(vk)) {
            continue;
        }
        anyTrigger = true;
        if (!mouseHeld(vk)) {
            triggerUp = true;
        }
    }
    if (anyTrigger ? triggerUp
                   : !std::any_of(combo.begin(), combo.end(), [](int vk) { return keyDown(vk); })) {
        wasDown = false;
    }
    return edge;
}

bool ItemScroller::screenBlacklisted() const
{
    for (const std::string& name : splitList(m_guiBlacklist)) {
        if (cui::collectionSize(name) > 0) {
            return true;
        }
    }
    return false;
}

bool ItemScroller::slotBlacklisted(const Slot& s) const
{
    for (const std::string& name : splitList(m_slotBlacklist)) {
        if (name == s.coll) {
            return true;
        }
    }
    return false;
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

bool ItemScroller::screenHas(Group group) const
{
    switch (group) {
    case Group::Player:
        return cui::collectionSize("inventory_items") > 0 || cui::collectionSize("hotbar_items") > 0
               || cui::collectionSize("combined_hotbar_and_inventory_items") > 0;
    case Group::Storage:
        return cui::collectionSize("container_items") > 0;
    case Group::Grid:
        return cui::collectionSize("crafting_input_items") > 0;
    case Group::Output:
        return cui::collectionSize("crafting_output_items") > 0;
    case Group::TradeIn:
        return cui::collectionSize("trade2_ingredient1_item") > 0;
    case Group::TradeOut:
        return cui::collectionSize("trade2_result_item") > 0;
    default:
        return false;
    }
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
    case Group::Output:
        add("crafting_output_items");
        break;
    case Group::TradeIn:
        add("trade2_ingredient1_item");
        add("trade2_ingredient2_item");
        break;
    case Group::TradeOut:
        add("trade2_result_item");
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

ItemScroller::Group ItemScroller::otherGroupOf(const Slot& slot) const
{
    const Group g = groupOf(slot.coll);
    if (g == Group::Player) {
        return screenHas(Group::Storage) ? Group::Storage : Group::Player;
    }
    return Group::Player;
}

std::vector<ItemScroller::Slot> ItemScroller::otherSlotsOf(const Slot& slot) const
{
    const bool split = !screenHas(Group::Storage);
    const int mine = invId(slot.coll, split);
    std::vector<Slot> all;
    for (Group g : {Group::Storage, Group::Player}) {
        for (const Slot& s : slotsOf(g)) {
            if (invId(s.coll, split) != mine && !slotBlacklisted(s)) {
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

int ItemScroller::clickCap() const
{
    return m_rateLimitClickPackets ? std::max(1, m_packetRateLimit) : kSafeClicksPerTick;
}

bool ItemScroller::budgetLeft() const
{
    return m_clicksThisTick < clickCap();
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
    if (debugLog()) {
        static const wchar_t* kNames[] = {L"L", L"R", L"Shift", L"Q", L"CtrlQ", L"Double"};
        const void* st = stackOf(s);
        const void* cur = cursor();
        log().info(L"ItemScroller: click {} \"{}\"[{}] {} -> slot {}x{} / cursor {}x{}",
                   kind < 0 ? L"drop-cursor" : kNames[kind], toUtf16(s.coll), s.index,
                   ok ? L"ok" : L"FAILED", toUtf16(cui::itemName(st)), cui::countOf(st),
                   toUtf16(cui::itemName(cur)), cui::countOf(cur));
    }
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

void ItemScroller::clickSlotsToMoveItemsFromSlot(const Slot& from, bool toOther)
{
    if (!cursorEmpty() || isEmptySlot(from)) {
        return;
    }
    std::vector<Slot> targets;
    if (toOther) {
        targets = otherSlotsOf(from);
    } else {
        const bool split = !screenHas(Group::Storage);
        for (Group g : {Group::Storage, Group::Player}) {
            for (const Slot& s : slotsOf(g)) {
                if (invId(s.coll, split) == invId(from.coll, split) && !(s == from)) {
                    targets.push_back(s);
                }
            }
        }
    }
    moveItemsToTargets(from, targets, false);
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
        const bool split = !screenHas(Group::Storage);
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
            if (s == slot || slotBlacklisted(s) || (invId(s.coll, split) == mine) != toOther) {
                continue;
            }
            out.push_back(s);
        }
        if (toOther) {
            out.push_back(slot);
        }
        return out;
    };
    auto unit = [this, ref, slot, matchingOnly, toOther, firstOnly](const Slot& s) {
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
        if (!shiftClickWithCheck(s) && m_enableItemMovingFallback) {
            clickSlotsToMoveItemsFromSlot(s, toOther);
        }
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

void ItemScroller::enqueueDropStacks(std::shared_ptr<StackRef> ref, const Slot& refSlot, bool sameInventory)
{
    if (ref == nullptr || ref->get() == nullptr) {
        return;
    }
    auto collect = [this, refSlot, sameInventory]() {
        const bool split = false;
        const int mine = invId(refSlot.coll, split);
        std::vector<Slot> out;
        for (Group g : {Group::Storage, Group::Player, Group::Grid, Group::TradeIn}) {
            for (const Slot& s : slotsOf(g)) {
                if ((invId(s.coll, split) == mine) == sameInventory && !slotBlacklisted(s)) {
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
    enqueueDropStacks(ref, m_cursorSource, true);
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

bool ItemScroller::slotY(const Slot& s, int& y) const
{
    int x = 0;
    return cui::slotScreenPos(s.coll, s.index, x, y);
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
            if (s == slot || slotBlacklisted(s)) {
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
    if (debugLog()) {
        log().info(L"ItemScroller: vertical {} from \"{}\"[{}] row {} -> {} candidates",
                   up ? L"up" : L"down", toUtf16(slot.coll), slot.index, myRow, cands.size());
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
                    if (s == src || slotBlacklisted(s) || r == srcRow || (r < srcRow) != up) {
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
    if (!cursorEmpty() || slotBlacklisted(slot)) {
        return false;
    }
    const bool everything = comboHeld(m_keys[kModifierMoveEverything].keys, true);
    const bool matching = comboHeld(m_keys[kModifierMoveMatching].keys, true);
    const bool stacks = comboHeld(m_keys[kModifierMoveStack].keys, true);
    const bool nonSingle = everything || matching || stacks;
    bool toOther = scrollingUp;
    if (m_useSlotPositionAwareScrollDirection) {
        bool above = false;
        const int myRow = rowOf(slot);
        const bool split = !screenHas(Group::Storage);
        for (Group g : {Group::Storage, Group::Player}) {
            for (const Slot& s : slotsOf(g)) {
                if (invId(s.coll, split) != invId(slot.coll, split) && rowOf(s) < myRow) {
                    above = true;
                    break;
                }
            }
        }
        toOther = (above == scrollingUp);
    }
    if ((m_reverseScrollDirectionSingle && !nonSingle) || (m_reverseScrollDirectionStacks && nonSingle)) {
        toOther = !toOther;
    }
    const Group g = groupOf(slot.coll);
    if (m_enableCraftingFeatures && g == Group::Output && isCraftingScreen()) {
        return tryMoveItemsCrafting(slot, toOther, stacks, everything);
    }
    if (m_enableScrollingVillager && g == Group::TradeOut) {
        return tryMoveItemsVillager(slot, toOther, stacks);
    }
    if (isEmptySlot(slot)) {
        return false;
    }
    if ((!m_enableScrollingSingle && !nonSingle) || (!m_enableScrollingStacks && stacks)
        || (!m_enableScrollingMatchingStacks && matching)
        || (!m_enableScrollingEverything && everything)) {
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
    if (debugLog()) {
        log().info(L"ItemScroller: drag action {} on \"{}\"[{}]", static_cast<int>(action),
                   toUtf16(slot.coll), slot.index);
    }
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
    if (m_drag == DragAction::None || !slot.valid() || slotBlacklisted(slot)) {
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
        dumpScreen(L"after drag");
    }
    m_drag = DragAction::None;
    m_dragMouseVk = 0;
    m_dragLast = Slot{};
    m_dragged.clear();
}

bool ItemScroller::onSlotButton(std::uint32_t id, int state, const std::string& coll, int index)
{
    logStats();
    if (!enabled() || screenBlacklisted()) {
        return false;
    }
    if (m_swallowId != 0 && id == m_swallowId && state != cui::kStatePressed) {
        if (state == cui::kStateReleased) {
            m_swallowId = 0;
        }
        return true;
    }
    const Slot slot{coll, index};
    m_inTick = true;
    if (id != cui::button::kHover && debugLog()) {
        log().info(L"ItemScroller: button {:#x} state {} on \"{}\"[{}] keys{}{}{}{}{}{}{}{}",
                   id, state, toUtf16(coll), index, keyDown(kShift) ? L" Shift" : L"",
                   keyDown(kCtrl) ? L" Ctrl" : L"", keyDown(kAlt) ? L" Alt" : L"",
                   mouseHeld(kLmb) ? L" L" : L"", mouseHeld(kRmb) ? L" R" : L"",
                   keyDown('Q') ? L" Q" : L"", keyDown('W') ? L" W" : L"", keyDown('S') ? L" S" : L"");
    }

    if (id == cui::button::kHover) {
        if (m_drag != DragAction::None) {
            if (dragActionHeld(m_drag)) {
                dragOver(slot, false);
            } else {
                stopDrag();
            }
        }
        m_inTick = false;
        return false;
    }
    if (state != cui::kStatePressed) {
        m_inTick = false;
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
        if (keyDown(kShift) && !cursorEmpty()) {
            swallow = m_enableShiftDropItems ? shiftDropItems() : true;
        }
    } else if (slotButton && slot.valid() && !slotBlacklisted(slot)) {
        const int mouseVk = isRight ? kRmb : recentMouseButton();
        if (cursorEmpty() && (id == cui::button::kTakeAllPlaceAll || isRight)
            && !isOutputSlot(slot)) {
            m_cursorSource = slot;
        }
        if (isRight && m_enableRightClickCraftingOneStack && isOutputSlot(slot) && isCraftingScreen()) {
            rightClickCraftOneStack(slot);
            swallow = true;
        } else if (isLeftish && comboHeldAt(m_keys[kKeyMoveEverything].keys, true, mouseVk)) {
            if (cursorEmpty() && !isEmptySlot(slot)) {
                enqueueMoveStacksFrom(slot, false, true, false);
            }
            swallow = true;
        } else if (isAutoPlace && m_enableShiftPlaceItems && !cursorEmpty()
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
        dumpScreen(L"after click");
    }
    m_inTick = false;
    return swallow;
}

void ItemScroller::onScreenLost()
{
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
    m_loggedVillager.clear();
    m_loggedSelTier = -2;
    m_loggedSelIndex = -2;
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
    m_wheel.store(0);
    if (m_recipesDirty) {
        saveRecipes();
    }
}

void ItemScroller::consumeWheel()
{
    int notches = m_wheel.exchange(0, std::memory_order_acq_rel);
    if (notches == 0) {
        return;
    }
    if (m_recipeViewOpen) {
        changeRecipeSelection(m_selectedRecipe + (notches < 0 ? 1 : -1) * std::abs(notches));
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
    if (debugLog()) {
        log().info(L"ItemScroller: wheel {} on \"{}\"[{}]", notches, toUtf16(coll), index);
        dumpScreen(L"after wheel");
    }
}

void ItemScroller::debugSlot(const Slot& s) const
{
    const void* st = stackOf(s);
    log().info(L"ItemScroller: slot \"{}\"[{}] item {} x{} (max {}) aux {} / screen vtable {:#x} / "
               L"collections: hotbar {} inventory {} container {} grid {} output {} trade {}",
               toUtf16(s.coll), s.index, toUtf16(cui::itemName(st)), cui::countOf(st),
               cui::maxStackOf(st), cui::auxOf(st),
               cui::screenKind() - reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)),
               cui::collectionSize("hotbar_items"), cui::collectionSize("inventory_items"),
               cui::collectionSize("container_items"), cui::collectionSize("crafting_input_items"),
               cui::collectionSize("crafting_output_items"),
               cui::collectionSize("trade2_result_item"));
}

void ItemScroller::dumpScreen(const wchar_t* why) const
{
    if (!debugLog()) {
        return;
    }
    auto shortName = [](std::string n) {
        const size_t c = n.find(':');
        return c == std::string::npos ? n : n.substr(c + 1);
    };
    std::wstring text = std::format(L"ItemScroller [{}]", why);
    for (const char* coll : {"container_items", "inventory_items", "hotbar_items",
                             "crafting_input_items", "crafting_output_items", "cursor_items",
                             "trade2_ingredient1_item", "trade2_ingredient2_item",
                             "trade2_result_item"}) {
        const int n = cui::collectionSize(coll);
        if (n <= 0) {
            continue;
        }
        text += std::format(L" | {}:", toUtf16(coll));
        for (int i = 0; i < n; ++i) {
            const void* st = cui::stackAt(coll, i);
            if (cui::isEmpty(st)) {
                text += L" .";
            } else {
                text += std::format(L" {}x{}", toUtf16(shortName(cui::itemName(st))), cui::countOf(st));
            }
        }
    }
    log().info(L"{}", text);
}

void ItemScroller::pollHotkeys()
{
    std::string coll;
    int index = -1;
    const bool over = cui::hovered(coll, index);
    const Slot hovered{coll, index};

    if (!toggleKey().empty()) {
        static bool was = false;
        if (comboEdge(toggleKey().combo(), was, true)) {
            toggle();
            log().info(L"ItemScroller: toggled {}", enabled() ? L"ON" : L"OFF");
            return;
        }
    }
    auto edge = [this](KeyId id) { return comboEdge(m_keys[id].keys, m_keys[id].wasDown, true); };

    if (edge(kSlotDebug)) {
        dumpScreen(L"slot debug");
        if (over) {
            debugSlot(hovered);
        } else {
            log().info(L"ItemScroller: no slot under the cursor (screen vtable {:#x})",
                       cui::screenKind() - reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)));
        }
    }
    if (edge(kDropAllMatching) && m_enableDropkeyDropMatching && over && !isEmptySlot(hovered)) {
        enqueueDropStacks(std::make_shared<StackRef>(stackOf(hovered)), hovered, true);
    }
    if (edge(kSortInventory) && m_sortInventoryToggle && over) {
        sortInventory(hovered);
    }
    if (m_enableCraftingFeatures && isCraftingScreen()) {
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
    if (edge(kMassCraftToggle)) {
        m_massCraftHold = !m_massCraftHold;
        log().info(L"ItemScroller: massCraftHold {}", m_massCraftHold ? L"ON" : L"OFF");
    }
    if (edge(kVillagerTradeFavorites) && m_enableVillagerTradeFeatures && isTradeScreen()) {
        tradeFavorites(true);
    }
}

void ItemScroller::logStats()
{
    if (m_debugMessages) {
        const unsigned long long now = GetTickCount64();
        if (now - m_lastStatsMs >= 3000 && debugLog()) {
            m_lastStatsMs = now;
            const cui::Stats st = cui::stats();
            std::string coll;
            int index = -1;
            const bool over = cui::hovered(coll, index);
            log().info(L"ItemScroller: stats sm {} hover {} ticks {} ({} calls) wheel {} hook {} "
                       L"hovered {} \"{}\"[{}] pitch {}",
                       st.smEvents, st.hoverEvents, st.ticks, st.tickCalls,
                       m_wheelSeen.load(), m_mouseHook != nullptr, over, toUtf16(coll), index,
                       cui::slotPitchPixels());
        }
    }
}

void ItemScroller::onScreenTick()
{
    logStats();
    if (!m_wheelPrimed) {
        const int dropped = m_wheel.exchange(0, std::memory_order_acq_rel);
        m_wheelPrimed = true;
        m_wheelCarry = GetTickCount64() - m_wheelLastMs.load(std::memory_order_acquire) < kWheelCarryGapMs;
        if ((dropped != 0 || m_wheelCarry) && debugLog()) {
            log().info(L"ItemScroller: dropped {} wheel notch(es) from before the screen opened{}", dropped,
                       m_wheelCarry ? L" (still turning; muted until it stops)" : L"");
        }
    } else if (m_wheelCarry) {
        m_wheel.store(0, std::memory_order_release);
        if (GetTickCount64() - m_wheelLastMs.load(std::memory_order_acquire) >= kWheelCarryGapMs) {
            m_wheelCarry = false;
        }
    }
    if (m_autoTradeOffstack) {
        m_wheel.store(0);
        if (!enabled()) {
            return;
        }
        m_inTick = true;
        m_clicksThisTick = 0;
        runJobs();
        tradeScreenTick(true);
        runJobs();
        m_inTick = false;
        return;
    }
    if (!enabled() || !input::isGameForeground() || screenBlacklisted()) {
        m_wheel.store(0);
        m_recipeViewOpen = false;
        updateRecipeView();
        return;
    }
    m_inTick = true;
    m_clicksThisTick = 0;
    applyPendingKeys();
    if (m_drag != DragAction::None && (!mouseHeld(m_dragMouseVk) || !dragActionHeld(m_drag))) {
        stopDrag();
    }
    runJobs();
    m_recipeViewOpen = m_enableCraftingFeatures && isCraftingScreen()
                       && comboHeld(m_keys[kRecipeView].keys, false);
    if (m_recipeViewOpen) {
        onRecipeViewKeys();
    }
    handleViewInput();
    consumeWheel();
    pollHotkeys();
    if (m_enableCraftingFeatures && isCraftingScreen() && m_jobs.empty()) {
        massCraftTick();
    }
    tradeScreenTick(false);
    updateRecipeView();
    runJobs();
    m_inTick = false;
}

namespace {

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
    const int bottom = find(m_bottomPriority);
    if (bottom >= 0) {
        return static_cast<int>(m_bottomPriority.size()) + bottom;
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
    const auto method = static_cast<SortMethod>(m_sortMethod);
    if (method == SortMethod::CategoryRarity || method == SortMethod::ItemRarity) {
        x.rarity = cui::rarityOf(stack);
    }
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
    const bool ea = cui::isEmpty(a);
    const bool eb = cui::isEmpty(b);
    const std::string na = ea ? std::string() : cui::itemName(a);
    const std::string nb = eb ? std::string() : cui::itemName(b);
    const bool boxA = isShulkerName(na);
    const bool boxB = isShulkerName(nb);
    if (m_sortShulkerBoxesAtEnd && boxA != boxB) {
        return boxA ? 1 : -1;
    }
    const bool bunA = isBundleName(na);
    const bool bunB = isBundleName(nb);
    if (m_sortBundlesAtEnd && bunA != bunB) {
        return bunA ? 1 : -1;
    }
    const int pa = customPriority(a);
    const int pb = customPriority(b);
    if (pa != -1 || pb != -1) {
        return (pa < pb) ? -1 : (pa > pb ? 1 : 0);
    }
    if (ea != eb) {
        return ea ? 1 : -1;
    }
    if (ea) {
        return 0;
    }
    if (boxA && boxB) {
        const int f = m_sortShulkerBoxesInverted ? -1 : 1;
        return f * ((xa.boxSlots < xb.boxSlots) ? -1 : (xa.boxSlots > xb.boxSlots ? 1 : 0));
    }
    if (bunA && bunB) {
        const int f = m_sortBundlesInverted ? -1 : 1;
        return f * ((xa.bundleFill < xb.bundleFill) ? -1 : (xa.bundleFill > xb.bundleFill ? 1 : 0));
    }
    const auto method = static_cast<SortMethod>(m_sortMethod);
    const bool byCategory = method == SortMethod::CategoryName || method == SortMethod::CategoryCount
                            || method == SortMethod::CategoryRarity
                            || method == SortMethod::CategoryRawId;
    if (byCategory) {
        const int ca = categoryIndex(a);
        const int cb = categoryIndex(b);
        if (ca != cb) {
            return ca < cb ? -1 : 1;
        }
    }
    if (na != nb) {
        switch (method) {
        case SortMethod::CategoryCount:
        case SortMethod::ItemCount: {
            const int c1 = cui::countOf(a), c2 = cui::countOf(b);
            if (c1 != c2) {
                return c1 > c2 ? -1 : 1;
            }
            return na < nb ? -1 : 1;
        }
        case SortMethod::CategoryRarity:
        case SortMethod::ItemRarity:
            if (xa.rarity >= 0 && xb.rarity >= 0 && xa.rarity != xb.rarity) {
                return xa.rarity < xb.rarity ? -1 : 1;
            }
            return na < nb ? -1 : 1;
        default:
            return na < nb ? -1 : 1;
        }
    }
    const int ax = cui::auxOf(a), bx = cui::auxOf(b);
    if (ax != bx) {
        return ax < bx ? -1 : 1;
    }
    const int c1 = cui::countOf(a), c2 = cui::countOf(b);
    return (c1 > c2) ? -1 : (c1 < c2 ? 1 : 0);
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
        if (s.phase == 1) {
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
            s.phase = 2;
        }
        if (s.phase == 2) {
            std::vector<StackCopy> snap(count);
            std::vector<SortExtra> extra(count);
            std::vector<int> order(count);
            for (size_t i = 0; i < count; ++i) {
                const void* live = stackOf(s.region[i]);
                snap[i].copyFrom(live);
                extra[i] = sortExtraOf(live);
                order[i] = static_cast<int>(i);
            }
            std::stable_sort(order.begin(), order.end(), [this, &snap, &extra](int x, int y) {
                const auto ux = static_cast<size_t>(x);
                const auto uy = static_cast<size_t>(y);
                return compareStacks(snap[ux].get(), snap[uy].get(), extra[ux], extra[uy]) < 0;
            });
            s.cls.assign(count, 0);
            s.clsIsBundle.assign(1, false);
            std::vector<int> reps;
            for (size_t i = 0; i < count; ++i) {
                const void* item = snap[i].get();
                if (cui::isEmpty(item)) {
                    continue;
                }
                int found = 0;
                if (cui::maxStackOf(item) > 1) {
                    for (size_t r = 0; r < reps.size(); ++r) {
                        const void* rep = snap[static_cast<size_t>(reps[r])].get();
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
        for (; s.pass < 4 && s.safety > 0; ++s.pass, s.i = 0, s.moved = false) {
            for (; s.i < count && s.safety > 0; ++s.i) {
                if (s.cls[s.i] == s.target[s.i] || s.cls[s.i] == 0) {
                    continue;
                }
                if (!budgetLeft()) {
                    return false;
                }
                if (!cursorEmpty() || isEmptySlot(s.region[s.i])) {
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
        if (!cursorEmpty()) {
            for (const Slot& r : s.region) {
                if (isEmptySlot(r)) {
                    leftClick(r);
                    break;
                }
            }
        }
        if (debugLog()) {
            log().info(L"ItemScroller: sorted {} slots of \"{}\"", count, toUtf16(s.region.front().coll));
            dumpScreen(L"after sort");
        }
        return true;
    });
}

}
