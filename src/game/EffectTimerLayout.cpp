#include "game/EffectTimerLayout.h"

#include <nlohmann/json.hpp>

#include <cmath>

namespace tsukuyomi::efxtimer {

namespace {

using json = nlohmann::json;

json named(const std::string& name, const json& body)
{
    return json{{name, body}};
}

std::string indexed(const char* prefix, int row)
{
    return std::string(prefix) + std::to_string(row);
}

json always(const std::string& name, const char* overrideName)
{
    return {{"binding_name", name}, {"binding_name_override", overrideName}, {"binding_condition", "always"}};
}

json row(int index)
{
    const std::string text = textBinding(index);
    const json label = {{"type", "label"},
                        {"text", text},
                        {"localize", false},
                        {"shadow", true},
                        {"size", {"default", "default"}},
                        {"anchor_from", "top_left"},
                        {"anchor_to", "right_middle"},
                        {"bindings", json::array({
                            {{"binding_name", text}, {"binding_condition", "always_when_visible"}, {"binding_type", "global"}}
                        })}};
    return {{"type", "panel"},
            {"size", {1, 1}},
            {"anchor_from", "top_left"},
            {"anchor_to", "top_left"},
            {"use_anchored_offset", true},
            {"visible", false},
            {"bindings", json::array({always(visibleBinding(index), "#visible"),
                                      always(xBinding(index), "#anchored_offset_value_x"),
                                      always(yBinding(index), "#anchored_offset_value_y")})},
            {"controls", json::array({named("tk_efx_l", label)})}};
}

}

std::string visibleBinding(int row) { return indexed("#tk_efx_v", row); }
std::string xBinding(int row) { return indexed("#tk_efx_x", row); }
std::string yBinding(int row) { return indexed("#tk_efx_y", row); }
std::string textBinding(int row) { return indexed("#tk_efx_t", row); }

std::string layoutJson()
{
    json rows = json::array();
    for (int r = 0; r < kRows; ++r) {
        rows.push_back(named(indexed("tk_efx_r", r), row(r)));
    }
    const json root = {{"type", "panel"},
                       {"size", {1, 1}},
                       {"anchor_from", "top_left"},
                       {"anchor_to", "top_left"},
                       {"layer", 5},
                       {"controls", rows}};
    return json::array({named("tk_efx_root", root)}).dump();
}

std::vector<int> columnsOf(const std::vector<float>& tops)
{
    std::vector<int> columns;
    columns.reserve(tops.size());
    int column = 0;
    for (std::size_t i = 0; i < tops.size(); ++i) {
        if (i > 0 && !(tops[i] > tops[i - 1])) {
            ++column;
        }
        columns.push_back(column);
    }
    return columns;
}

float columnShift(int column, int textWidth)
{
    if (column <= 0) {
        return 0.0f;
    }
    return static_cast<float>(column) * (static_cast<float>(textWidth > 0 ? textWidth : 0) + kGap + static_cast<float>(kColumnMargin));
}

bool anchorOf(const Rect& background, float scale, float ownerX, float ownerY, float& x, float& y)
{
    if (!(scale > 0.0f) || !std::isfinite(scale) || !(background.x1 > background.x0) || !(background.y1 > background.y0)) {
        return false;
    }
    x = background.x0 / scale - ownerX - kGap;
    y = (background.y0 + background.y1) * 0.5f / scale - ownerY;
    return std::isfinite(x) && std::isfinite(y);
}

}
