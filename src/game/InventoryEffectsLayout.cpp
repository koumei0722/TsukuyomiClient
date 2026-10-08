#include "game/InventoryEffectsLayout.h"

#include "game/InventoryEffectsLogic.h"

#include <nlohmann/json.hpp>

namespace tsukuyomi::invfx {

namespace {

using json = nlohmann::json;

json named(const std::string& name, const json& body)
{
    return json{{name, body}};
}

json visibleBinding(const std::string& name)
{
    return {{"binding_name", name}, {"binding_name_override", "#visible"}, {"binding_condition", "always"}};
}

json valueBinding(const std::string& name, const char* overrideName = nullptr)
{
    json b = {{"binding_name", name}, {"binding_condition", "always_when_visible"}};
    if (overrideName != nullptr) {
        b["binding_name_override"] = overrideName;
    }
    return b;
}

json panel(const json& size)
{
    return {{"type", "panel"}, {"size", size}};
}

json gate(int threshold, bool above, const json& controls)
{
    const int edge = threshold * 1000 - 500;
    const std::string width = above ? "100000% - " + std::to_string(edge) + "px"
                                    : "-100000% + " + std::to_string(edge) + "px";
    return {{"type", "panel"},
            {"anchor_from", "top_left"},
            {"anchor_to", "top_left"},
            {"size", {width, "100%"}},
            {"min_size", {0, "100%"}},
            {"max_size", {"100%", "100%"}},
            {"clips_children", true},
            {"controls", controls}};
}

json icon(int row)
{
    return {{"type", "image"},
            {"texture", "#texture"},
            {"size", {kIconSize, kIconSize}},
            {"offset", {kIconInset, kIconInset}},
            {"anchor_from", "top_left"},
            {"anchor_to", "top_left"},
            {"layer", 1},
            {"bindings", json::array({valueBinding(iconBinding(row), "#texture")})}};
}

json boxImage(int row, const json& size)
{
    return {{"type", "image"},
            {"texture", "#texture"},
            {"anchor_from", "top_left"},
            {"anchor_to", "top_left"},
            {"size", size},
            {"bindings", json::array({valueBinding(bgBinding(row), "#texture")})}};
}

json label(const std::string& binding, const char* color)
{
    return {{"type", "label"},
            {"text", binding},
            {"color", color},
            {"shadow", true},
            {"text_alignment", "left"},
            {"bindings", json::array({valueBinding(binding)})}};
}

json fullRow(int row)
{
    const json text = {{"type", "stack_panel"},
                       {"orientation", "vertical"},
                       {"size", {"100%cm", kBoxHeight}},
                       {"controls", json::array({named("pad_top", panel({1, kIconInset})),
                                                 named("name", label(nameBinding(row), "$body_text_color")),
                                                 named("time", label(timeBinding(row),
                                                                     "$dark_bg_contrast_button_default_text_color"))})}};
    const json content = {{"type", "stack_panel"},
                          {"orientation", "horizontal"},
                          {"anchor_from", "top_left"},
                          {"anchor_to", "top_left"},
                          {"size", {"100%c", kBoxHeight}},
                          {"layer", 1},
                          {"controls", json::array({named("pad_icon", panel({kTextX, kBoxHeight})),
                                                    named("text", text),
                                                    named("pad_end", panel({kSpacing, kBoxHeight}))})}};
    json box = boxImage(row, {"100%cm", kBoxHeight});
    box["max_size"] = {"100% - " + std::to_string(kSpacing) + "px", kBoxHeight};
    box["clips_children"] = true;
    box["controls"] = json::array({named("content", content), named("icon", icon(row))});
    return box;
}

json compactRow(int row)
{
    json box = boxImage(row, {kBoxHeight, kBoxHeight});
    box["controls"] = json::array({named("icon", icon(row))});
    const json hover = {{"layer", 100},
                        {"bindings", json::array({valueBinding(tipBinding(row), "#hover_text")})}};
    const json tip = {{"type", "button"},
                      {"size", {"100%", "100%"}},
                      {"anchor_from", "top_left"},
                      {"anchor_to", "top_left"},
                      {"hover_control", "hover"},
                      {"layer", 2},
                      {"controls", json::array({named("hover@common.hover_text", hover)})}};
    return json::array({named("box", box), named("tip", tip)});
}

json rowPanel(int row, const json& size, const json& controls)
{
    return {{"type", "panel"},
            {"anchor_from", "top_left"},
            {"anchor_to", "top_left"},
            {"size", size},
            {"layer", row * 2},
            {"use_anchored_offset", true},
            {"bindings", json::array({visibleBinding(rowBinding(row)),
                                      {{"binding_name", yBinding(row)},
                                       {"binding_name_override", "#anchored_offset_value_y"},
                                       {"binding_condition", "always"}}})},
            {"controls", controls}};
}

std::string indexed(const char* prefix, int row)
{
    return std::string(prefix) + std::to_string(row);
}

}

std::string rowBinding(int row) { return indexed("#tk_fx_row", row); }
std::string yBinding(int row) { return indexed("#tk_fx_y", row); }
std::string nameBinding(int row) { return indexed("#tk_fx_name", row); }
std::string timeBinding(int row) { return indexed("#tk_fx_time", row); }
std::string iconBinding(int row) { return indexed("#tk_fx_icon", row); }
std::string bgBinding(int row) { return indexed("#tk_fx_bg", row); }
std::string tipBinding(int row) { return indexed("#tk_fx_tip", row); }

std::string layoutJson()
{
    json fullRows = json::array();
    json compactRows = json::array();
    for (int row = 0; row < kMaxRows; ++row) {
        fullRows.push_back(named(indexed("tk_fx_f", row),
                                 rowPanel(row, {"100%", kBoxHeight}, json::array({named("box", fullRow(row))}))));
        json compact = rowPanel(row, {"100%", kBoxHeight}, compactRow(row));
        compact["max_size"] = {kBoxHeight, kBoxHeight};
        compactRows.push_back(named(indexed("tk_fx_c", row), compact));
    }
    const json any = gate(kMinSpace, true,
                          json::array({named("tk_fx_full", gate(kFullSpace, true, fullRows)),
                                       named("tk_fx_compact", gate(kFullSpace, false, compactRows))}));
    const json body = {{"type", "panel"},
                       {"size", {"fill", "100%"}},
                       {"bindings", json::array({visibleBinding(kBindOn)})},
                       {"controls", json::array({named("tk_fx_any", any)})}};
    const json root = {{"type", "stack_panel"},
                       {"orientation", "horizontal"},
                       {"anchor_from", "center"},
                       {"anchor_to", "top_left"},
                       {"offset", {0, -kPanelHeight / 2}},
                       {"size", {"50%", kPanelHeight}},
                       {"layer", 1},
                       {"ignored", kIgnoreVariable},
                       {"controls", json::array({
                           named("pad_survival", {{"type", "panel"},
                                                  {"size", {kSurvivalRight + kGap, kPanelHeight}},
                                                  {"bindings", json::array({visibleBinding("#is_survival_layout")})}}),
                           named("pad_wide", {{"type", "panel"},
                                              {"size", {kWideRight + kGap, kPanelHeight}},
                                              {"bindings", json::array({visibleBinding("(not #is_survival_layout)")})}}),
                           named("tk_fx_body", body)})}};
    return json::array({named("tk_fx_root", root)}).dump();
}

}
