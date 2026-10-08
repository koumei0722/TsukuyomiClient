#pragma once

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "game/ContainerUi.h"
#include "game/TradeUi.h"
#include "input/Hotkey.h"
#include "modules/Module.h"
#include "modules/ItemScrollerLogic.h"

namespace tsukuyomi {

class ItemScroller : public Module, public containerui::Listener, public tradeui::Listener {
public:
    static ItemScroller& instance();

    const wchar_t* name() const override { return L"ItemScroller"; }
    bool available() const override;

    MenuItem buildMenu() override;
    void loadConfig(const nlohmann::json& section) override;
    void saveConfig(nlohmann::json& section) const override;
    void onScansReady() override;
    void shutdown() override;

    bool onSlotButton(std::uint32_t id, int state, const std::string& coll, int index) override;
    void onScreenTick() override;
    void onScreenLost() override;
    void onScreenCreated(void* ctrl) override;
    void onTradeSecondary(int tier, int index) override;

    void* offstackTradeController() const;
    void onTradeScreenHeldOffstack();
    void onOffstackTradeReleased();
    void onPlayerViewUpdate();

protected:
    void onUpdate() override;
    void onEnabledChanged(bool enabled) override;

private:
    ItemScroller() = default;
    void applyDisable();
    std::atomic<bool> m_disableRequested{false};
    std::atomic<int> m_unloadRelease{0};

    struct StackRef;

    struct Slot {
        std::string coll;
        int index = -1;
        bool valid() const { return index >= 0 && !coll.empty(); }
        bool operator==(const Slot& o) const { return index == o.index && coll == o.coll; }
        bool operator<(const Slot& o) const
        {
            return (coll != o.coll) ? coll < o.coll : index < o.index;
        }
    };

    enum class Group { None, Player, Storage, Grid, Output, TradeIn, TradeOut, Other };
    static Group groupOf(const std::string& coll);
    std::vector<Slot> slotsOf(Group group) const;
    std::vector<Slot> otherSlotsOf(const Slot& slot) const;
    bool screenHasStorage() const;

    bool leftClick(const Slot& s);
    bool rightClick(const Slot& s);
    bool shiftClick(const Slot& s);
    bool dropOne(const Slot& s);
    bool dropStack(const Slot& s);
    bool dropCursorAll();
    bool clickOne(const Slot& s, int kind);

    const void* stackOf(const Slot& s) const;
    bool isEmptySlot(const Slot& s) const;
    int countIn(const Slot& s) const;
    const void* cursor() const;
    bool cursorEmpty() const;

    bool shiftClickWithCheck(const Slot& s);
    void enqueueMoveStacks(std::shared_ptr<StackRef> ref, const Slot& slot, bool matchingOnly, bool toOther,
                           bool firstOnly);
    void enqueueMoveStacksFrom(const Slot& slot, bool matchingOnly, bool toOther, bool firstOnly);
    bool tryMoveSingleItemToOtherInventory(const Slot& slot);
    bool tryMoveAllButOneItemToOtherInventory(const Slot& slot);
    bool tryMoveSingleItemToThisInventory(const Slot& slot);
    void moveItemsToTargets(const Slot& source, const std::vector<Slot>& targets, bool one);
    void enqueueDropStacks(std::shared_ptr<StackRef> ref, const Slot& refSlot);
    bool shiftPlaceItems(const Slot& slot);
    bool shiftDropItems();
    void dropLeaveOne(const Slot& slot);
    enum class Amount { One, LeaveOne, Stacks, Matching };
    bool tryMoveItemsVertically(const Slot& slot, bool up, Amount amount);
    int rowOf(const Slot& s) const;

    bool tryMoveItemsByScroll(const Slot& slot, bool scrollingUp);

    enum class DragAction {
        None,
        MoveStacks,
        MoveLeaveOne,
        MoveOne,
        MoveMatching,
        DropOne,
        DropLeaveOne,
        DropStacks,
        UpStacks,
        UpMatching,
        UpLeaveOne,
        UpOne,
        DownStacks,
        DownMatching,
        DownLeaveOne,
        DownOne,
    };
    DragAction matchDrag(int mouseVk) const;
    bool dragActionHeld(DragAction action) const;
    void dragOver(const Slot& slot, bool isStart);
    void dragApply(const Slot& slot, DragAction action);
    void stopDrag();

