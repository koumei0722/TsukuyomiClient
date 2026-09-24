#include "game/InventoryHudLayout.h"

#include <nlohmann/json.hpp>

namespace tsukuyomi::invhud {

namespace {

using json = nlohmann::json;

json named(const std::string& name, const json& body)
{
    return json{{name, body}};
}

json visibleBinding(const char* name)
{
    return {{"binding_name", name},
            {"binding_name_override", "#visible"},
            {"binding_condition", "always"}};
}

json spacer(const json& size, const char* binding)
{
    return {{"type", "panel"},
            {"size", size},
            {"bindings", json::array({visibleBinding(binding)})}};
}

json line(const char* kind, int index, int width, int height, int x, int y, bool dark)
{
    json body = {{"type", "image"},
                 {"anchor_from", "top_left"},
                 {"anchor_to", "top_left"},
                 {"layer", 2},
                 {"texture", "textures/ui/white_background"},
                 {"keep_ratio", false},
                 {"size", {width, height}},
                 {"offset", {x, y}}};
    if (dark) {
        body["color"] = "$sidebar_verbose_expand_border_color";
    }
    return named(std::string("tk_invhud_") + kind + std::to_string(index), body);
}

json itemParts(const char* collectionName)
{
    const json icon = {{"size", {kPitch - 2, kPitch - 2}},
                       {"$item_collection_name", collectionName},
                       {"property_bag", {{"force_update", true}}}};
    const json details = {{"binding_type", "collection_details"}, {"binding_collection_name", collectionName}};
    const json count = {{"ignored", false},
                        {"$item_collection_name", collectionName},
                        {"bindings", json::array({
                            details,
                            {{"binding_name", "#inventory_stack_count"},
                             {"binding_type", "collection"},
                             {"binding_collection_name", collectionName}},
                            {{"binding_name", "#stack_count_visible"},
                             {"binding_name_override", "#visible"},
                             {"binding_type", "collection"},
                             {"binding_collection_name", collectionName}}
                        })}};
    auto collection = [collectionName](const char* name, const char* over) {
        return json{{"binding_name", name},
                    {"binding_name_override", over},
                    {"binding_type", "collection"},
                    {"binding_collection_name", collectionName}};
    };
    const json dura = {{"ignored", false},
                       {"$item_collection_name", collectionName},
                       {"bindings", json::array({
                           details,
                           collection("#item_durability_visible", "#touch_progress_bar_visible"),
                           collection("#item_durability_total_amount", "#progress_bar_total_amount"),
                           collection("#item_durability_current_amount", "#progress_bar_current_amount")
                       })}};
    return json::array({
        named("icon@common.item_renderer", icon),
        named("count@common.stack_count_label", count),
        named("dura@common.durability_bar", dura)
    });
}

json cell(int index)
{
    const json body = {{"type", "panel"},
                       {"size", {kPitch, kPitch}},
                       {"collection_index", kFirstSlot + index},
                       {"$item_collection_name", "hotbar_items"},
                       {"$stack_count_required", true},
                       {"$durability_bar_required", true},
                       {"controls", itemParts("hotbar_items")}};
    return named("tk_invhud_c" + std::to_string(index), body);
}

}

int clampAnchor(int anchor)
{
    return anchor >= 0 && anchor < kAnchorCount ? anchor : kDefaultAnchor;
}

int clampOffset(int value)
{
    if (value < kOffsetMin) {
        return kOffsetMin;
    }
    return value > kOffsetMax ? kOffsetMax : value;
}

const wchar_t* anchorLabel(int anchor)
{
    static constexpr const wchar_t* labels[kAnchorCount] = {
        L"Top left", L"Top middle", L"Top right",
        L"Left middle", L"Center", L"Right middle",
        L"Bottom left", L"Bottom middle", L"Bottom right"
    };
    return labels[clampAnchor(anchor)];
}

Placement placementOf(int anchor)
{
    const int valid = clampAnchor(anchor);
    const int vertical = valid / 3;
    const int horizontal = valid % 3;
    return {vertical == 1, vertical == 2, horizontal == 1, horizontal == 2};
}

std::string layoutJson()
{
    json lines = json::array();
    for (int row = 0; row < kRows; ++row) {
        lines.push_back(line("hd", row, kWidth, 1, 0, row * kPitch, true));
    }
    for (int row = 0; row < kRows; ++row) {
        lines.push_back(line("hw", row, kWidth, 1, 0, row * kPitch + kPitch - 1, false));
    }
    for (int column = 0; column < kColumns; ++column) {
        lines.push_back(line("vd", column, 1, kHeight, column * kPitch, 0, true));
    }
    for (int column = 0; column < kColumns; ++column) {
        lines.push_back(line("vw", column, 1, kHeight, column * kPitch + kPitch - 1, 0, false));
    }
    json contents = json::array();
    contents.push_back(named("tk_invhud_frame",
                             {{"type", "panel"},
                              {"size", {kWidth, kHeight}},
                              {"controls", lines}}));
    json rows = json::array();
    for (int row = 0; row < kRows; ++row) {
        json cells = json::array();
        for (int column = 0; column < kColumns; ++column) {
            cells.push_back(cell(row * kColumns + column));
        }
        rows.push_back(named("tk_invhud_r" + std::to_string(row),
                             {{"type", "stack_panel"},
                              {"orientation", "horizontal"},
                              {"size", {kWidth, kPitch}},
                              {"collection_name", "hotbar_items"},
                              {"controls", cells}}));
    }
    contents.push_back(named("tk_invhud_rows",
                             {{"type", "stack_panel"},
                              {"orientation", "vertical"},
                              {"size", {kWidth, kHeight}},
                              {"anchor_from", "top_left"},
                              {"anchor_to", "top_left"},
                              {"controls", rows}}));

    const json grid = {{"type", "panel"},
                       {"size", {kWidth, kHeight}},
                       {"anchor_from", "top_left"},
                       {"anchor_to", "top_left"},
                       {"use_anchored_offset", true},
                       {"bindings", json::array({
                           {{"binding_name", kBindX},
                            {"binding_name_override", "#anchored_offset_value_x"},
                            {"binding_condition", "always"}},
                           {{"binding_name", kBindY},
                            {"binding_name_override", "#anchored_offset_value_y"},
                            {"binding_condition", "always"}}
                       })},
                       {"controls", contents}};
    const json slot = {{"type", "panel"},
                       {"size", {kWidth, kHeight}},
                       {"controls", json::array({named("tk_invhud_grid", grid)})}};
    const json row = {{"type", "stack_panel"},
                      {"orientation", "horizontal"},
                      {"size", {"100%", kHeight}},
                      {"controls", json::array({
                          named("tk_invhud_hmid", spacer(
                              {"50% - " + std::to_string(kWidth / 2) + "px", "100%"}, kBindHMid)),
                          named("tk_invhud_hright", spacer(
                              {"100% - " + std::to_string(kWidth) + "px", "100%"}, kBindHRight)),
                          named("tk_invhud_slot", slot)
                      })}};
    const json column = {{"type", "stack_panel"},
                         {"orientation", "vertical"},
                         {"size", {"100%", "100%"}},
                         {"bindings", json::array({visibleBinding("#hotbar_visible")})},
                         {"controls", json::array({
                             named("tk_invhud_vmid", spacer(
                                 {"100%", "50% - " + std::to_string(kHeight / 2) + "px"}, kBindVMid)),
                             named("tk_invhud_vbot", spacer(
                                 {"100%", "100% - " + std::to_string(kHeight) + "px"}, kBindVBottom)),
                             named("tk_invhud_row", row)
                         })}};
    const json on = {{"type", "panel"},
                     {"size", {"100%", "100%"}},
                     {"bindings", json::array({visibleBinding(kBindVisible)})},
                     {"controls", json::array({named("tk_invhud_col", column)})}};
    const json root = {{"type", "panel"},
                       {"size", {"100%", "100%"}},
                       {"layer", 1},
                       {"bindings", json::array({
                           {{"binding_name", "#hud_visible"},
                            {"binding_name_override", "#visible"},
                            {"binding_type", "global"}}
                       })},
                       {"controls", json::array({named("tk_invhud_on", on)})}};
    return json::array({named("tk_invhud", root)}).dump();
}

std::string offhandJson()
{
    const json slot = {{"type", "panel"},
                       {"size", {kOffhandSlotWidth, kOffhandHeight}},
                       {"collection_index", 0},
                       {"$item_collection_name", "offhand_items"},
                       {"$stack_count_required", true},
                       {"$durability_bar_required", true},
                       {"controls", json::array({
                           named("tk_invhud_offhand_bg@hud.hotbar_slot_image",
                                 {{"texture", "textures/ui/hotbar_0"}, {"layer", 1}}),
                           named("tk_invhud_offhand_item",
                                 {{"type", "panel"},
                                  {"size", {kPitch, kPitch}},
                                  {"layer", 3},
                                  {"controls", itemParts("offhand_items")}})
                       })}};
    const json box = {{"type", "stack_panel"},
                      {"orientation", "horizontal"},
                      {"size", {kOffhandWidth, kOffhandHeight}},
                      {"anchor_from", "left_middle"},
                      {"anchor_to", "right_middle"},
                      {"offset", {-kOffhandGap, 0}},
                      {"collection_name", "offhand_items"},
                      {"bindings", json::array({visibleBinding(kBindOffhand)})},
                      {"controls", json::array({
                          named("tk_invhud_offhand_start@hud.start_cap_image", json::object()),
                          named("tk_invhud_offhand_slot", slot),
                          named("tk_invhud_offhand_end@hud.end_cap_image", json::object())
                      })}};
    const json anchor = {{"type", "panel"},
                         {"size", {0, kOffhandHeight}},
                         {"bindings", json::array({visibleBinding("#hotbar_visible")})},
                         {"controls", json::array({named("tk_invhud_offhand_box", box)})}};
    return json::array({named("tk_invhud_offhand", anchor)}).dump();
}

}
