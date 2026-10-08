#include "game/ArmorHudLayout.h"

#include <nlohmann/json.hpp>

namespace tsukuyomi::armorhud {

namespace {

using json = nlohmann::json;

json named(const std::string& name, const json& body)
{
    return json{{name, body}};
}

json visibleBinding(const std::string& name)
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

bool isText(int form)
{
    return form % 2 == 1;
}

bool isVertical(int form)
{
    return form >= 2;
}

FormSize cellSize(int form)
{
    if (!isText(form)) {
        return {kPitch, kPitch};
    }
    return isVertical(form) ? FormSize{kPitch + kTextGap + kTextWidth, kPitch}
                            : FormSize{kTextWidth, kPitch + kTextHeight};
}

constexpr const char* kCondition = "always_when_visible";

json itemParts()
{
    const json icon = {{"size", {kPitch - 2, kPitch - 2}},
                       {"$item_collection_name", kCollection},
                       {"$item_renderer_binding_condition", kCondition},
                       {"property_bag", {{"force_update", true}}}};
    const json details = {{"binding_type", "collection_details"}, {"binding_collection_name", kCollection}};
    auto collection = [](const char* name, const char* over) {
        json b = {{"binding_name", name},
                  {"binding_type", "collection"},
                  {"binding_condition", kCondition},
                  {"binding_collection_name", kCollection}};
        if (over != nullptr) {
            b["binding_name_override"] = over;
        }
        return b;
    };
    const json count = {{"ignored", false},
                        {"$item_collection_name", kCollection},
                        {"bindings", json::array({
                            details,
                            collection("#inventory_stack_count", nullptr),
                            collection("#stack_count_visible", "#visible")
                        })}};
    json parts = json::array({
        named("icon@common.item_renderer", icon),
        named("count@common.stack_count_label", count)
    });
    const json dura = {{"ignored", false},
                       {"$item_collection_name", kCollection},
                       {"bindings", json::array({
                           details,
                           collection("#item_durability_visible", "#touch_progress_bar_visible"),
                           collection("#item_durability_total_amount", "#progress_bar_total_amount"),
                           collection("#item_durability_current_amount", "#progress_bar_current_amount")
                       })}};
    const json bar = {{"type", "panel"},
                      {"size", {"100%", "100%"}},
                      {"bindings", json::array({visibleBinding(kBindBar)})},
                      {"controls", json::array({named("dura@common.durability_bar", dura)})}};
    parts.push_back(named("tk_ahud_bar", bar));
    return parts;
}

json label(int item, const char* anchor, int x)
{
    const std::string text = textBinding(item);
    return {{"type", "label"},
            {"text", text},
            {"localize", false},
            {"shadow", true},
            {"layer", 4},
            {"size", {"default", "default"}},
            {"anchor_from", anchor},
            {"anchor_to", anchor},
            {"offset", {x, 0}},
            {"bindings", json::array({
                {{"binding_name", text}, {"binding_condition", "always_when_visible"}, {"binding_type", "global"}}
            })}};
}

json cell(int form, int item)
{
    const FormSize size = cellSize(form);
    json body = {{"type", "panel"},
                 {"size", {size.width, size.height}},
                 {"collection_index", item},
                 {"$item_collection_name", kCollection},
                 {"$stack_count_required", true},
                 {"$durability_bar_required", true}};
    const std::string suffix = std::to_string(form) + "_" + std::to_string(item);
    if (!isText(form)) {
        body["controls"] = itemParts();
    } else {
        const char* const iconAnchor = isVertical(form) ? "left_middle" : "top_middle";
        const json box = {{"type", "panel"},
                          {"size", {kPitch, kPitch}},
                          {"anchor_from", iconAnchor},
                          {"anchor_to", iconAnchor},
                          {"controls", itemParts()}};
        body["controls"] = json::array({
            named("tk_ahud_i" + suffix, box),
            named("tk_ahud_l" + suffix, isVertical(form) ? label(item, "left_middle", kPitch + kTextGap)
                                                          : label(item, "bottom_middle", 0))
        });
    }
    return named("tk_ahud_c" + suffix, body);
}

json formTree(int form)
{
    const FormSize size = formSize(form);
    const std::string k = std::to_string(form);
    json cells = json::array();
    for (int item = 0; item < kItems; ++item) {
        cells.push_back(cell(form, item));
    }
    const json list = {{"type", "stack_panel"},
                       {"orientation", isVertical(form) ? "vertical" : "horizontal"},
                       {"size", {size.width, size.height}},
                       {"anchor_from", "top_left"},
                       {"anchor_to", "top_left"},
                       {"collection_name", kCollection},
                       {"controls", cells}};
    const json grid = {{"type", "panel"},
                       {"size", {size.width, size.height}},
                       {"anchor_from", "top_left"},
                       {"anchor_to", "top_left"},
                       {"use_anchored_offset", true},
                       {"bindings", json::array({
                           {{"binding_name", formXBinding(form)},
                            {"binding_name_override", "#anchored_offset_value_x"},
                            {"binding_condition", "always"}},
                           {{"binding_name", formYBinding(form)},
                            {"binding_name_override", "#anchored_offset_value_y"},
                            {"binding_condition", "always"}}
                       })},
                       {"controls", json::array({named("tk_ahud_list" + k, list)})}};
    const json slot = {{"type", "panel"},
                       {"size", {size.width, size.height}},
                       {"controls", json::array({named("tk_ahud_grid" + k, grid)})}};
    const json row = {{"type", "stack_panel"},
                      {"orientation", "horizontal"},
                      {"size", {"100%", size.height}},
                      {"controls", json::array({
                          named("tk_ahud_hmid" + k, spacer(
                              {"50% - " + std::to_string(size.width / 2) + "px", "100%"}, kBindHMid)),
                          named("tk_ahud_hright" + k, spacer(
                              {"100% - " + std::to_string(size.width) + "px", "100%"}, kBindHRight)),
                          named("tk_ahud_slot" + k, slot)
                      })}};
    const json column = {{"type", "stack_panel"},
                         {"orientation", "vertical"},
                         {"size", {"100%", "100%"}},
                         {"controls", json::array({
                             named("tk_ahud_vmid" + k, spacer(
                                 {"100%", "50% - " + std::to_string(size.height / 2) + "px"}, kBindVMid)),
                             named("tk_ahud_vbot" + k, spacer(
                                 {"100%", "100% - " + std::to_string(size.height) + "px"}, kBindVBottom)),
                             named("tk_ahud_row" + k, row)
                         })}};
    return named("tk_ahud_f" + k, {{"type", "panel"},
                                   {"size", {"100%", "100%"}},
                                   {"bindings", json::array({visibleBinding(formVisibleBinding(form))})},
                                   {"controls", json::array({named("tk_ahud_col" + k, column)})}});
}

}

int formOf(int orientation, int textMode)
{
    return clampOrientation(orientation) * 2 + (clampTextMode(textMode) == kNoText ? 0 : 1);
}

FormSize formSize(int form)
{
    const FormSize cellBox = cellSize(form);
    return isVertical(form) ? FormSize{cellBox.width, cellBox.height * kItems}
                            : FormSize{cellBox.width * kItems, cellBox.height};
}

int clampOrientation(int value)
{
    return value >= 0 && value < kOrientationCount ? value : kDefaultOrientation;
}

int clampTextMode(int value)
{
    return value >= 0 && value < kTextModeCount ? value : kDefaultText;
}

int clampAnchor(int value)
{
    return value >= 0 && value < 9 ? value : kDefaultAnchor;
}

int clampOffset(int value)
{
    if (value < kOffsetMin) {
        return kOffsetMin;
    }
    return value > kOffsetMax ? kOffsetMax : value;
}

const wchar_t* orientationLabel(int value)
{
    return clampOrientation(value) == kVertical ? L"Vertical" : L"Horizontal";
}

const wchar_t* textModeLabel(int value)
{
    static constexpr const wchar_t* labels[kTextModeCount] = {L"None", L"Number", L"Percent"};
    return labels[clampTextMode(value)];
}

std::string durabilityText(int mode, int maxDamage, int damage)
{
    if (maxDamage <= 0 || clampTextMode(mode) == kNoText) {
        return {};
    }
    int remaining = maxDamage - damage;
    if (remaining < 0) {
        remaining = 0;
    }
    if (remaining > maxDamage) {
        remaining = maxDamage;
    }
    if (mode == kNumber) {
        return std::to_string(remaining);
    }
    return std::to_string(static_cast<long long>(remaining) * 100 / maxDamage) + "%";
}

std::string formVisibleBinding(int form)
{
    return "#tk_ahud_f" + std::to_string(form);
}

std::string formXBinding(int form)
{
    return "#tk_ahud_x" + std::to_string(form);
}

std::string formYBinding(int form)
{
    return "#tk_ahud_y" + std::to_string(form);
}

std::string textBinding(int item)
{
    return "#tk_ahud_t" + std::to_string(item);
}

std::string layoutJson()
{
    json forms = json::array();
    for (int form = 0; form < kForms; ++form) {
        forms.push_back(formTree(form));
    }
    const json hotbar = {{"type", "panel"},
                         {"size", {"100%", "100%"}},
                         {"bindings", json::array({visibleBinding("#hotbar_visible")})},
                         {"controls", forms}};
    const json on = {{"type", "panel"},
                     {"size", {"100%", "100%"}},
                     {"bindings", json::array({visibleBinding(kBindVisible)})},
                     {"controls", json::array({named("tk_ahud_hot", hotbar)})}};
    const json root = {{"type", "panel"},
                       {"size", {"100%", "100%"}},
                       {"layer", 1},
                       {"bindings", json::array({
                           {{"binding_name", "#hud_visible"},
                            {"binding_name_override", "#visible"},
                            {"binding_type", "global"}}
                       })},
                       {"controls", json::array({named("tk_ahud_on", on)})}};
    return json::array({named("tk_ahud", root)}).dump();
}

}