    using Ingredient = itemscrollerlogic::Ingredient;
    using Recipe = itemscrollerlogic::Recipe;
    static constexpr int kRecipesPerPage = 18;
    static constexpr int kRecipePages = 8;
    static constexpr int kRecipeCount = kRecipesPerPage * kRecipePages;

    bool isCraftingScreen() const;
    bool isOutputSlot(const Slot& s) const;
    std::vector<Slot> gridSlots() const;
    Slot outputSlot() const;
    Recipe selectedRecipe()
    {
        if (!m_recipesLoaded) {
            loadRecipes();
        }
        const std::lock_guard lock(m_recipesMutex);
        return m_recipes[static_cast<size_t>(m_selectedRecipe)];
    }
    bool matchesIngredient(const void* stack, const Ingredient& ing) const;
    bool gridMatchesRecipe(const Recipe& recipe) const;
    static bool wantedCells(const Recipe& recipe, int gridSize, std::vector<Ingredient>& want);
    enum class Step { Done, Yield, Fail };
    Step dropCraftFromOutput(const Recipe& recipe);
    bool pendingStacks() const;
    bool craftPendingSettled();
    unsigned long long m_craftPendingSince = 0;
    bool gridOrCursorPending() const;
    bool m_harvestReady = false;
    bool m_mcActiveWas = false;
    void endMassCraftSession();
    void massCraftStep();
    void putBackCursor(const Recipe& recipe);
    Ingredient ingredientOf(const void* stack) const;
    void storeRecipeFromGrid(bool clearIfEmpty);
    Step clearGridStep();
    Step fillGridStep(const Recipe& recipe, bool fillStacks);
    Step throwCraftResultsStep(const Recipe& recipe);
    Step throwNonRecipeItemsStep(const Recipe& recipe);
    void enqueueClearGrid();
    void enqueueFillGrid(bool fillStacks);
    void enqueueCraftAsManyAsPossible();
    void craftEverything();
    void enqueueThrowCraftResults();
    void enqueueMoveCraftResults();
    void rightClickCraftOneStack(const Slot& output);
    void moveOneSetFromOutput(const Slot& output);
    bool tryMoveItemsCrafting(const Slot& slot, bool toOther, bool moveStacks, bool moveEverything);
    void massCraftTick();
    void onRecipeViewKeys();
    void changeRecipeSelection(int index);
    void loadRecipes();
    void saveRecipes() const;
    std::wstring recipeFilePath() const;

    struct RecipeView {
        bool visible;
        char page[16];
        int item[kRecipesPerPage];
        bool selected[kRecipesPerPage];
        char number[kRecipesPerPage][8];
        char count[kRecipesPerPage][8];
        int ing[9];
        int res;
        char resCount[8];
    };
    void registerUiDefinitions();
    void updateRecipeView();
    void handleViewInput();
    bool viewHovered(int& entryIndex) const;
    int idAuxOf(const Ingredient& ing);
    static bool viewBool(std::uintptr_t arg);
    static int viewInt(std::uintptr_t arg);
    static void viewText(std::uintptr_t arg, char* out, std::size_t cap);
    static void viewButton(std::uintptr_t arg);

