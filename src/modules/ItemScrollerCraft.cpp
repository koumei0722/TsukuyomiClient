#include "modules/ItemScroller.h"

#include "core/Logger.h"
#include "core/Paths.h"
#include "core/Strings.h"

#include <Windows.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>

namespace tsukuyomi {

namespace {
namespace cui = containerui;
constexpr char kGrid[] = "crafting_input_items";
constexpr char kOutput[] = "crafting_output_items";
constexpr char kRecipeBook[] = "recipe_search";
}

bool ItemScroller::isCraftingScreen() const
{
    return cui::collectionSize(kGrid) > 0 && cui::collectionSize(kOutput) > 0;
}

bool ItemScroller::isOutputSlot(const Slot& s) const
{
    return s.coll == kOutput;
}

std::vector<ItemScroller::Slot> ItemScroller::gridSlots() const
{
    std::vector<Slot> out;
    const int n = cui::collectionSize(kGrid);
    for (int i = 0; i < n; ++i) {
        out.push_back(Slot{kGrid, i});
    }
    return out;
}

ItemScroller::Slot ItemScroller::outputSlot() const
{
    return Slot{kOutput, 0};
}

ItemScroller::Ingredient ItemScroller::ingredientOf(const void* stack) const
{
    Ingredient ing;
    if (!cui::isEmpty(stack)) {
        ing.name = cui::itemName(stack);
        ing.aux = cui::auxOf(stack);
    }
    return ing;
}

bool ItemScroller::matchesIngredient(const void* stack, const Ingredient& ing) const
{
    if (ing.empty() || cui::isEmpty(stack)) {
        return false;
    }
    return cui::itemName(stack) == ing.name && cui::auxOf(stack) == ing.aux;
}

namespace {

int mapCell(int index, int fromSize, int toSize)
{
    const int fw = (fromSize == 4) ? 2 : 3;
    const int tw = (toSize == 4) ? 2 : 3;
    const int r = index / fw;
    const int c = index % fw;
    if (r >= tw || c >= tw) {
        return -1;
    }
    return r * tw + c;
}

}

void ItemScroller::storeRecipeFromGrid(bool clearIfEmpty)
{
    const void* out = stackOf(outputSlot());
    Recipe& r = selectedRecipe();
    if (cui::isEmpty(out)) {
        if (clearIfEmpty) {
            r = Recipe{};
            m_recipesDirty = true;
            log().info(L"ItemScroller: cleared recipe {}", m_selectedRecipe + 1);
        }
        return;
    }
    const std::vector<Slot> grid = gridSlots();
    Recipe rec;
    rec.gridSize = static_cast<int>(grid.size());
    for (size_t i = 0; i < grid.size() && i < rec.items.size(); ++i) {
        rec.items[i] = ingredientOf(stackOf(grid[i]));
    }
    rec.result = ingredientOf(out);
    rec.resultCount = cui::countOf(out);
    r = rec;
    m_recipesDirty = true;
    log().info(L"ItemScroller: stored recipe {} = {} x{}", m_selectedRecipe + 1,
               toUtf16(rec.result.name), rec.resultCount);
    enqueueClearGrid();
}

ItemScroller::Step ItemScroller::clearGridStep(bool nonMatchingOnly)
{
    const std::vector<Slot> grid = gridSlots();
    const Recipe& r = selectedRecipe();
    bool ok = true;
    for (size_t i = 0; i < grid.size(); ++i) {
        const void* st = stackOf(grid[i]);
        if (cui::isEmpty(st)) {
            continue;
        }
        if (nonMatchingOnly && !r.empty()) {
            bool wanted = false;
            for (size_t j = 0; j < r.items.size(); ++j) {
                const int cell = mapCell(static_cast<int>(j), r.gridSize == 0 ? 9 : r.gridSize,
                                         static_cast<int>(grid.size()));
                if (cell == static_cast<int>(i) && matchesIngredient(st, r.items[j])) {
                    wanted = true;
                    break;
                }
            }
            if (wanted) {
                continue;
            }
        }
        if (!cursorEmpty()) {
            return Step::Fail;
        }
        if (!budgetLeft()) {
            return Step::Yield;
        }
        if (!shiftClickWithCheck(grid[i]) || !isEmptySlot(grid[i])) {
            std::vector<Slot> targets = slotsOf(Group::Player);
            moveItemsToTargets(grid[i], targets, false);
        }
        if (!isEmptySlot(grid[i])) {
            ok = false;
        }
    }
    return ok ? Step::Done : Step::Fail;
}

bool ItemScroller::outputMatchesSelected() const
{
    const Recipe& r = m_recipes[static_cast<size_t>(m_selectedRecipe)];
    return !r.empty() && matchesIngredient(stackOf(outputSlot()), r.result);
}

ItemScroller::Step ItemScroller::fillGridStep(const Recipe& recipe, bool fillStacks)
{
    if (recipe.empty() || !cursorEmpty()) {
        return Step::Fail;
    }
    const std::vector<Slot> grid = gridSlots();
    const int gridSize = static_cast<int>(grid.size());
    const int fromSize = recipe.gridSize == 0 ? 9 : recipe.gridSize;
    std::vector<Ingredient> want(static_cast<size_t>(gridSize));
    for (size_t j = 0; j < recipe.items.size() && static_cast<int>(j) < fromSize; ++j) {
        if (recipe.items[j].empty()) {
            continue;
        }
        const int cell = mapCell(static_cast<int>(j), fromSize, gridSize);
        if (cell < 0) {
            return Step::Fail;
        }
        want[static_cast<size_t>(cell)] = recipe.items[j];
    }
    for (int i = 0; i < gridSize; ++i) {
        const Slot& cell = grid[static_cast<size_t>(i)];
        const void* st = stackOf(cell);
        if (!cui::isEmpty(st) && !matchesIngredient(st, want[static_cast<size_t>(i)])) {
            if (!budgetLeft()) {
                return Step::Yield;
            }
            if (!shiftClickWithCheck(cell) || !isEmptySlot(cell)) {
                moveItemsToTargets(cell, slotsOf(Group::Player), false);
            }
            if (!isEmptySlot(cell)) {
                return Step::Fail;
            }
        }
    }
    bool placedAny = false;
    std::vector<bool> done(static_cast<size_t>(gridSize), false);
    for (int i = 0; i < gridSize; ++i) {
        const Ingredient& ing = want[static_cast<size_t>(i)];
        if (ing.empty() || done[static_cast<size_t>(i)]) {
            continue;
        }
        std::vector<Slot> cells;
        for (int k = i; k < gridSize; ++k) {
            const Ingredient& other = want[static_cast<size_t>(k)];
            if (!other.empty() && other.name == ing.name && other.aux == ing.aux) {
                cells.push_back(grid[static_cast<size_t>(k)]);
                done[static_cast<size_t>(k)] = true;
            }
        }
        std::vector<Slot> sources;
        int total = 0;
        int maxStack = 0;
        for (const Slot& s : slotsOf(Group::Player)) {
            const void* st = stackOf(s);
            if (matchesIngredient(st, ing)) {
                sources.push_back(s);
                total += cui::countOf(st);
                maxStack = std::max(maxStack, cui::maxStackOf(st));
            }
        }
        for (const Slot& c : cells) {
            const void* st = stackOf(c);
            if (matchesIngredient(st, ing)) {
                total += cui::countOf(st);
                maxStack = std::max(maxStack, cui::maxStackOf(st));
            }
        }
        std::stable_sort(sources.begin(), sources.end(), [this](const Slot& a, const Slot& b) {
            return countIn(a) > countIn(b);
        });
        if (maxStack <= 0 || cells.empty()) {
            continue;
        }
        const int target = fillStacks ? std::min(maxStack, total / static_cast<int>(cells.size())) : 1;
        if (target <= 0) {
            continue;
        }
        size_t src = 0;
        Slot holding;
        for (const Slot& cell : cells) {
            int need = target - countIn(cell);
            while (need > 0) {
                if (cursorEmpty()) {
                    if (placedAny && !budgetLeft()) {
                        return Step::Yield;
                    }
                    while (src < sources.size() && isEmptySlot(sources[src])) {
                        ++src;
                    }
                    if (src >= sources.size()) {
                        break;
                    }
                    holding = sources[src];
                    leftClick(holding);
                    if (cursorEmpty()) {
                        return Step::Fail;
                    }
                }
                const int held = cui::countOf(cursor());
                const int before = countIn(cell);
                if (held <= need) {
                    leftClick(cell);
                } else {
                    rightClick(cell);
                }
                const int placed = countIn(cell) - before;
                if (placed <= 0) {
                    if (!cursorEmpty() && holding.valid()) {
                        leftClick(holding);
                    }
                    return Step::Fail;
                }
                placedAny = true;
                need -= placed;
                if (!budgetLeft() && !cursorEmpty()) {
                    leftClick(holding);
                    return cursorEmpty() ? Step::Yield : Step::Fail;
                }
            }
        }
        if (!cursorEmpty() && holding.valid()) {
            leftClick(holding);
        }
    }
    return Step::Done;
}

void ItemScroller::enqueueClearGrid()
{
    pushJob([this]() { return clearGridStep(false) != Step::Yield; });
}

void ItemScroller::enqueueFillGrid(bool fillStacks)
{
    const Recipe recipe = selectedRecipe();
    if (recipe.empty()) {
        return;
    }
    pushJob([this, recipe, fillStacks]() { return fillGridStep(recipe, fillStacks) != Step::Yield; });
}

void ItemScroller::enqueueCraftAsManyAsPossible()
{
    const Recipe recipe = selectedRecipe();
    if (recipe.empty()) {
        return;
    }
    pushJob([this, recipe]() {
        const Slot out = outputSlot();
        auto gridTotal = [this]() {
            int n = 0;
            for (const Slot& s : gridSlots()) {
                n += countIn(s);
            }
            return n;
        };
        for (int guard = 0; guard < 256; ++guard) {
            if (!budgetLeft()) {
                return false;
            }
            if (!cursorEmpty()) {
                return true;
            }
            if (!matchesIngredient(stackOf(out), recipe.result)) {
                const Step s = fillGridStep(recipe, true);
                if (s == Step::Yield) {
                    return false;
                }
                if (s == Step::Fail || !matchesIngredient(stackOf(out), recipe.result)) {
                    return true;
                }
                continue;
            }
            const int before = gridTotal();
            shiftClick(out);
            if (matchesIngredient(stackOf(out), recipe.result) && gridTotal() == before) {
                return true;
            }
        }
        return false;
    });
}

void ItemScroller::craftEverything()
{
    const Recipe recipe = selectedRecipe();
    if (recipe.empty()) {
        return;
    }
    auto failed = std::make_shared<bool>(false);
    pushJob([this, failed]() {
        const Step s = clearGridStep(false);
        if (s == Step::Yield) {
            return false;
        }
        *failed = (s == Step::Fail);
        return true;
    });
    pushJob([this, failed, recipe]() {
        if (*failed) {
            return true;
        }
        (void)recipe;
        enqueueCraftAsManyAsPossible();
        return true;
    });
}

ItemScroller::Step ItemScroller::throwCraftResultsStep(const Recipe& recipe)
{
    if (recipe.empty()) {
        return Step::Done;
    }
    for (const Slot& s : slotsOf(Group::Player)) {
        if (matchesIngredient(stackOf(s), recipe.result)) {
            if (!budgetLeft()) {
                return Step::Yield;
            }
            dropStack(s);
        }
    }
    return Step::Done;
}

void ItemScroller::enqueueThrowCraftResults()
{
    const Recipe recipe = selectedRecipe();
    if (recipe.empty()) {
        return;
    }
    pushListJob([this]() { return slotsOf(Group::Player); },
                [this, recipe](const Slot& s) {
                    if (matchesIngredient(stackOf(s), recipe.result)) {
                        dropStack(s);
                    }
                    return true;
                });
}

void ItemScroller::enqueueMoveCraftResults()
{
    const Recipe recipe = selectedRecipe();
    if (recipe.empty()) {
        return;
    }
    pushListJob([this]() { return slotsOf(Group::Player); },
                [this, recipe](const Slot& s) {
                    if (!cursorEmpty()) {
                        return false;
                    }
                    if (matchesIngredient(stackOf(s), recipe.result)) {
                        shiftClick(s);
                    }
                    return true;
                });
}

ItemScroller::Step ItemScroller::throwNonRecipeItemsStep(const Recipe& recipe)
{
    const std::vector<Slot> grid = gridSlots();
    const int gridSize = static_cast<int>(grid.size());
    const int fromSize = recipe.gridSize == 0 ? 9 : recipe.gridSize;
    for (int i = 0; i < gridSize; ++i) {
        const void* st = stackOf(grid[static_cast<size_t>(i)]);
        if (cui::isEmpty(st)) {
            continue;
        }
        bool wanted = false;
        for (size_t j = 0; j < recipe.items.size(); ++j) {
            if (mapCell(static_cast<int>(j), fromSize, gridSize) == i
                && matchesIngredient(st, recipe.items[j])) {
                wanted = true;
            }
        }
        if (!wanted) {
            const Ingredient bad = ingredientOf(st);
            for (Group g : {Group::Grid, Group::Player}) {
                for (const Slot& s : slotsOf(g)) {
                    if (matchesIngredient(stackOf(s), bad)) {
                        if (!budgetLeft()) {
                            return Step::Yield;
                        }
                        dropStack(s);
                    }
                }
            }
        }
    }
    return Step::Done;
}

void ItemScroller::rightClickCraftOneStack(const Slot& output)
{
    const void* out = stackOf(output);
    if (cui::isEmpty(out)) {
        return;
    }
    const Ingredient want = ingredientOf(out);
    if (!cursorEmpty() && !matchesIngredient(cursor(), want)) {
        return;
    }
    auto sizeLast = std::make_shared<int>(cui::countOf(cursor()));
    auto left = std::make_shared<int>(64);
    pushJob([this, output, want, sizeLast, left]() {
        while (*left > 0) {
            if (!budgetLeft()) {
                return false;
            }
            if (!matchesIngredient(stackOf(output), want)
                || (!cursorEmpty() && !matchesIngredient(cursor(), want))) {
                return true;
            }
            --*left;
            leftClick(output);
            const int size = cui::countOf(cursor());
            if (size <= *sizeLast || size >= cui::maxStackOf(cursor())) {
                return true;
            }
            *sizeLast = size;
        }
        return true;
    });
}

void ItemScroller::moveOneSetFromOutput(const Slot& output)
{
    if (!cursorEmpty() || isEmptySlot(output)) {
        return;
    }
    leftClick(output);
    if (cursorEmpty()) {
        return;
    }
    const std::vector<Slot> player = slotsOf(Group::Player);
    for (const Slot& t : player) {
        if (cursorEmpty()) {
            break;
        }
        const void* st = stackOf(t);
        if (!cui::isEmpty(st) && cui::sameItem(st, cursor()) && cui::countOf(st) < cui::maxStackOf(st)) {
            leftClick(t);
        }
    }
    for (const Slot& t : player) {
        if (cursorEmpty()) {
            break;
        }
        if (isEmptySlot(t)) {
            leftClick(t);
        }
    }
}

bool ItemScroller::tryMoveItemsCrafting(const Slot& slot, bool toOther, bool moveStacks,
                                         bool moveEverything)
{
    const Recipe recipe = selectedRecipe();
    if (toOther) {
        if (!isEmptySlot(slot)) {
            if (matchesIngredient(stackOf(slot), recipe.result)) {
                if (moveEverything) {
                    enqueueCraftAsManyAsPossible();
                } else if (moveStacks) {
                    pushJob([this, slot]() {
                        shiftClick(slot);
                        return true;
                    });
                } else {
                    pushJob([this, slot]() {
                        moveOneSetFromOutput(slot);
                        return true;
                    });
                }
            }
        } else {
            enqueueClearGrid();
        }
    } else if (!recipe.empty()) {
        enqueueFillGrid(moveStacks);
    }
    return false;
}

int ItemScroller::recipeBookIndexOf(const Ingredient& result)
{
    if (m_bookIndex >= 0 && matchesIngredient(cui::screenStackAt(kRecipeBook, m_bookIndex), result)) {
        return m_bookIndex;
    }
    m_bookIndex = -1;
    const int n = cui::screenCollectionSize(kRecipeBook);
    for (int i = 0; i < n; ++i) {
        if (matchesIngredient(cui::screenStackAt(kRecipeBook, i), result)) {
            m_bookIndex = i;
            break;
        }
    }
    return m_bookIndex;
}

int ItemScroller::countIngredientInPlayer(const Ingredient& ing) const
{
    int n = 0;
    for (const Slot& s : slotsOf(Group::Player)) {
        const void* st = stackOf(s);
        if (matchesIngredient(st, ing)) {
            n += cui::countOf(st);
        }
    }
    return n;
}

int ItemScroller::countIngredientAvailable(const Ingredient& ing) const
{
    int n = countIngredientInPlayer(ing);
    for (const Slot& s : gridSlots()) {
        const void* st = stackOf(s);
        if (matchesIngredient(st, ing)) {
            n += cui::countOf(st);
        }
    }
    return n;
}

bool ItemScroller::ingredientsAvailable(const Recipe& recipe) const
{
    for (size_t i = 0; i < recipe.items.size(); ++i) {
        const Ingredient& ing = recipe.items[i];
        if (ing.empty()) {
            continue;
        }
        int need = 0;
        for (const Ingredient& other : recipe.items) {
            if (!other.empty() && other.name == ing.name && other.aux == ing.aux) {
                ++need;
            }
        }
        if (countIngredientAvailable(ing) < need) {
            return false;
        }
    }
    return true;
}

bool ItemScroller::massCraftWithRecipeBook(const Recipe& recipe)
{
    if (std::none_of(recipe.items.begin(), recipe.items.end(), [](const Ingredient& i) { return !i.empty(); })) {
        return false;
    }
    const int index = recipeBookIndexOf(recipe.result);
    if (index < 0) {
        if (!m_bookMissWarned) {
            m_bookMissWarned = true;
            log().info(L"ItemScroller: the recipe book does not list {}; mass crafting places the items by hand",
                       toUtf16(recipe.result.name));
        }
        return false;
    }
    for (int i = 0; i < m_massCraftIterations && budgetLeft(); ++i) {
        if (!cursorEmpty()) {
            dropCursorAll();
        }
        if (throwCraftResultsStep(recipe) == Step::Yield || throwNonRecipeItemsStep(recipe) == Step::Yield) {
            break;
        }
        const int at = recipeBookIndexOf(recipe.result);
        if (at < 0) {
            break;
        }
        if (!ingredientsAvailable(recipe)) {
            ++m_badRecipeClicks;
            break;
        }
        const Ingredient& first = *std::find_if(recipe.items.begin(), recipe.items.end(),
                                                [](const Ingredient& i) { return !i.empty(); });
        const int before = countIngredientAvailable(first);
        ++m_clicksThisTick;
        const bool ok = cui::press(cui::button::kRecipeTertiary, kRecipeBook, at);
        const int after = countIngredientAvailable(first);
        if (debugLog()) {
            log().info(L"ItemScroller: recipe book craft {} [{}] {} {} -> {} {}", toUtf16(recipe.result.name), at,
                       toUtf16(first.name), before, after, ok ? L"" : L"(press failed)");
        }
        if (!ok || after >= before) {
            ++m_badRecipeClicks;
            break;
        }
        m_badRecipeClicks = std::max(0, m_badRecipeClicks - 1);
    }
    return true;
}

void ItemScroller::massCraftTick()
{
    const bool active = (comboHeld(m_keys[kMassCraft].keys, true) || m_massCraftHold)
                        && !selectedRecipe().empty();
    if (!active) {
        m_badRecipeClicks = 0;
        m_massCraftTicker = 0;
        return;
    }
    if (++m_massCraftTicker < m_massCraftInterval) {
        return;
    }
    m_massCraftTicker = 0;
    if (m_recipeBookFailureLimit > 0 && m_badRecipeClicks > m_recipeBookFailureLimit) {
        m_badRecipeClicks -= std::max(m_recipeBookFailureLimit / 16, 1);
        return;
    }
    const Recipe recipe = selectedRecipe();
    if (m_massCraftUseRecipeBook && massCraftWithRecipeBook(recipe)) {
        return;
    }
    const Slot out = outputSlot();
    for (int i = 0; i < m_massCraftIterations && budgetLeft(); ++i) {
        if (!cursorEmpty()) {
            dropCursorAll();
        }
        if (throwCraftResultsStep(recipe) == Step::Yield || throwNonRecipeItemsStep(recipe) == Step::Yield) {
            break;
        }
        const Step fill = fillGridStep(recipe, true);
        if (fill == Step::Yield) {
            break;
        }
        if (!matchesIngredient(stackOf(out), recipe.result)) {
            ++m_badRecipeClicks;
            break;
        }
        const int before = cui::countOf(stackOf(gridSlots().empty() ? out : gridSlots().front()));
        shiftClick(out);
        if (matchesIngredient(stackOf(out), recipe.result)
            && cui::countOf(stackOf(gridSlots().empty() ? out : gridSlots().front())) == before) {
            ++m_badRecipeClicks;
            break;
        }
        m_badRecipeClicks = std::max(0, m_badRecipeClicks - 1);
    }
}

void ItemScroller::changeRecipeSelection(int index)
{
    index = std::clamp(index, 0, kRecipeCount - 1);
    if (index != m_selectedRecipe) {
        m_selectedRecipe = index;
        m_recipesDirty = true;
    }
}

void ItemScroller::onRecipeViewKeys()
{
    if (!m_recipesLoaded) {
        loadRecipes();
    }
    auto edge = [this](int slot, int vk) {
        const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        const bool e = down && !m_viewKeysWas[static_cast<size_t>(slot)];
        m_viewKeysWas[static_cast<size_t>(slot)] = down;
        return e;
    };
    for (int k = 0; k < 9; ++k) {
        if (edge(k, VK_NUMPAD1 + k)) {
            changeRecipeSelection(k);
        }
    }
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const int jump = shift ? kRecipesPerPage : kRecipesPerPage / 2;
    if (edge(9, VK_UP)) {
        changeRecipeSelection(m_selectedRecipe - 1);
    }
    if (edge(10, VK_DOWN)) {
        changeRecipeSelection(m_selectedRecipe + 1);
    }
    if (edge(11, VK_LEFT) && m_selectedRecipe >= jump) {
        changeRecipeSelection(m_selectedRecipe - jump);
    }
    if (edge(12, VK_RIGHT) && m_selectedRecipe < kRecipeCount - jump) {
        changeRecipeSelection(m_selectedRecipe + jump);
    }
    const std::vector<int>& store = m_keys[kStoreRecipe].keys;
    const bool storeDown = comboHeld(store, false);
    if (storeDown && !m_middleWas) {
        std::string coll;
        int index = -1;
        if (containerui::hovered(coll, index) && coll == kOutput) {
            storeRecipeFromGrid(true);
        }
    }
    m_middleWas = storeDown;
}

std::wstring ItemScroller::recipeFilePath() const
{
    const std::filesystem::path dir = paths::dataDir() / L"ItemScroller";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return (dir / L"recipes.json").wstring();
}

void ItemScroller::loadRecipes()
{
    m_recipesLoaded = true;
    if (!m_craftingRecipesSaveToFile) {
        return;
    }
    std::ifstream in{std::filesystem::path(recipeFilePath())};
    if (!in) {
        return;
    }
    nlohmann::json root;
    try {
        in >> root;
    } catch (...) {
        log().warn(L"ItemScroller: the recipe file could not be read");
        return;
    }
    auto readIng = [](const nlohmann::json& j) {
        Ingredient ing;
        if (j.is_object()) {
            ing.name = j.value("name", std::string());
            ing.aux = j.value("aux", 0);
        }
        return ing;
    };
    if (root.contains("recipes") && root["recipes"].is_array()) {
        for (const auto& r : root["recipes"]) {
            const int index = r.value("index", -1);
            if (index < 0 || index >= kRecipeCount) {
                continue;
            }
            Recipe rec;
            rec.result = readIng(r.value("result", nlohmann::json()));
            rec.resultCount = r.value("count", 1);
            rec.gridSize = r.value("grid", 9);
            if (r.contains("items") && r["items"].is_array()) {
                size_t i = 0;
                for (const auto& it : r["items"]) {
                    if (i >= rec.items.size()) {
                        break;
                    }
                    rec.items[i++] = readIng(it);
                }
            }
            m_recipes[static_cast<size_t>(index)] = rec;
        }
    }
    m_selectedRecipe = std::clamp(root.value("selected", 0), 0, kRecipeCount - 1);
}

void ItemScroller::saveRecipes() const
{
    m_recipesDirty = false;
    if (!m_craftingRecipesSaveToFile || !m_recipesLoaded) {
        return;
    }
    nlohmann::json root;
    root["selected"] = m_selectedRecipe;
    nlohmann::json list = nlohmann::json::array();
    for (int i = 0; i < kRecipeCount; ++i) {
        const Recipe& r = m_recipes[static_cast<size_t>(i)];
        if (r.empty()) {
            continue;
        }
        nlohmann::json j;
        j["index"] = i;
        j["result"] = {{"name", r.result.name}, {"aux", r.result.aux}};
        j["count"] = r.resultCount;
        j["grid"] = r.gridSize;
        nlohmann::json items = nlohmann::json::array();
        for (const Ingredient& ing : r.items) {
            items.push_back({{"name", ing.name}, {"aux", ing.aux}});
        }
        j["items"] = std::move(items);
        list.push_back(std::move(j));
    }
    root["recipes"] = std::move(list);
    std::ofstream out(std::filesystem::path(recipeFilePath()), std::ios::binary | std::ios::trunc);
    if (out) {
        out << root.dump(2);
    }
}

}
