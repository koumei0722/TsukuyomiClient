#include "game/DebugScreenLayout.h"

#include <cstdio>
#include <cstring>
#include <nlohmann/json.hpp>

namespace tsukuyomi::dbgscreen {

namespace {

using json = nlohmann::json;

json named(const std::string& name, const json& body)
{
    return json{{name, body}};
}

void bindName(Column column, char kind, int row, char* out, std::size_t cap)
{
    if (out == nullptr || cap == 0) return;
    out[0] = '\0';
    if (row < 0 || row >= kRowsPerColumn) return;
    const int written = std::snprintf(out, cap, "#tk_f3_%c%c%02d",
                                      column == Column::Left ? 'l' : 'r', kind, row);
    if (written < 0 || static_cast<std::size_t>(written) >= cap) out[0] = '\0';
}

json line(Column column, int row)
{
    char visible[16]{};
    char blank[16]{};
    char text[16]{};
    visibleBindName(column, row, visible, sizeof visible);
    blankBindName(column, row, blank, sizeof blank);
    textBindName(column, row, text, sizeof text);
    const char side = column == Column::Left ? 'l' : 'r';
    const char* anchor = column == Column::Left ? "top_left" : "top_right";
    char name[16]{};
    char rowName[24]{};
    char blankName[24]{};
    char bgName[24]{};
    std::snprintf(name, sizeof name, "tk_f3_%c%02d", side, row);
    std::snprintf(rowName, sizeof rowName, "%s_row", name);
    std::snprintf(blankName, sizeof blankName, "%s_blank", name);
    std::snprintf(bgName, sizeof bgName, "%s_bg", name);

    const json label = {{"type", "label"}, {"text", text}, {"shadow", false},
                        {"color", "$tab_unchecked_text_color"}, {"layer", 1},
                        {"anchor_from", anchor}, {"anchor_to", anchor},
                        {"offset", {column == Column::Left ? 1 : -1, 0}},
                        {"bindings", json::array({{{"binding_name", text},
                                                   {"binding_condition", "always_when_visible"}}})}};
    const json background = {{"type", "image"}, {"texture", "textures/ui/White"},
                             {"color", "$coin_screen_callout_bevel_color"},
                             {"size", {"100%c + 2px", "100%c"}},
                             {"anchor_from", anchor}, {"anchor_to", anchor},
                             {"bindings", json::array({
                                 {{"binding_name", "#hud_text_background_alpha"},
                                  {"binding_name_override", "#alpha"}}
                             })},
                             {"controls", json::array({named(text + 1, label)})}};
    const json rowPanel = {{"type", "panel"}, {"size", {"100%", "100%c"}},
                           {"bindings", json::array({{{"binding_name", visible},
                                                      {"binding_name_override", "#visible"},
                                                      {"binding_condition", "always"}}})},
                           {"controls", json::array({named(bgName, background)})}};
    const json blankLabel = {{"type", "label"}, {"text", text}, {"shadow", false},
                             {"bindings", json::array({
                                 {{"binding_name", text}, {"binding_condition", "always_when_visible"}},
                                 {{"binding_name", blank}, {"binding_name_override", "#visible"},
                                  {"binding_condition", "always"}}
                             })}};
    const json outer = {{"type", "panel"}, {"size", {"100%", "100%c"}},
                        {"controls", json::array({named(rowName, rowPanel), named(blankName, blankLabel)})}};
    return named(name, outer);
}

json column(Column side)
{
    json controls = json::array();
    for (int row = 0; row < kRowsPerColumn; ++row) controls.push_back(line(side, row));
    const bool left = side == Column::Left;
    return {{"type", "stack_panel"}, {"orientation", "vertical"},
            {"anchor_from", left ? "top_left" : "top_right"},
            {"anchor_to", left ? "top_left" : "top_right"},
            {"offset", {left ? 2 : -2, 2}}, {"size", {"50%", "100%"}},
            {"controls", controls}};
}

struct Group {
    const char* name = nullptr;
    LineRef lines[32]{};
    int n = 0;
};

}

void visibleBindName(Column column, int row, char* out, std::size_t cap)
{
    bindName(column, 'v', row, out, cap);
}

void blankBindName(Column column, int row, char* out, std::size_t cap)
{
    bindName(column, 'b', row, out, cap);
}

void textBindName(Column column, int row, char* out, std::size_t cap)
{
    bindName(column, 't', row, out, cap);
}

void arrange(const Contribution* items, int count, Arranged& out)
{
    out.rows[0] = 0;
    out.rows[1] = 0;
    out.dropped[0] = 0;
    out.dropped[1] = 0;

    const auto push = [&out](int side, LineRef ref) {
        if (out.rows[side] < kRowsPerColumn) {
            out.line[side][out.rows[side]] = ref;
            ++out.rows[side];
        } else {
            ++out.dropped[side];
        }
    };

    LineRef regular[kElementCount * kMaxRowsPerElement];
    int regularCount = 0;
    Group groups[16];
    int groupCount = 0;

    if (items != nullptr) {
        for (int i = 0; i < count; ++i) {
            const Contribution& item = items[i];
            if (item.element < 0 || item.element >= kElementCount) continue;
            const Element& element = kElements[item.element];
            const std::int8_t contribution = static_cast<std::int8_t>(i);
            switch (element.port) {
            case Port::Priority: {
                for (int row = 0; row < item.rows; ++row) {
                    const int side = out.rows[0] > out.rows[1] ? 1 : 0;
                    push(side, LineRef{contribution, static_cast<std::int8_t>(row)});
                }
                break;
            }
            case Port::Regular: {
                for (int row = 0; row < item.rows; ++row) {
                    if (regularCount < kElementCount * kMaxRowsPerElement) {
                        regular[regularCount++] = LineRef{contribution, static_cast<std::int8_t>(row)};
                    }
                }
                break;
            }
            case Port::Group: {
                if (!item.groupPresent) break;
                Group* group = nullptr;
                for (int g = 0; g < groupCount; ++g) {
                    if (element.group != nullptr && groups[g].name != nullptr &&
                        std::strcmp(groups[g].name, element.group) == 0) {
                        group = &groups[g];
                        break;
                    }
                }
                if (group == nullptr && groupCount < 16) {
                    group = &groups[groupCount++];
                    group->name = element.group;
                }
                if (group != nullptr) {
                    for (int row = 0; row < item.rows; ++row) {
                        if (group->n < 32) {
                            group->lines[group->n++] = LineRef{contribution, static_cast<std::int8_t>(row)};
                        }
                    }
                }
                break;
            }
            }
        }
    }

    if (out.rows[0] > 0) push(0, LineRef{kBlankLine, 0});
    if (out.rows[1] > 0) push(1, LineRef{kBlankLine, 0});

    if (regularCount > 0) {
        const int half = (regularCount + 1) / 2;
        for (int i = 0; i < half; ++i) push(0, regular[i]);
        push(0, LineRef{kBlankLine, 0});
        for (int i = half; i < regularCount; ++i) push(1, regular[i]);
        if (half < regularCount) push(1, LineRef{kBlankLine, 0});
    }

    const int groupHalf = (groupCount + 1) / 2;
    for (int g = 0; g < groupCount; ++g) {
        if (groups[g].n == 0) continue;
        const int side = g < groupHalf ? 0 : 1;
        for (int i = 0; i < groups[g].n; ++i) push(side, groups[g].lines[i]);
        push(side, LineRef{kBlankLine, 0});
    }
}

std::string layoutJson()
{
    const json root = {{"type", "panel"}, {"size", {"100%", "100%"}}, {"layer", 1},
                       {"bindings", json::array({{{"binding_name", "#hud_visible"},
                                                   {"binding_name_override", "#visible"},
                                                   {"binding_type", "global"}}})},
                       {"controls", json::array({named("tk_f3_left", column(Column::Left)),
                                                 named("tk_f3_right", column(Column::Right))})}};
    return json::array({named("tk_f3", root)}).dump();
}

namespace {
json alwaysGlobalVisible(const std::string& bindingName)
{
    return {{"binding_name", bindingName},
            {"binding_name_override", "#visible"},
            {"binding_condition", "always"},
            {"binding_type", "global"}};
}
}

std::string hideBindingJson(const char* vanillaVisible, const char* hideBind)
{
    const json collectVanilla = {{"binding_name", vanillaVisible},
                                 {"binding_condition", "always"},
                                 {"binding_type", "global"}};
    const json collectHide = {{"binding_name", hideBind}, {"binding_condition", "always"}, {"binding_type", "global"}};
    const std::string expression = std::string("(") + vanillaVisible + " and (not " + hideBind + "))";
    const json apply = {{"binding_type", "view"},
                        {"source_property_name", expression},
                        {"target_property_name", "#visible"}};
    const json alpha = {{"binding_name", "#hud_text_background_alpha"}, {"binding_name_override", "#alpha"}};
    return json::array({collectVanilla, collectHide, apply, alpha}).dump();
}

std::string chatHideBindingJson()
{
    return json::array({alwaysGlobalVisible(std::string("(not ") + kBindChatBottom + ")")}).dump();
}

std::string chatJson()
{
    const json parentBindings = json::array(
        {{{"binding_name", "#hud_visible"}, {"binding_name_override", "#visible"}, {"binding_type", "global"}},
         {{"binding_name", "#hud_alpha"}, {"binding_name_override", "#alpha"}, {"binding_type", "global"}},
         {{"binding_name", "#hud_propagate_alpha"},
          {"binding_name_override", "#propagateAlpha"},
          {"binding_type", "global"}}});
    const json chat = {{"anchor_from", "bottom_left"},
                       {"anchor_to", "bottom_left"},
                       {"size", {"40%", "100%c"}},
                       {"offset", {0, kChatBottomOffsetY}},
                       {"bindings", json::array({alwaysGlobalVisible(kBindChatBottom)})}};
    const json root = {{"type", "panel"},
                       {"size", {"100%", "100%"}},
                       {"layer", 1},
                       {"bindings", parentBindings},
                       {"controls", json::array({named("tk_f3_chat_panel@hud.chat_panel", chat)})}};
    return json::array({named("tk_f3_chat", root)}).dump();
}

std::string pausedJson()
{
    const json label = {{"type", "label"},
                        {"text", "pauseScreen.title"},
                        {"shadow", true},
                        {"anchor_from", "top_middle"},
                        {"anchor_to", "top_middle"},
                        {"offset", {0, kGamePausedOffsetY}},
                        {"layer", 2}};
    const json root = {{"type", "panel"},
                       {"size", {"100%", "100%"}},
                       {"layer", 3},
                       {"bindings", json::array({alwaysGlobalVisible(kBindGamePaused)})},
                       {"controls", json::array({named("tk_f3_paused_label", label)})}};
    return json::array({named("tk_f3_paused", root)}).dump();
}

std::string pauseWheelHideJson()
{
    return json::array({alwaysGlobalVisible(std::string("(not ") + kBindPauseWheel + ")")}).dump();
}

}