    bool isTradeScreen() const;
    bool tryMoveItemsVillager(const Slot& slot, bool toOther, bool fullStacks);
    void fillTradeInputs(bool fullStacks);
    void clearTradeInputs();
    std::string globalTradeKeyOf(const void* offer) const;
    void refreshVillagerKey();
    bool isFavorite(int tier, int index);
    bool isVillagerFavorite(const void* offer);
    bool isGlobalFavorite(const void* offer);
    bool villagerHasOwnFavorites();
    void toggleFavorite(int tier, int index, bool global);
    static bool rowFavVillager(int tier, int index);
    static bool rowFavGlobal(int tier, int index);
    static bool rowFavGlobalIdle(int tier, int index);
    void updateTradeOrder(void* ctrl);
    void autoTradeTick(void* ctrl);
    void startThrowResults(void* ctrl);
    void closeAutoTradeScreen(const wchar_t* why);
    void tradeScreenTick(bool offstack);
    void* m_autoTradeCtrl = nullptr;
    bool m_autoTradeWanted = false;
    bool m_autoTradeOffstack = false;
    unsigned long long m_offstackTickMs = 0;
    int m_autoTradePhase = 0;
    int m_autoTradeTicks = 0;
    struct GainBase {
        int tier = 0;
        int index = 0;
        int before = 0;
    };
    std::vector<GainBase> m_autoTradeGains;
    int m_useButton = -1;
    int m_sneakButton = -1;
    bool autoTradeBypassed() const;
    std::vector<std::pair<int, int>> m_favTier;
    std::vector<int> m_favTierCounts;
    bool m_favTierSet = false;
    int tradeTierLimit(void* ctrl) const;
    int m_unlockSent = -2;
    std::string m_orderSig;
    int m_orderTicks = 0;
    unsigned m_favVersion = 0;
    std::wstring favoritesFilePath() const;
    void loadFavorites();
    void saveFavorites() const;
    int tradeFavorites();
    int countItemInPlayer(const void* like) const;
    void tradeTick();
    static std::string favoriteStarJson();
    void registerTradeUi(void* ctrl);

    void sortInventory(const Slot& focused);
    struct SortExtra {
        int boxSlots = 0;
        int bundleFill = 0;
    };
    SortExtra sortExtraOf(const void* stack) const;
    int compareStacks(const void* a, const void* b, const SortExtra& xa, const SortExtra& xb) const;
    int customPriority(const void* stack) const;
    int categoryIndex(const void* stack) const;
    static const char* categoryName(const void* stack);

    bool comboHeld(const std::vector<int>& combo, bool exactModifiers) const;
    bool comboHeldAt(const std::vector<int>& combo, bool exactModifiers, int pressVk) const;
    bool keyHeld(int vk) const;
    std::uint64_t keySeq(int vk) const;
    void watchKeys();
    int recentMouseButton() const;
    bool comboEdge(const std::vector<int>& combo, bool& wasDown, std::uint64_t& seenSeq);
    void pollHotkeys();
    void consumeWheel();
    void resyncWheelSeqs();
    unsigned long long wheelLastMs() const;
    static constexpr int kSafeClicksPerTick = 16;
    bool budgetLeft() const;
    void pushJob(std::function<bool()> step);
    void runJobs();
    void pushListJob(std::function<std::vector<Slot>()> collect, std::function<bool(const Slot&)> unit);

    static constexpr int kHotkeyCount = 29;
    bool readHotkeys(const nlohmann::json& section, std::array<std::vector<int>, kHotkeyCount>& out,
                     std::array<std::string, kHotkeyCount>& text) const;
    void watchConfigFile();
    void applyPendingKeys();
    mutable std::mutex m_keysMutex;
    std::array<std::string, kHotkeyCount> m_keyText{};
    std::array<std::vector<int>, kHotkeyCount> m_pendingKeys{};
    std::atomic<bool> m_keysPending{false};
    std::atomic<bool> m_listsPending{false};
    unsigned long long m_cfgCheckMs = 0;
    long long m_cfgStamp = 0;
    bool m_cfgWarned = false;

    struct KeySetting {
        const wchar_t* name;
        std::vector<int> keys;
        std::vector<int> defaults;
        bool wasDown = false;
        std::uint64_t seenSeq = 0;
    };
    enum KeyId {
        kCraftEverything,
        kDropAllMatching,
        kMassCraft,
        kMoveCraftResults,
        kRecipeView,
        kStoreRecipe,
        kThrowCraftResults,
        kVillagerTradeFavorites,
        kModifierMoveEverything,
        kModifierMoveMatching,
        kModifierMoveStack,
        kModifierToggleVillagerGlobalFavorite,
        kKeyDragMoveStacks,
        kKeyDragMoveLeaveOne,
        kKeyDragMoveMatching,
        kKeyDragMoveOne,
        kKeyDragDropLeaveOne,
        kKeyDragDropSingle,
        kKeyDragDropStacks,
        kKeyMoveEverything,
        kWsMoveDownLeaveOne,
        kWsMoveDownMatching,
        kWsMoveDownSingle,
        kWsMoveDownStacks,
        kWsMoveUpLeaveOne,
        kWsMoveUpMatching,
        kWsMoveUpSingle,
        kWsMoveUpStacks,
        kSortInventory,
        kKeyCount
    };
    std::array<KeySetting, kKeyCount> m_keys = makeKeys();
    std::array<int, 256> m_keySlots = [] {
        std::array<int, 256> slots{};
        slots.fill(-1);
        return slots;
    }();
    int m_namedButtons[6]{-1, -1, -1, -1, -1, -1};
    static std::array<KeySetting, kKeyCount> makeKeys();

