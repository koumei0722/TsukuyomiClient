#include "modules/ItemScroller.h"

#include "core/Logger.h"
#include "core/Strings.h"
#include "game/BlockRegistry.h"
#include "game/UiProbe.h"
#include "input/Foreground.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <string>

namespace tsukuyomi {

namespace {

namespace cui = containerui;
using json = nlohmann::json;

enum BoolKind { kBVisible, kBSelected, kBFavVisible, kBFavShow, kBFavStar, kBFavGlobe };
enum IntKind { kIItem, kIIng, kIRes, kIFavBuy, kIFavSell };
enum TextKind { kTPage, kTNum, kTCount, kTResCount, kTFavBuyCount, kTFavSellCount };
enum ButtonKind { kBtnLeft, kBtnRight, kBtnHover, kBtnFavLeft, kBtnFavRight };

constexpr std::uintptr_t arg(int kind, int index)
{
    return (static_cast<std::uintptr_t>(kind) << 8) | static_cast<std::uintptr_t>(index);
}

std::string num(int i)
{
    return std::to_string(i);
}

json binding(const std::string& name, const char* over, const char* condition)
{
    json b = {{"binding_name", name}};
    if (over != nullptr) {
        b["binding_name_override"] = over;
    }
    if (condition != nullptr) {
        b["binding_condition"] = condition;
    }
    return b;
}

json itemIcon(const std::string& idName)
{
    return {{"icon",
             {{"type", "custom"},
              {"renderer", "inventory_item_renderer"},
              {"size", {16, 16}},
              {"layer", 5},
              {"bindings", json::array({binding(idName, "#item_id_aux", nullptr)})}}}};
}

json countLabel(const std::string& name)
{
    return {{"count",
             {{"type", "label"},
              {"text", name},
              {"color", "$item_stack_count_color"},
              {"shadow", true},
              {"anchor_from", "bottom_right"},
              {"anchor_to", "bottom_right"},
              {"offset", {0, 1}},
              {"layer", 7},
              {"bindings", json::array({binding(name, nullptr, "always_when_visible")})}}}};
}

json cell(const std::string& name, int x, int y, const std::string& idName,
          const std::string& countName)
{
    json kids = json::array({itemIcon(idName)});
    if (!countName.empty()) {
        kids.push_back(countLabel(countName));
    }
    return {{name,
             {{"type", "image"},
              {"texture", "textures/ui/cell_image"},
              {"size", {18, 18}},
              {"anchor_from", "top_left"},
              {"anchor_to", "top_left"},
              {"offset", {x, y}},
              {"layer", 1},
              {"controls", kids}}}};
}

json entry(int i)
{
    const int col = i / 9;
    const int row = i % 9;
    const std::string n = num(i);
    const std::string cellTex = "textures/ui/cell_image";
    json mappings = json::array({
        {{"from_button_id", "button.menu_select"}, {"to_button_id", "button.tk_is_rv_l" + n},
         {"mapping_type", "pressed"}},
        {{"from_button_id", "button.menu_secondary_select"}, {"to_button_id", "button.tk_is_rv_r" + n},
         {"mapping_type", "pressed"}},
        {{"from_button_id", "button.menu_auto_place"}, {"to_button_id", "button.tk_is_rv_l" + n},
         {"mapping_type", "pressed"}},
        {{"to_button_id", "button.tk_is_rv_h" + n}, {"mapping_type", "pressed"}},
    });
    json slot = {{"slot",
                  {{"type", "panel"},
                   {"size", {18, 18}},
                   {"anchor_from", "right_middle"},
                   {"anchor_to", "right_middle"},
                   {"layer", 1},
                   {"controls",
                    json::array({
                        {{"bg", {{"type", "image"}, {"texture", cellTex}, {"layer", 1}}}},
                        {{"selected",
                          {{"type", "image"},
                           {"texture", "textures/ui/focus_border_white"},
                           {"layer", 6},
                           {"bindings", json::array({binding("#tk_is_rv_sel_" + n, "#visible", "always")})}}}},
                        itemIcon("#tk_is_rv_item_" + n),
                        countLabel("#tk_is_rv_cnt_" + n),
                    })}}}};
    return {{"tk_is_rv_e" + n,
             {{"type", "button"},
              {"size", {32, 18}},
              {"anchor_from", "top_left"},
              {"anchor_to", "top_left"},
              {"offset", {78 + col * 34, 16 + row * 18}},
              {"layer", 2},
              {"hover_control", "hover"},
              {"button_mappings", mappings},
              {"controls",
               json::array({
                   {{"number",
                     {{"type", "label"},
                      {"text", "#tk_is_rv_num_" + n},
                      {"color", "$dark_body_text_color"},
                      {"anchor_from", "left_middle"},
                      {"anchor_to", "left_middle"},
                      {"layer", 3},
                      {"bindings", json::array({binding("#tk_is_rv_num_" + n, nullptr,
                                                        "always_when_visible")})}}}},
                   slot,
                   {{"hover@common.highlight_slot",
                     {{"size", {16, 16}},
                      {"anchor_from", "right_middle"},
                      {"anchor_to", "right_middle"},
                      {"offset", {-1, 0}},
                      {"layer", 8}}}},
               })}}}};
}

std::string recipeViewJson()
{
    json kids = json::array();
    kids.push_back({{"bg@common.dialog_background_opaque", {{"layer", 0}}}});
    kids.push_back({{"page",
                     {{"type", "label"},
                      {"text", "#tk_is_rv_page"},
                      {"color", "$dark_body_text_color"},
                      {"anchor_from", "top_left"},
                      {"anchor_to", "top_left"},
                      {"offset", {6, 5}},
                      {"layer", 3},
                      {"bindings", json::array({binding("#tk_is_rv_page", nullptr, "always_when_visible")})}}}});
    for (int i = 0; i < 9; ++i) {
        const std::string n = num(i);
        kids.push_back(cell("tk_is_rv_g" + n, 6 + (i % 3) * 18, 36 + (i / 3) * 18,
                            "#tk_is_rv_ing_" + n, ""));
    }
    kids.push_back(cell("tk_is_rv_res", 24, 104, "#tk_is_rv_res", "#tk_is_rv_rescnt"));
    for (int i = 0; i < 18; ++i) {
        kids.push_back(entry(i));
    }
    json panel = {{"tk_is_recipe_view",
                   {{"type", "panel"},
                    {"size", {146, 182}},
                    {"anchor_from", "center"},
                    {"anchor_to", "right_middle"},
                    {"offset", {-167, 0}},
                    {"layer", 40},
                    {"bindings", json::array({binding("#tk_is_rv_visible", "#visible", "always")})},
                    {"controls", kids}}}};
    return json::array({panel}).dump();
}

std::string outsideCatcherJson()
{
    json panel = {{"tk_is_outside",
                   {{"type", "input_panel"},
                    {"size", {"100%", "100%"}},
                    {"layer", 1},
                    {"button_mappings", json::array({
                                            {{"from_button_id", "button.menu_auto_place"},
                                             {"to_button_id", "button.cursor_drop_all"},
                                             {"mapping_type", "pressed"}},
                                        })}}}};
    return json::array({panel}).dump();
}

json favEntry(int i)
{
    const std::string n = num(i);
    json mappings = json::array({
        {{"from_button_id", "button.menu_select"}, {"to_button_id", "button.tk_is_fv_l" + n},
         {"mapping_type", "pressed"}},
        {{"from_button_id", "button.menu_secondary_select"}, {"to_button_id", "button.tk_is_fv_r" + n},
         {"mapping_type", "pressed"}},
        {{"from_button_id", "button.menu_auto_place"}, {"to_button_id", "button.tk_is_fv_r" + n},
         {"mapping_type", "pressed"}},
    });
    auto mark = [&n](const char* name, const char* texture, const char* bindName) {
        return json{{name,
                     {{"type", "image"},
                      {"texture", texture},
                      {"size", {7, 7}},
                      {"anchor_from", "top_left"},
                      {"anchor_to", "top_left"},
                      {"offset", {-3, -2}},
                      {"layer", 9},
                      {"bindings", json::array({binding(std::string(bindName) + n, "#visible", "always")})}}}};
    };
    json kids = json::array({
        cell("buy", 0, 0, "#tk_is_fv_buy_" + n, "#tk_is_fv_buycnt_" + n),
        cell("sell", 22, 0, "#tk_is_fv_sell_" + n, "#tk_is_fv_sellcnt_" + n),
        {{"hover@common.highlight_slot",
          {{"size", {40, 18}}, {"anchor_from", "top_left"}, {"anchor_to", "top_left"}, {"layer", 8}}}},
        mark("star", "textures/ui/filledStar", "#tk_is_fv_star_"),
        mark("globe", "textures/ui/filledStarFocus", "#tk_is_fv_globe_"),
    });
    return {{"tk_is_fv_e" + n,
             {{"type", "button"},
              {"size", {40, 18}},
              {"anchor_from", "top_left"},
              {"anchor_to", "top_left"},
              {"offset", {5, 16 + i * 20}},
              {"layer", 2},
              {"hover_control", "hover"},
              {"button_mappings", mappings},
              {"bindings", json::array({binding("#tk_is_fv_show_" + n, "#visible", "always")})},
              {"controls", kids}}}};
}

void copyText(char* out, std::size_t cap, const char* text)
{
    if (cap == 0) {
        return;
    }
    std::snprintf(out, cap, "%s", text);
}

}

std::string ItemScroller::favoritePanelJson()
{
    json kids = json::array();
    kids.push_back({{"bg@common.dialog_background_opaque", {{"layer", 0}}}});
    kids.push_back({{"title",
                     {{"type", "image"},
                      {"texture", "textures/ui/filledStar"},
                      {"size", {8, 8}},
                      {"anchor_from", "top_middle"},
                      {"anchor_to", "top_middle"},
                      {"offset", {0, 5}},
                      {"layer", 3}}}});
    for (int i = 0; i < kFavEntries; ++i) {
        kids.push_back(favEntry(i));
    }
    json panel = {{"tk_is_fav_panel",
                   {{"type", "panel"},
                    {"size", {50, 20 + kFavEntries * 20}},
                    {"anchor_from", "center"},
                    {"anchor_to", "right_middle"},
                    {"offset", {-162, 0}},
                    {"layer", 40},
                    {"bindings", json::array({binding("#tk_is_fv_visible", "#visible", "always")})},
                    {"controls", kids}}}};
    return json::array({panel}).dump();
}

void ItemScroller::registerUiDefinitions()
{
    static bool done = false;
    if (done) {
        return;
    }
    done = true;
    uiprobe::registerDefExtension("crafting", "recipe_inventory_screen_content", "", recipeViewJson());
    uiprobe::registerDefExtension("common", "screen_background", "", outsideCatcherJson());
    uiprobe::registerDefExtension("trade2", "toggle_content_holder", "", favoriteStarJson());
    uiprobe::registerDefExtension("trade2", "screen_panel", "", favoritePanelJson());
    uiprobe::registerDefAppend("trade2", "trade_result_item_slot_button", "button_mappings",
                               R"([{"to_button_id": "button.shape_drawing", "mapping_type": "pressed"}])");
}

void ItemScroller::onScreenCreated(void* ctrl)
{
    applyDisable();
    int ok = 0;
    int tried = 0;
    auto count = [&ok, &tried](bool r) {
        ++tried;
        ok += r ? 1 : 0;
    };
    count(cui::bindBool(ctrl, "#tk_is_rv_visible", &ItemScroller::viewBool, arg(kBVisible, 0)));
    count(cui::bindText(ctrl, "#tk_is_rv_page", &ItemScroller::viewText, arg(kTPage, 0)));
    for (int i = 0; i < kRecipesPerPage; ++i) {
        const std::string n = num(i);
        count(cui::bindBool(ctrl, ("#tk_is_rv_sel_" + n).c_str(), &ItemScroller::viewBool,
                            arg(kBSelected, i)));
        count(cui::bindInt(ctrl, ("#tk_is_rv_item_" + n).c_str(), &ItemScroller::viewInt, arg(kIItem, i)));
        count(cui::bindText(ctrl, ("#tk_is_rv_num_" + n).c_str(), &ItemScroller::viewText, arg(kTNum, i)));
        count(cui::bindText(ctrl, ("#tk_is_rv_cnt_" + n).c_str(), &ItemScroller::viewText, arg(kTCount, i)));
        count(cui::onButtonPressed(ctrl, ("button.tk_is_rv_l" + n).c_str(), &ItemScroller::viewButton,
                                   arg(kBtnLeft, i)));
        count(cui::onButtonPressed(ctrl, ("button.tk_is_rv_r" + n).c_str(), &ItemScroller::viewButton,
                                   arg(kBtnRight, i)));
        count(cui::onButtonHovered(ctrl, ("button.tk_is_rv_h" + n).c_str(), &ItemScroller::viewButton,
                                   arg(kBtnHover, i)));
    }
    for (int i = 0; i < 9; ++i) {
        const std::string n = num(i);
        count(cui::bindInt(ctrl, ("#tk_is_rv_ing_" + n).c_str(), &ItemScroller::viewInt, arg(kIIng, i)));
    }
    count(cui::bindInt(ctrl, "#tk_is_rv_res", &ItemScroller::viewInt, arg(kIRes, 0)));
    count(cui::bindText(ctrl, "#tk_is_rv_rescnt", &ItemScroller::viewText, arg(kTResCount, 0)));
    count(cui::bindBool(ctrl, "#tk_is_fv_visible", &ItemScroller::viewBool, arg(kBFavVisible, 0)));
    for (int i = 0; i < kFavEntries; ++i) {
        const std::string n = num(i);
        count(cui::bindBool(ctrl, ("#tk_is_fv_show_" + n).c_str(), &ItemScroller::viewBool, arg(kBFavShow, i)));
        count(cui::bindBool(ctrl, ("#tk_is_fv_star_" + n).c_str(), &ItemScroller::viewBool, arg(kBFavStar, i)));
        count(cui::bindBool(ctrl, ("#tk_is_fv_globe_" + n).c_str(), &ItemScroller::viewBool, arg(kBFavGlobe, i)));
        count(cui::bindInt(ctrl, ("#tk_is_fv_buy_" + n).c_str(), &ItemScroller::viewInt, arg(kIFavBuy, i)));
        count(cui::bindInt(ctrl, ("#tk_is_fv_sell_" + n).c_str(), &ItemScroller::viewInt, arg(kIFavSell, i)));
        count(cui::bindText(ctrl, ("#tk_is_fv_buycnt_" + n).c_str(), &ItemScroller::viewText,
                            arg(kTFavBuyCount, i)));
        count(cui::bindText(ctrl, ("#tk_is_fv_sellcnt_" + n).c_str(), &ItemScroller::viewText,
                            arg(kTFavSellCount, i)));
        count(cui::onButtonPressed(ctrl, ("button.tk_is_fv_l" + n).c_str(), &ItemScroller::viewButton,
                                   arg(kBtnFavLeft, i)));
        count(cui::onButtonPressed(ctrl, ("button.tk_is_fv_r" + n).c_str(), &ItemScroller::viewButton,
                                   arg(kBtnFavRight, i)));
    }
    registerTradeUi(ctrl);
    applyPendingKeys();
    m_autoTradeCtrl = ctrl;
    m_autoTradePhase = 0;
    m_autoTradeTicks = 0;
    m_autoTradeGains.clear();
    m_autoTradeOffstack = false;
    m_autoTradeWanted = enabled() && m_villagerTradeFavoritesOnOpen
                        && !autoTradeBypassed();
    std::memset(&m_view, 0, sizeof(m_view));
    std::memset(&m_favView, 0, sizeof(m_favView));
    m_favPress = -1;
    m_viewHover = -1;
    static int said = 0;
    if (said < 3 || ok != tried) {
        ++said;
        log().info(L"ItemScroller: registered {}/{} bindings on the container screen", ok, tried);
    }
}

bool ItemScroller::viewBool(std::uintptr_t a)
{
    const RecipeView& v = instance().m_view;
    const FavView& f = instance().m_favView;
    const int kind = static_cast<int>(a >> 8);
    const int i = static_cast<int>(a & 0xFF);
    switch (kind) {
    case kBVisible:
        return v.visible;
    case kBSelected:
        return i < kRecipesPerPage && v.selected[i];
    case kBFavVisible:
        return f.visible;
    case kBFavShow:
        return f.visible && i < f.n;
    case kBFavStar:
        return f.visible && i < f.n && !f.global[i];
    case kBFavGlobe:
        return f.visible && i < f.n && f.global[i];
    default:
        return false;
    }
}

int ItemScroller::viewInt(std::uintptr_t a)
{
    const RecipeView& v = instance().m_view;
    const int kind = static_cast<int>(a >> 8);
    const int i = static_cast<int>(a & 0xFF);
    switch (kind) {
    case kIItem:
        return i < kRecipesPerPage ? v.item[i] : 0;
    case kIIng:
        return i < 9 ? v.ing[i] : 0;
    case kIRes:
        return v.res;
    case kIFavBuy:
        return i < kFavEntries ? instance().m_favView.buy[i] : 0;
    case kIFavSell:
        return i < kFavEntries ? instance().m_favView.sell[i] : 0;
    default:
        return 0;
    }
}

void ItemScroller::viewText(std::uintptr_t a, char* out, std::size_t cap)
{
    const RecipeView& v = instance().m_view;
    const int kind = static_cast<int>(a >> 8);
    const int i = static_cast<int>(a & 0xFF);
    switch (kind) {
    case kTPage:
        copyText(out, cap, v.page);
        break;
    case kTNum:
        copyText(out, cap, i < kRecipesPerPage ? v.number[i] : "");
        break;
    case kTCount:
        copyText(out, cap, i < kRecipesPerPage ? v.count[i] : "");
        break;
    case kTResCount:
        copyText(out, cap, v.resCount);
        break;
    case kTFavBuyCount:
        copyText(out, cap, i < kFavEntries ? instance().m_favView.buyCount[i] : "");
        break;
    case kTFavSellCount:
        copyText(out, cap, i < kFavEntries ? instance().m_favView.sellCount[i] : "");
        break;
    default:
        copyText(out, cap, "");
        break;
    }
}

void ItemScroller::viewButton(std::uintptr_t a)
{
    ItemScroller& self = instance();
    self.applyDisable();
    const int kind = static_cast<int>(a >> 8);
    const int i = static_cast<int>(a & 0xFF);
    if (kind == kBtnHover) {
        self.m_viewHover = i;
        GetCursorPos(&self.m_viewHoverAt);
        return;
    }
    if (kind == kBtnFavLeft || kind == kBtnFavRight) {
        self.m_favPress = i;
        self.m_favPressRight = (kind == kBtnFavRight);
        return;
    }
    self.m_viewPress = i;
}

int ItemScroller::idAuxOf(const Ingredient& ing)
{
    if (ing.empty()) {
        return 0;
    }
    const std::string key = ing.name + '|' + std::to_string(ing.aux);
    const auto it = m_idAuxCache.find(key);
    if (it != m_idAuxCache.end()) {
        return it->second;
    }
    const void* const item = blocks::itemByName(ing.name);
    const int value = (item != nullptr) ? cui::idAuxOfItem(item, ing.aux) : 0;
    m_idAuxCache.emplace(key, value);

    return value;
}

bool ItemScroller::viewHovered(int& entryIndex) const
{
    if (m_viewHover < 0) {
        return false;
    }
    POINT now{};
    GetCursorPos(&now);
    const int pitch = std::max(16, cui::slotPitchPixels());
    if (std::abs(now.x - m_viewHoverAt.x) > pitch || std::abs(now.y - m_viewHoverAt.y) > pitch) {
        return false;
    }
    entryIndex = m_viewHover;
    return true;
}

void ItemScroller::updateRecipeView()
{
    RecipeView next{};
    next.visible = m_recipeViewOpen;
    if (next.visible) {
        if (!m_recipesLoaded) {
            loadRecipes();
        }
        std::array<Recipe, kRecipesPerPage> recipes;
        Recipe shownRecipe;
        int selected;
        int first;
        int hovered = -1;
        const bool overRecipe = viewHovered(hovered);
        {
            const std::lock_guard lock(m_recipesMutex);
            selected = m_selectedRecipe;
            first = (selected / kRecipesPerPage) * kRecipesPerPage;
            for (int i = 0; i < kRecipesPerPage; ++i) {
                recipes[static_cast<size_t>(i)] = m_recipes[static_cast<size_t>(first + i)];
            }
            const int shown = overRecipe ? first + hovered : selected;
            if (shown >= 0 && shown < kRecipeCount) shownRecipe = m_recipes[static_cast<size_t>(shown)];
        }
        std::snprintf(next.page, sizeof(next.page), "Page %d/%d", first / kRecipesPerPage + 1,
                      kRecipePages);
        for (int i = 0; i < kRecipesPerPage; ++i) {
            const Recipe& r = recipes[static_cast<size_t>(i)];
            std::snprintf(next.number[i], sizeof(next.number[i]), "%d", first + i + 1);
            next.selected[i] = (first + i == selected);
            if (!r.empty()) {
                next.item[i] = idAuxOf(r.result);
                if (r.resultCount > 1) {
                    std::snprintf(next.count[i], sizeof(next.count[i]), "%d", r.resultCount);
                }
            }
        }

        const int shown = overRecipe ? first + hovered : selected;
        if (shown >= 0 && shown < kRecipeCount) {
            const Recipe& r = shownRecipe;
            if (!r.empty()) {
                const int width = (r.gridSize == 4) ? 2 : 3;
                for (int j = 0; j < 9; ++j) {
                    const Ingredient& ing = r.items[static_cast<size_t>(j)];
                    if (ing.empty()) {
                        continue;
                    }
                    const int cellIndex = (width == 2) ? ((j / 2) * 3 + (j % 2)) : j;
                    if (width == 2 && j >= 4) {
                        continue;
                    }
                    next.ing[cellIndex] = idAuxOf(ing);
                }
                next.res = idAuxOf(r.result);
                if (r.resultCount > 1) {
                    std::snprintf(next.resCount, sizeof(next.resCount), "%d", r.resultCount);
                }
            }
        }

    }
    if (std::memcmp(&next, &m_view, sizeof(next)) != 0) {
        std::memcpy(&m_view, &next, sizeof(next));
        cui::requestRefresh();

    }
}

void ItemScroller::handleViewInput()
{
    const int pressed = m_viewPress;
    m_viewPress = -1;
    if (pressed >= 0 && m_recipeViewOpen) {
        const int first = (m_selectedRecipe / kRecipesPerPage) * kRecipesPerPage;
        const int index = first + pressed;
        if (index >= 0 && index < kRecipeCount) {
            const bool changed = (index != m_selectedRecipe);
            changeRecipeSelection(index);
            if (changed) {
                enqueueClearGrid();
            } else {
                const bool shift = keyHeld(VK_SHIFT);
                enqueueFillGrid(shift);
            }

        }
    }
    const bool middle = keyHeld(VK_MBUTTON);
    int hovered = -1;
    if (middle && !m_viewMiddleWas && m_recipeViewOpen && viewHovered(hovered)) {
        enqueueClearGrid();
    }
    m_viewMiddleWas = middle;
}

}
