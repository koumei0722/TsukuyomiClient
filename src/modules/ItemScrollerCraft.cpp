#include "modules/ItemScroller.h"

#include "core/Logger.h"
#include "core/Notice.h"
#include "core/Paths.h"
#include "core/Strings.h"
#include "game/ItemStackRequest.h"

#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>

namespace tsukuyomi {

namespace {
namespace cui = containerui;
constexpr char kGrid[] = "crafting_input_items";
constexpr char kOutput[] = "crafting_output_items";
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
        ing.tagKnown = cui::tagHashOf(stack, ing.tag);
        if (!ing.tagKnown) {
            notice::failOnce("ItemScroller.tagHash",
                             L"ItemScroller: the NBT of " + toUtf16(ing.name)
                                 + L" could not be hashed (CompoundTag::hash); the recipe will not compare NBT",
                             "ItemScroller: recipes cannot tell items apart by NBT");
        }
    }
    return ing;
}

bool ItemScroller::matchesIngredient(const void* stack, const Ingredient& ing) const
{
    if (ing.empty() || cui::isEmpty(stack)) {
        return false;
    }
    if (cui::itemName(stack) != ing.name || cui::auxOf(stack) != ing.aux) {
        return false;
    }
    if (!ing.tagKnown) {
        return true;
    }
    std::uint64_t tag = 0;
    if (!cui::tagHashOf(stack, tag)) {
        notice::failOnce("ItemScroller.tagHash",
                         L"ItemScroller: the NBT of " + toUtf16(ing.name)
                             + L" could not be hashed (CompoundTag::hash); items with NBT never match a recipe",
                         "ItemScroller: recipes cannot tell items apart by NBT");
        return false;
    }
    return tag == ing.tag;
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

bool ItemScroller::wantedCells(const Recipe& recipe, int gridSize, std::vector<Ingredient>& want)
{
    const int fromSize = recipe.gridSize == 0 ? 9 : recipe.gridSize;
    want.assign(static_cast<size_t>(gridSize), Ingredient{});
    for (size_t j = 0; j < recipe.items.size() && static_cast<int>(j) < fromSize; ++j) {
        if (recipe.items[j].empty()) {
            continue;
        }
        const int cell = mapCell(static_cast<int>(j), fromSize, gridSize);
        if (cell < 0) {
            return false;
        }
        want[static_cast<size_t>(cell)] = recipe.items[j];
    }
    return true;
}

bool ItemScroller::gridMatchesRecipe(const Recipe& recipe) const
{
    const std::vector<Slot> grid = gridSlots();
    std::vector<Ingredient> want;
    if (recipe.empty() || !wantedCells(recipe, static_cast<int>(grid.size()), want)) {
        return false;
    }
    for (size_t i = 0; i < grid.size(); ++i) {
        const void* st = stackOf(grid[i]);
        if (want[i].empty() ? !cui::isEmpty(st) : !matchesIngredient(st, want[i])) {
            return false;
        }
    }
    return true;
}

void ItemScroller::storeRecipeFromGrid(bool clearIfEmpty)
{
    const void* out = stackOf(outputSlot());
    selectedRecipe();
    if (cui::isEmpty(out)) {
        if (clearIfEmpty) {
            {
                const std::lock_guard lock(m_recipesMutex);
                m_recipes[static_cast<size_t>(m_selectedRecipe)] = Recipe{};
                m_recipesDirty = true;
            }
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
    {
        const std::lock_guard lock(m_recipesMutex);
        m_recipes[static_cast<size_t>(m_selectedRecipe)] = rec;
        m_recipesDirty = true;
    }
    log().info(L"ItemScroller: stored recipe {} = {} x{} (nbt {:016x}{})", m_selectedRecipe + 1,
               toUtf16(rec.result.name), rec.resultCount, rec.result.tag, rec.result.tagKnown ? L"" : L" unknown");
    enqueueClearGrid();
}

ItemScroller::Step ItemScroller::clearGridStep()
{
    const std::vector<Slot> grid = gridSlots();
    bool ok = true;
    for (size_t i = 0; i < grid.size(); ++i) {
        const void* st = stackOf(grid[i]);
        if (cui::isEmpty(st)) {
            continue;
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

ItemScroller::Step ItemScroller::fillGridStep(const Recipe& recipe, bool fillStacks)
{
    if (recipe.empty() || !cursorEmpty()) {
        return Step::Fail;
    }
    const std::vector<Slot> grid = gridSlots();
    const int gridSize = static_cast<int>(grid.size());
    std::vector<Ingredient> want;
    if (!wantedCells(recipe, gridSize, want)) {
        return Step::Fail;
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
            if (!other.empty() && other.same(ing)) {
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
    pushJob([this]() { return clearGridStep() != Step::Yield; });
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
            if (!craftPendingSettled()) {
                return false;
            }
            if (!cursorEmpty()) {
                return true;
            }
            if (!matchesIngredient(stackOf(out), recipe.result) || !gridMatchesRecipe(recipe)) {
                const Step s = fillGridStep(recipe, true);
                if (s == Step::Yield) {
                    return false;
                }
                if (s == Step::Fail || !matchesIngredient(stackOf(out), recipe.result)
                    || !gridMatchesRecipe(recipe)) {
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
        const Step s = clearGridStep();
        if (s == Step::Yield) {
            return false;
        }
        *failed = (s == Step::Fail);
        return true;
    });
    pushJob([this, failed]() {
        if (*failed) {
            return true;
        }
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

bool ItemScroller::pendingStacks() const
{
    if (cui::netTagOf(cursor()) > 0) {
        return true;
    }
    for (Group g : {Group::Player, Group::Grid}) {
        for (const Slot& s : slotsOf(g)) {
            if (cui::netTagOf(stackOf(s)) > 0) {
                return true;
            }
        }
    }
    return false;
}

bool ItemScroller::gridOrCursorPending() const
{
    if (cui::netTagOf(cursor()) > 0) {
        return true;
    }
    for (const Slot& s : slotsOf(Group::Grid)) {
        if (cui::netTagOf(stackOf(s)) > 0) {
            return true;
        }
    }
    return false;
}

bool ItemScroller::craftPendingSettled()
{
    if (!pendingStacks()) {
        m_craftPendingSince = 0;
        return true;
    }
    const unsigned long long now = GetTickCount64();
    if (m_craftPendingSince == 0) {
        m_craftPendingSince = now;
    }
    if (now - m_craftPendingSince < 5000) {
        return false;
    }
    m_craftPendingSince = 0;
    return true;
}

void ItemScroller::putBackCursor(const Recipe& recipe)
{
    if (cursorEmpty()) {
        return;
    }
    const bool ingredient = std::any_of(recipe.items.begin(), recipe.items.end(),
                                        [&](const Ingredient& i) { return matchesIngredient(cursor(), i); });
    if (ingredient) {
        for (const Slot& s : slotsOf(Group::Player)) {
            if (cursorEmpty()) {
                return;
            }
            const void* st = stackOf(s);
            if (!cui::isEmpty(st) && !(cui::sameItem(st, cursor()) && cui::countOf(st) < cui::maxStackOf(st))) {
                continue;
            }
            if (!budgetLeft()) {
                return;
            }
            leftClick(s);
        }
        if (cursorEmpty() || !budgetLeft()) {
            return;
        }
    }
    dropCursorAll();
}

ItemScroller::Step ItemScroller::dropCraftFromOutput(const Recipe& recipe)
{
    const Slot out = outputSlot();
    if (!itemscrollerlogic::canTakeCraftOutput(m_clicksThisTick, kSafeClicksPerTick)) {
        return Step::Yield;
    }
    int taken = 0;
    while (itemscrollerlogic::canTakeCraftOutput(m_clicksThisTick, kSafeClicksPerTick)
           && matchesIngredient(stackOf(out), recipe.result)) {
        const int perSet = cui::countOf(stackOf(out));
        const void* held = cursor();
        const int before = cui::countOf(held);
        if (before > 0 && before + perSet > cui::maxStackOf(held)) {
            break;
        }
        leftClick(out);
        if (cui::countOf(cursor()) <= before) {
            break;
        }
        ++taken;
    }
    if (!cursorEmpty()) {
        const void* held = cursor();
        const bool more = matchesIngredient(stackOf(out), recipe.result)
                          && cui::countOf(held) + cui::countOf(stackOf(out)) <= cui::maxStackOf(held);
        if (!more || m_clicksThisTick + 1 < kSafeClicksPerTick) {
            dropCursorAll();
        }
    }
    return taken > 0 ? Step::Done : Step::Fail;
}

void ItemScroller::endMassCraftSession()
{
    m_harvestReady = false;

}

void ItemScroller::massCraftTick()
{
    const bool active = (comboHeld(m_keys[kMassCraft].keys, true) || m_massCraftHold)
                        && !selectedRecipe().empty();
    if (!active) {
        if (m_mcActiveWas) {
            m_mcActiveWas = false;
            if (matchesIngredient(cursor(), selectedRecipe().result) && budgetLeft()) {
                dropCursorAll();
            }
            endMassCraftSession();
        }
        return;
    }
    if (!m_mcActiveWas) {
        m_mcActiveWas = true;
    }

    massCraftStep();
}

void ItemScroller::massCraftStep()
{
    const Recipe recipe = selectedRecipe();

    const Slot out = outputSlot();
    for (int i = 0; i < 36 && budgetLeft(); ++i) {
        if (m_harvestReady && matchesIngredient(stackOf(out), recipe.result) && gridMatchesRecipe(recipe)) {
            const Step s = dropCraftFromOutput(recipe);
            if (s == Step::Yield) {
                break;
            }
            if (s == Step::Fail) {
                m_harvestReady = false;
                break;
            }
            continue;
        }
        m_harvestReady = false;
        if (!craftPendingSettled()) {
            break;
        }
        putBackCursor(recipe);
        if (!cursorEmpty()) {
            break;
        }
        if (throwCraftResultsStep(recipe) == Step::Yield || throwNonRecipeItemsStep(recipe) == Step::Yield) {
            break;
        }
        const Step fill = fillGridStep(recipe, true);
        if (fill == Step::Yield) {
            break;
        }
        if (!matchesIngredient(stackOf(out), recipe.result) || !gridMatchesRecipe(recipe)) {
            break;
        }
        if (gridOrCursorPending()) {
            break;
        }
        m_harvestReady = true;
    }
}

void ItemScroller::changeRecipeSelection(int index)
{
    index = std::clamp(index, 0, kRecipeCount - 1);
    const std::lock_guard lock(m_recipesMutex);
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
        const std::uint64_t seq = keySeq(vk);
        const bool e = seq != m_viewKeysSeq[static_cast<size_t>(slot)];
        m_viewKeysSeq[static_cast<size_t>(slot)] = seq;
        return e;
    };
    for (int k = 0; k < 9; ++k) {
        if (edge(k, VK_NUMPAD1 + k)) {
            changeRecipeSelection(k);
        }
    }
    const bool shift = keyHeld(VK_SHIFT);
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
    const std::lock_guard saveLock(m_recipeSaveMutex);
    itemscrollerlogic::Recipes recipes;
    int selected;
    {
        const std::lock_guard lock(m_recipesMutex);
        m_recipesLoaded = true;
        recipes = m_recipes;
        selected = m_selectedRecipe;
    }
    std::ifstream in{std::filesystem::path(recipeFilePath())};
    if (!in) return;
    nlohmann::json root;
    try {
        in >> root;
    } catch (const nlohmann::json::exception&) {
        log().warn(L"ItemScroller: the recipe file could not be read");
        return;
    }
    if (!itemscrollerlogic::readRecipes(root, recipes, selected)) {
        log().warn(L"ItemScroller: the recipe file has invalid field types");
        return;
    }
    const std::lock_guard lock(m_recipesMutex);
    m_recipes = std::move(recipes);
    m_selectedRecipe = selected;
}

void ItemScroller::saveRecipes() const
{
    const std::lock_guard saveLock(m_recipeSaveMutex);
    itemscrollerlogic::Recipes recipes;
    int selected;
    {
        const std::lock_guard lock(m_recipesMutex);
        if (!m_recipesLoaded || !m_recipesDirty) return;
        recipes = m_recipes;
        selected = m_selectedRecipe;
        m_recipesDirty = false;
    }
    bool saved = false;
    try {
        nlohmann::json root;
        root["selected"] = selected;
        nlohmann::json list = nlohmann::json::array();
        for (int i = 0; i < kRecipeCount; ++i) {
            const Recipe& r = recipes[static_cast<size_t>(i)];
            if (r.empty()) continue;
            auto writeIng = [](const Ingredient& ing) {
                nlohmann::json o = {{"name", ing.name}, {"aux", ing.aux}};
                if (ing.tagKnown) o["tag"] = std::format("{:016x}", ing.tag);
                return o;
            };
            nlohmann::json j;
            j["index"] = i;
            j["result"] = writeIng(r.result);
            j["count"] = r.resultCount;
            j["grid"] = r.gridSize;
            nlohmann::json items = nlohmann::json::array();
            for (const Ingredient& ing : r.items) items.push_back(writeIng(ing));
            j["items"] = std::move(items);
            list.push_back(std::move(j));
        }
        root["recipes"] = std::move(list);
        std::ofstream out(std::filesystem::path(recipeFilePath()), std::ios::binary | std::ios::trunc);
        if (out) {
            out << root.dump(2);
            out.flush();
            saved = static_cast<bool>(out);
        }
    } catch (const std::exception&) {
    }
    if (!saved) {
        const std::lock_guard lock(m_recipesMutex);
        m_recipesDirty = true;
    }
}

}