    static constexpr const wchar_t* kDefaultTopPriority =
        L"minecraft:diamond_sword,minecraft:diamond_spear,minecraft:diamond_pickaxe,"
        L"minecraft:diamond_axe,minecraft:diamond_shovel,minecraft:diamond_hoe,"
        L"minecraft:netherite_sword,minecraft:netherite_spear,minecraft:netherite_pickaxe,"
        L"minecraft:netherite_axe,minecraft:netherite_shovel,minecraft:netherite_hoe";

    static constexpr const wchar_t* kDefaultCategoryOrder = L"construction,equipment,items,nature,other";

    std::atomic<bool> m_massCraftHold{false};
    bool m_villagerTradeUnlockAllTiers = false;
    bool m_villagerTradeFavoritesOnOpen = false;
    bool m_villagerTradeOnOpenThrowResults = false;
    std::wstring m_sortTopPriority{kDefaultTopPriority};
    std::wstring m_sortCategoryOrder{kDefaultCategoryOrder};

    std::uint32_t m_swallowId = 0;
    DragAction m_drag = DragAction::None;
    int m_dragMouseVk = 0;
    Slot m_dragLast;
    std::set<Slot> m_dragged;
    Slot m_cursorSource;
    int m_wheelLeftButton = -1;
    int m_wheelRightButton = -1;
    std::uint64_t m_wheelLeftSeen = 0;
    std::uint64_t m_wheelRightSeen = 0;
    bool m_wheelPrimed = false;
    static constexpr unsigned long long kWheelCarryGapMs = 150;
    bool m_wheelCarry = false;
    int m_clicksThisTick = 0;
    std::deque<std::function<bool()>> m_jobs;
    mutable std::mutex m_recipesMutex;
    mutable std::mutex m_recipeSaveMutex;
    std::array<Recipe, kRecipeCount> m_recipes{};
    int m_selectedRecipe = 0;
    bool m_recipesLoaded = false;
    mutable bool m_recipesDirty = false;
    bool m_recipeViewOpen = false;
    std::array<std::uint64_t, 16> m_viewKeysSeq{};
    bool m_toggleWasDown = false;
    std::uint64_t m_toggleSeenSeq = 0;
    void resyncKeySeqs();
    bool m_middleWas = false;
    RecipeView m_view{};
    int m_viewHover = -1;
    POINT m_viewHoverAt{};
    int m_viewPress = -1;
    bool m_viewMiddleWas = false;
    std::map<std::string, int> m_idAuxCache;
    struct TradeJob {
        int tier = 0;
        int index = 0;
        int phase = 0;
        int steps = 0;
        int wait = 0;
    };
    std::vector<TradeJob> m_tradeJobs;
    static constexpr int kFavEntries = 7;
    struct FavView {
        bool visible;
        int n;
        int tier[kFavEntries];
        int index[kFavEntries];
        bool global[kFavEntries];
        int buy[kFavEntries];
        int sell[kFavEntries];
        char buyCount[kFavEntries][8];
        char sellCount[kFavEntries][8];
    };
    FavView m_favView{};
    int m_favPress = -1;
    bool m_favPressRight = false;
    void updateFavView(void* ctrl);
    void handleFavInput(void* ctrl);
    static std::string favoritePanelJson();
    void* m_tradeCtrl = nullptr;
    std::string m_villagerKey;
    bool m_tradeMiddleWas = false;
    bool m_favoritesLoaded = false;
    mutable bool m_favoritesDirty = false;
    std::set<std::string> m_globalFavorites;
    std::map<std::string, std::set<std::string>> m_villagerFavorites;
    std::vector<std::string> m_topPriority;
    std::vector<std::string> m_categoryOrder;
    void rebuildLists();
};

}
