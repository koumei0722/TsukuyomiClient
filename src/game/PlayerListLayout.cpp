#include "game/PlayerListLayout.h"

#include "core/Strings.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <nlohmann/json.hpp>

namespace tsukuyomi::playerlist {
namespace {

std::size_t charBytes(std::string_view text, std::size_t at)
{
    const auto c = static_cast<unsigned char>(text[at]);
    const std::size_t n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    return std::min(n, text.size() - at);
}

bool formatAt(std::string_view text, std::size_t at)
{
    return at + 1 < text.size() && text[at] == '\xC2' && text[at + 1] == '\xA7';
}

std::wstring sortKey(std::string_view name)
{
    std::string plain;
    for (std::size_t i = 0; i < name.size();) {
        if (formatAt(name, i)) {
            i += 2;
            if (i < name.size()) i += charBytes(name, i);
        } else {
            const auto n = charBytes(name, i);
            plain.append(name.substr(i, n));
            i += n;
        }
    }
    return toUtf16(plain);
}

int asciiWidth(char c)
{
    switch (c) {
    case ' ': case '"': case '(': case ')': case '*': case 'I': case '[': case ']':
    case 't': case '{': case '}': return 4;
    case '!': case '\'': case ',': case '.': case ':': case ';': case 'i': case '|': return 2;
    case '<': case '>': case 'f': case 'k': return 5;
    case '@': case '~': return 7;
    case '`': case 'l': return 3;
    default: return 6;
    }
}

using json = nlohmann::json;
json named(const std::string& name, const json& body) { return json{{name, body}}; }
json binding(const std::string& name, const char* target = nullptr)
{
    json result = {{"binding_condition", target != nullptr && std::string_view(target) == "#visible"
                                                ? "always" : "always_when_visible"},
                   {"binding_name", name}, {"binding_type", "global"}};
    if (target != nullptr) result["binding_name_override"] = target;
    return result;
}
std::string numbered(char kind, int index)
{
    char name[32]{};
    bindName(kind, index, name, sizeof(name));
    return name;
}
std::string control(const char* kind, int index)
{
    char name[32]{};
    std::snprintf(name, sizeof(name), "tk_tab_%s%02d", kind, index);
    return name;
}
json label(const std::string& text, const std::string& alpha = {})
{
    json binds = json::array({binding(text)});
    if (!alpha.empty()) binds.push_back(binding(alpha, "#alpha"));
    return {{"bindings", binds}, {"localize", false}, {"shadow", true},
            {"size", {"default", "default"}}, {"text", text}, {"type", "label"}};
}
json viewVisible(const std::string& value, const std::string& expression)
{
    return json::array({{{"binding_condition", "always"}, {"binding_name", value}, {"binding_type", "global"}},
                        {{"binding_type", "view"}, {"source_property_name", expression},
                         {"target_property_name", "#visible"}}});
}
json heart(const std::string& name, const char* texture, int i, const std::string& value,
           const std::string& expression, int layer)
{
    return named(name, {{"anchor_from", "top_left"}, {"anchor_to", "top_left"},
                        {"bindings", viewVisible(value, expression)}, {"layer", layer}, {"offset", {8 * i, 0}},
                        {"size", {9, 9}}, {"texture", texture}, {"type", "image"}});
}
json row(int index)
{
    const json sample = {{"anchor_from", "top_left"}, {"anchor_to", "top_left"},
        {"controls", json::array({
            named(control("hf", index), {{"bindings", json::array({binding("#tk_tab_hc", "#visible")})},
                                          {"size", {9, 8}}, {"type", "panel"}}),
            named(control("hn", index), label("#tk_tab_wn", "#tk_tab_ghost_alpha")),
            named(control("hp", index), {{"bindings", json::array({binding("#tk_tab_sp", "#visible")})},
                                          {"size", {4, 8}}, {"type", "panel"}}),
            named(control("hs", index), label("#tk_tab_ws", "#tk_tab_ghost_alpha")),
            named(control("hm", index), {{"bindings", json::array({binding("#tk_tab_hm", "#visible")})},
                                          {"size", {kHeartsColumn, 8}}, {"type", "panel"}})})},
        {"orientation", "horizontal"}, {"size", {"100%c", 8}}, {"type", "stack_panel"}};
    const json face = {{"anchor_from", "top_left"}, {"anchor_to", "top_left"},
        {"bindings", json::array({binding(numbered('f', index), "#texture"), binding(numbered('k', index), "#visible")})},
        {"size", {8, 8}}, {"texture", numbered('f', index)}, {"texture_file_system", "RawPath"}, {"type", "image"}};
    const json headSlot = {{"bindings", json::array({binding("#tk_tab_hc", "#visible")})},
        {"controls", json::array({named(control("fc", index), face)})}, {"size", {9, 8}}, {"type", "panel"}};
    json name = label(numbered('n', index), numbered('a', index));
    name["anchor_from"] = name["anchor_to"] = "top_left";
    name["offset"] = {0, -1};
    const json content = {{"anchor_from", "top_left"}, {"anchor_to", "top_left"},
        {"controls", json::array({named(control("hd", index), headSlot),
                                  named(control("nw", index), {{"controls", json::array({named(control("n", index), name)})},
                                                               {"size", {"100%c", 8}}, {"type", "panel"}})})},
        {"orientation", "horizontal"}, {"size", {"100%c", 8}}, {"type", "stack_panel"}};
    json score = label(numbered('s', index));
    score["anchor_from"] = score["anchor_to"] = "top_right";
    score["offset"] = {-12, -1};
    const std::string health = numbered('h', index);
    json hearts = json::array();
    for (int i = 0; i < kHearts; ++i) {
        const std::string n = std::to_string(i);
        hearts.push_back(heart(control("b", index) + "_" + n, "textures/ui/heart_background", i, health,
                               "(" + health + " > 0)", 1));
        hearts.push_back(heart(control("u", index) + "_" + n, "textures/ui/heart", i, health,
                               "(" + health + " > " + std::to_string(2 * i + 2) + ")", 2));
        hearts.push_back(heart(control("l", index) + "_" + n, "textures/ui/heart_half", i, health,
                               "(" + health + " = " + std::to_string(2 * i + 2) + ")", 2));
    }
    const json heartPanel = {{"anchor_from", "top_right"}, {"anchor_to", "top_right"}, {"controls", hearts},
                             {"offset", {-21, 0}}, {"size", {81, 9}}, {"type", "panel"}};
    return named(control("r", index), {
        {"bindings", json::array({binding(numbered('v', index), "#visible"),
                                   binding("#tk_tab_stripe_alpha", "#alpha")})},
        {"controls", json::array({named(control("h", index), sample), named(control("c", index), content),
                                   named(control("s", index), score), named(control("t", index), heartPanel)})},
        {"inherit_max_sibling_width", true}, {"size", {"100%cm + 13px", 8}},
        {"texture", "textures/ui/White"}, {"type", "image"}});
}
}

Arrangement arrange(int count)
{
    if (count <= 0) return {0, 0};
    count = std::min(count, kMaxPlayers);
    Arrangement a{count, 1};
    while (a.rows > kMaxRows) { ++a.columns; a.rows = (count + a.columns - 1) / a.columns; }
    return a;
}

int playerAt(const Arrangement& a, int column, int row, int count)
{
    if (a.rows <= 0 || a.rows > kMaxRows || a.columns <= 0 || a.columns > kMaxColumns
        || column < 0 || column >= a.columns || row < 0 || row >= a.rows) return -1;
    const int index = column * a.rows + row;
    return index < std::min(count, kMaxPlayers) ? index : -1;
}

void sortEntries(std::vector<Entry>& entries)
{
    struct Keyed { Entry entry; std::wstring key; };
    std::vector<Keyed> keyed;
    keyed.reserve(entries.size());
    for (auto& entry : entries) { auto key = sortKey(entry.name); keyed.push_back({std::move(entry), std::move(key)}); }
    std::stable_sort(keyed.begin(), keyed.end(), [](const Keyed& a, const Keyed& b) {
        if (a.entry.spectator != b.entry.spectator) return !a.entry.spectator;
        const int result = CompareStringOrdinal(a.key.data(), static_cast<int>(a.key.size()),
                                                b.key.data(), static_cast<int>(b.key.size()), TRUE);
        return result == CSTR_EQUAL ? a.entry.name < b.entry.name : result == CSTR_LESS_THAN;
    });
    for (std::size_t i = 0; i < keyed.size(); ++i) entries[i] = std::move(keyed[i].entry);
}

int estimateWidth(std::string_view utf8)
{
    int width = 0;
    for (std::size_t i = 0; i < utf8.size();) {
        if (formatAt(utf8, i)) {
            i += 2;
            if (i < utf8.size()) i += charBytes(utf8, i);
        } else {
            width += static_cast<unsigned char>(utf8[i]) < 0x80 ? asciiWidth(utf8[i]) : 9;
            i += charBytes(utf8, i);
        }
    }
    return width;
}

std::string displayName(const Entry& e)
{
    std::string result = e.spectator ? "\xC2\xA7o" : "";
    for (std::size_t i = 0; i < e.name.size();) {
        auto n = charBytes(e.name, i);
        if (formatAt(e.name, i) && i + 2 < e.name.size()) n = 2 + charBytes(e.name, i + 2);
        const bool percent = e.name[i + n - 1] == '%';
        if (result.size() + n + (percent ? 1 : 0) > 100) break;
        result.append(e.name, i, n);
        if (percent) result += '%';
        i += n;
    }
    return result;
}

std::string scoreText(int score) { return std::string("\xC2\xA7" "e") + std::to_string(score); }

void bindName(char kind, int index, char* out, std::size_t cap)
{
    if (out == nullptr || cap == 0) return;
    out[0] = '\0';
    if (index < 0 || index >= kMaxPlayers || std::string_view("nsavfkh").find(kind) == std::string_view::npos) return;
    const int n = std::snprintf(out, cap, "#tk_tab_%c%02d", kind, index);
    if (n < 0 || static_cast<std::size_t>(n) >= cap) out[0] = '\0';
}

int healthPoints(float current)
{
    if (!(current >= 0.0f) || current > 4096.0f) return -1;
    const int points = static_cast<int>(std::ceil(current));
    return points > 2 * kHearts ? 2 * kHearts : points;
}

std::string faceFileName(const std::vector<std::uint8_t>& rgba, int side)
{
    std::uint64_t h = 0xcbf29ce484222325ull;
    const auto mix = [&h](std::uint8_t v) { h ^= v; h *= 0x100000001b3ull; };
    for (int i = 0; i < 4; ++i) mix(static_cast<std::uint8_t>(side >> (8 * i)));
    for (std::uint8_t v : rgba) mix(v);
    char name[32]{};
    std::snprintf(name, sizeof(name), "%016llx.png", static_cast<unsigned long long>(h));
    return name;
}

std::string layoutJson()
{
    json columns = json::array();
    for (int c = 0; c < kMaxColumns; ++c) {
        const std::string suffix = std::to_string(c);
        if (c != 0) columns.push_back(named("tk_tab_g" + suffix,
            {{"bindings", json::array({binding("#tk_tab_g" + suffix + "v", "#visible")})},
             {"size", {5, 1}}, {"type", "panel"}}));
        json rows = json::array();
        for (int r = 0; r < kMaxRows; ++r) {
            const int index = c * kMaxRows + r;
            rows.push_back(row(index));
            rows.push_back(named(control("p", index),
                {{"bindings", json::array({binding(numbered('v', index), "#visible")})},
                 {"size", {0, 1}}, {"type", "panel"}}));
        }
        columns.push_back(named("tk_tab_c" + suffix,
            {{"bindings", json::array({binding("#tk_tab_c" + suffix + "v", "#visible")})},
             {"controls", rows}, {"orientation", "vertical"},
             {"size", {"100%cm", "100%c"}}, {"type", "stack_panel"}}));
    }
    const json cols = {{"anchor_from", "top_left"}, {"anchor_to", "top_left"}, {"controls", columns},
        {"offset", {1, 1}}, {"orientation", "horizontal"}, {"size", {"100%c", "100%cm"}}, {"type", "stack_panel"}};
    const json box = {{"anchor_from", "top_middle"}, {"anchor_to", "top_middle"},
        {"bindings", json::array({binding("#tk_tab_visible", "#visible"), binding("#tk_tab_box_alpha", "#alpha")})},
        {"controls", json::array({named("tk_tab_cols", cols)})}, {"offset", {0, 9}},
        {"size", {"100%c + 2px", "100%c + 1px"}}, {"texture", "textures/ui/Black"}, {"type", "image"}};
    const json root = {{"bindings", json::array({binding("#hud_visible", "#visible")})},
        {"controls", json::array({named("tk_tab_box", box)})}, {"layer", 30},
        {"size", {"100%", "100%"}}, {"type", "panel"}};
    return json::array({named("tk_tab", root)}).dump();
}
}
