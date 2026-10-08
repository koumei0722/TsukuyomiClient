#include "game/ShulkerPreviewLayout.h"

#include <nlohmann/json.hpp>

namespace tsukuyomi::spreview {

namespace {

using json = nlohmann::json;

json named(const std::string& name, const json& body)
{
    return json{{name, body}};
}

constexpr const char* kCondition = "always_when_visible";

json collection(const char* name, const char* over)
{
    json b = {{"binding_name", name},
              {"binding_type", "collection"},
              {"binding_condition", kCondition},
              {"binding_collection_name", kCollection}};
    if (over != nullptr) {
        b["binding_name_override"] = over;
    }
    return b;
}

json cell(int index)
{
    const json details = {{"binding_type", "collection_details"}, {"binding_collection_name", kCollection}};
    const json background = {{"size", {kCell, kCell}}, {"layer", 1}};
    const json icon = {{"size", {kCell - 2, kCell - 2}},
                       {"anchor_from", "center"},
                       {"anchor_to", "center"},
                       {"layer", 7},
                       {"$item_collection_name", kCollection},
                       {"$item_renderer_binding_condition", kCondition},
                       {"property_bag", {{"force_update", true}}}};
    const json count = {{"ignored", false},
                        {"layer", 27},
                        {"$item_collection_name", kCollection},
                        {"bindings", json::array({details, collection("#inventory_stack_count", nullptr)})}};
    const json item = {{"type", "panel"},
                       {"size", {kCell, kCell}},
                       {"layer", 0},
                       {"controls", json::array({named("count@common.stack_count_label", count),
                                                 named("icon@common.item_renderer", icon)})}};
    const json durability = {{"ignored", false},
                             {"layer", 20},
                             {"$item_collection_name", kCollection},
                             {"bindings", json::array({
                                 details,
                                 collection("#item_durability_visible", "#touch_progress_bar_visible"),
                                 collection("#item_durability_total_amount", "#progress_bar_total_amount"),
                                 collection("#item_durability_current_amount", "#progress_bar_current_amount")
                             })}};
    const json storage = {{"ignored", false},
                          {"layer", 20},
                          {"$item_collection_name", kCollection},
                          {"bindings", json::array({
                              details,
                              collection("#item_storage_visible", "#progress_bar_visible"),
                              collection("#item_storage_total_amount", "#progress_bar_total_amount"),
                              collection("#item_storage_current_amount", "#progress_bar_current_amount")
                          })}};
    const json body = {{"type", "panel"},
                       {"size", {kCell, kCell}},
                       {"collection_index", index},
                       {"$item_collection_name", kCollection},
                       {"$stack_count_required", true},
                       {"$durability_bar_required", true},
                       {"$storage_bar_required", true},
                       {"controls", json::array({named("bg@common.cell_image", background),
                                                 named("item", item),
                                                 named("durability_bar@common.durability_bar", durability),
                                                 named("storage_bar@common.storage_bar", storage)})}};
    return named("tk_sp_cell" + std::to_string(index), body);
}

}

namespace {

json bundleCell(int index)
{
    json body = cell(index).begin().value();
    body["size"] = {kBundleCell, kBundleCell};
    json& controls = body["controls"];
    controls[0] = named("bg", {{"type", "image"},
                              {"texture", "textures/ui/bundle_item_background"},
                              {"size", {kCell, kCell}},
                              {"layer", 1}});
    const json notTouch = {{"binding_name", "(not #using_touch)"}, {"binding_name_override", "#visible"}};
    const json green = {{"layer", 1}, {"size", {kCell, kCell}}, {"visible", false}, {"bindings", json::array({notTouch})}};
    const json white = {{"type", "image"},
                        {"texture", "textures/ui/focus_border_white"},
                        {"layer", 2},
                        {"size", {kCell, kCell}},
                        {"visible", false},
                        {"bindings", json::array({notTouch})}};
    const json blue = {{"layer", 1},
                       {"visible", false},
                       {"bindings", json::array({{{"binding_name", "#using_touch"}, {"binding_name_override", "#visible"}}})}};
    const json highlight = {{"type", "panel"},
                            {"size", {kCell, kCell}},
                            {"layer", 2},
                            {"visible", false},
                            {"bindings", json::array({{{"binding_name", selectedBinding(index)},
                                                       {"binding_name_override", "#visible"},
                                                       {"binding_condition", "always"}}})},
                            {"controls", json::array({
                                named("bundle_selected_item_background_colour_green@common.highlight_slot", green),
                                named("bundle_selected_item_background_border_white", white),
                                named("bundle_selected_item_background_colour_blue@common.cell_image_selected", blue)})}};
    controls.insert(controls.begin() + 1, named("highlight_panel", highlight));
    return named("tk_sp_bcell" + std::to_string(index), body);
}

json bundleRow(int r)
{
    json cells = json::array();
    for (int c = 0; c < kColumns; ++c) {
        cells.push_back(bundleCell(r * kColumns + c));
    }
    return named("tk_sp_brow" + std::to_string(r),
                 {{"type", "stack_panel"},
                  {"orientation", "horizontal"},
                  {"size", {kColumns * kBundleCell, kBundleCell}},
                  {"collection_name", kCollection},
                  {"controls", cells}});
}

}

std::string selectedBinding(int index)
{
    return kBindSelectedPrefix + std::to_string(index);
}

std::string bundleJson()
{
    json rows = json::array();
    for (int r = 0; r < kRows; ++r) {
        rows.push_back(bundleRow(r));
    }
    const json grid = {{"type", "stack_panel"},
                       {"orientation", "vertical"},
                       {"size", {kColumns * kBundleCell, kRows * kBundleCell}},
                       {"offset", {kBundleMargin, kBundleHeader}},
                       {"anchor_from", "top_left"},
                       {"anchor_to", "top_left"},
                       {"layer", 3},
                       {"controls", rows}};
    const json label = {{"type", "label"},
                        {"color", "$main_header_text_color"},
                        {"layer", 4},
                        {"anchor_from", "top_left"},
                        {"anchor_to", "top_left"},
                        {"text", kBindName},
                        {"enable_profanity_filter", true},
                        {"bindings", json::array({{{"binding_name", kBindName}, {"binding_condition", "visible"}}})}};
    const json header = {{"type", "stack_panel"},
                         {"orientation", "horizontal"},
                         {"size", {"default", 15}},
                         {"offset", {kBundleMargin, kBundleMargin}},
                         {"anchor_from", "top_left"},
                         {"anchor_to", "top_left"},
                         {"layer", 2},
                         {"controls", json::array({named("bundle_label", label)})}};
    const json selectedText = {{"text", kBindSelectedName},
                               {"bindings", json::array({{{"binding_name", kBindSelectedName},
                                                          {"binding_condition", "visible"}}})}};
    const json selected = {{"anchor_to", "bottom_middle"},
                           {"anchor_from", "top_middle"},
                           {"visible", false},
                           {"controls", json::array({named("item_text_label@common.item_text_label", selectedText)})},
                           {"bindings", json::array({{{"binding_name", kBindHasSelected},
                                                      {"binding_name_override", "#visible"},
                                                      {"binding_condition", "always"}}})}};
    const json background = {{"type", "image"},
                             {"texture", "textures/ui/purpleBorder"},
                             {"layer", 1},
                             {"size", {"100%", "100%"}},
                             {"anchor_from", "top_left"},
                             {"anchor_to", "top_left"},
                             {"controls", json::array({named("selected_item_tooltip@common.item_panel_image", selected)})}};
    const json tooltip = {{"type", "panel"},
                          {"size", {kBundleWidth, kBundleHeight}},
                          {"layer", 50},
                          {"visible", false},
                          {"bindings", json::array({{{"binding_name", kBindVisible},
                                                     {"binding_name_override", "#visible"},
                                                     {"binding_condition", "always"}}})},
                          {"controls", json::array({named("background", background), named("header_stack", header),
                                                    named("item_grid", grid)})}};
    const json cursorTooltip = {{"type", "custom"},
                                {"renderer", "bundle_tooltip_renderer"},
                                {"size", {"100%cm", "100%cm"}},
                                {"layer", 20},
                                {"controls", json::array({named("tooltip", tooltip)})}};
    return json::array({named("tk_sp_bundle", cursorTooltip)}).dump();
}

std::string hoverPanelControlsJson()
{
    const json visible = {{"binding_name", "#show_persistent_bundle_hover_text"}, {"binding_name_override", "#visible"}};
    const json hoverText = {{"layer", 29},
                            {"$hover_text_binding_name|default", "#hover_text"},
                            {"bindings", json::array({{{"binding_name", "$hover_text_binding_name"},
                                                       {"binding_name_override", "#hover_text"},
                                                       {"binding_type", "collection"},
                                                       {"binding_collection_name", "$item_collection_name"}}})}};
    const json highlight = {{"controls", json::array({named("hover_text@common.hover_text", hoverText)})},
                            {"bindings", json::array({visible})}};
    const json border = {{"bindings", json::array({visible})}};
    const json normal = {{"type", "panel"},
                         {"size", {"100%", "100%"}},
                         {"bindings", json::array({{{"binding_name", kBindNormal},
                                                    {"binding_name_override", "#visible"},
                                                    {"binding_condition", "always"}}})},
                         {"controls", json::array({named("highlight@common.highlight_slot", highlight),
                                                   named("white_border@common.white_border_slot", border)})}};
    const json recorder = json::parse(recorderJson()).at(0);
    return json::array({recorder, named("tk_sp_normal", normal)}).dump();
}

std::string recorderJson()
{
    const json recorder = {{"type", "panel"},
                           {"size", {1, 1}},
                           {"bindings", json::array({
                               {{"binding_name", kBindCurrent},
                                {"binding_type", "collection"},
                                {"binding_collection_name", "$item_collection_name"},
                                {"binding_condition", "always_when_visible"}}
                           })}};
    return json::array({named("tk_sp_hover", recorder)}).dump();
}

}
