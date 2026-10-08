#include "game/AppleSkinLayout.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <nlohmann/json.hpp>

namespace tsukuyomi::appleskin {
namespace {
using json = nlohmann::json;
constexpr const char* kSaturation[4][9] = {
    {"         ", "         ", "         ", "         ", "      a  ", "      a  ", "     b aa", "      b a", "      bb "},
    {"   c     ", "    d    ", "     a   ", "      a  ", "      a  ", "      a  ", "    bb aa", "      b a", "      bb "},
    {"  cc     ", "    d    ", "     a   ", "      a  ", "      a  ", "  b   a  ", "   bbb aa", "      b a", "      bb "},
    {"  cc     ", " d  d    ", "a    a   ", "a     a  ", " b    a  ", "  b   a  ", "   bbb aa", "      b a", "      bb "}};
json binding(const std::string& value, const char* target)
{
    return {{"binding_condition", "always"}, {"binding_name", value},
            {"binding_name_override", target}, {"binding_type", "global"}};
}
json viewVisible(const std::string& value, int code)
{
    return json::array({{{"binding_condition", "always"}, {"binding_name", value}, {"binding_type", "global"}},
        {{"binding_type", "view"}, {"source_property_name", "(" + value + " = " + std::to_string(code) + ")"},
         {"target_property_name", "#visible"}}});
}
json panel(const char* name, const json& controls)
{
    const std::string innerName = std::string(name) + "_on";
    return json::array({{{name, {{"anchor_from", "top_left"}, {"anchor_to", "top_left"},
        {"bindings", json::array({binding("#show_survival_ui", "#visible")})},
        {"controls", json::array({{{innerName, {{"bindings", json::array({binding("#tk_as_on", "#visible")})},
            {"controls", controls}, {"size", {"100%", "100%"}}, {"type", "panel"}}}}})},
        {"size", {180, 50}}, {"type", "panel"}}}}});
}
void icons(json& controls, char group, int index, int kinds, const std::array<std::string, 4>& textures,
           bool rawPath, bool flashing, int layer)
{
    const std::string value = "#tk_as_" + std::string(1, group) + std::to_string(index);
    for (int shake = 0; shake < 2; ++shake) {
        for (int kind = 1; kind <= kinds; ++kind) {
            const int code = kind + shake * 8;
            json bindings = viewVisible(value, code);
            if (flashing) bindings.push_back(binding("#tk_as_flash", "#alpha"));
            json body = {{"anchor_from", "top_left"}, {"anchor_to", "top_left"}, {"bindings", bindings},
                {"layer", layer}, {"offset", {group == 'p' ? -1 + 8 * index : 172 - 8 * index, 9 - shake}},
                {"size", {9, 9}}, {"texture", textures[kind - 1]}, {"type", "image"}};
            if (rawPath) body["texture_file_system"] = "RawPath";
            controls.push_back({{"tk_as_" + std::string(1, group) + std::to_string(index) + "_" + std::to_string(code), body}});
        }
    }
}
}
Layout layoutJson(const Textures& textures)
{
    json under = json::array();
    if (!textures.exhaustion.empty()) {
        under.push_back({{"tk_as_exhaustion", {{"anchor_from", "top_left"}, {"anchor_to", "top_left"},
            {"bindings", json::array({binding("#tk_as_exh", "#clip_ratio"), binding("#tk_as_exh_alpha", "#alpha")})},
            {"clip_direction", "right"}, {"clip_pixelperfect", false}, {"layer", 0}, {"offset", {100, 9}},
            {"size", {kExhaustionWidth, 9}}, {"texture", textures.exhaustion}, {"texture_file_system", "RawPath"}, {"type", "image"}}}});
    }
    json over = json::array();
    const std::array<std::string, 4> hunger = {"textures/ui/hunger_full", "textures/ui/hunger_half",
        "textures/ui/hunger_effect_full", "textures/ui/hunger_effect_half"};
    const std::array<std::string, 4> hearts = {"textures/ui/heart", "textures/ui/heart_half", "", ""};
    const bool satReady = std::all_of(textures.saturation.begin(), textures.saturation.end(), [](const auto& p) { return !p.empty(); });
    for (int i = 0; i < 10; ++i) {
        icons(over, 'h', i, 4, hunger, false, true, 3);
        if (satReady) {
            icons(over, 's', i, 4, textures.saturation, true, false, 4);
            icons(over, 'g', i, 4, textures.saturation, true, true, 5);
        }
        icons(over, 'p', i, 2, hearts, false, true, 3);
    }
    return {panel("tk_as_under", under).dump(), panel("tk_as_over", over).dump()};
}
std::vector<std::uint8_t> saturationPixels(int stage)
{
    std::vector<std::uint8_t> out(9 * 9 * 4);
    if (stage < 1 || stage > 4) return out;
    for (int r = 0; r < 9; ++r) {
        for (int c = 0; c < 9; ++c) {
            const char pixel = kSaturation[stage - 1][r][c];
            if (pixel == ' ') continue;
            const std::uint32_t rgb = pixel == 'a' ? 0xC2A100 : pixel == 'b' ? 0x9F8609 : pixel == 'c' ? 0xE9D262 : 0xDDC241;
            const int at = (r * 9 + c) * 4;
            out[at] = static_cast<std::uint8_t>(rgb >> 16); out[at + 1] = static_cast<std::uint8_t>(rgb >> 8);
            out[at + 2] = static_cast<std::uint8_t>(rgb); out[at + 3] = 0xFF;
        }
    }
    return out;
}
std::vector<std::uint8_t> exhaustionPixels()
{
    std::vector<std::uint8_t> out(kExhaustionWidth * 9 * 4);
    for (int y = 0; y < 9; ++y) {
        for (int x = 0; x < kExhaustionWidth; ++x) {
            if (((x + y) & 1) == 0) continue;
            const int at = (y * kExhaustionWidth + x) * 4;
            out[at] = out[at + 1] = out[at + 2] = 40; out[at + 3] = 0xFF;
        }
    }
    return out;
}
float exhaustionShown(float ratio)
{
    const float clamped = std::clamp(ratio, 0.0f, 1.0f);
    return static_cast<float>(static_cast<int>(clamped * kExhaustionWidth)) / kExhaustionWidth;
}
std::string textureFileName(const std::vector<std::uint8_t>& rgba)
{
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (const auto pixel : rgba) { hash ^= pixel; hash *= 0x100000001b3ull; }
    char name[32]{};
    std::snprintf(name, sizeof name, "%016llx.png", static_cast<unsigned long long>(hash));
    return name;
}
std::vector<TooltipIcon> tooltipHungerIcons(int nutrition, int bars)
{
    std::vector<TooltipIcon> icons;
    for (int k = 0; k < bars; ++k) {
        const int i = 2 * k;
        const int image = nutrition > i ? (nutrition - 1 == i ? kHungerHalf : kHungerFull) : kHungerNone;
        icons.push_back({(bars - 1 - k) * 9, image, false});
    }
    return icons;
}
std::vector<TooltipIcon> tooltipSaturationIcons(float gain, int bars)
{
    std::vector<TooltipIcon> icons;
    const float amount = std::fabs(gain);
    for (int k = 0; k < bars; ++k) {
        const float i = static_cast<float>(2 * k);
        const float e = (amount - i) / 2;
        const int image = e >= 1 ? 3 : e > 0.5f ? 2 : e > 0.25f ? 1 : e > 0 ? 0 : 4;
        icons.push_back({(bars - 1 - k) * 7, image, amount <= i});
    }
    return icons;
}
std::vector<PixelRun> tooltipSaturationRuns(int image, bool negative)
{
    constexpr const char* kIcons[5][7] = {
        {"  o    ", " oio   ", "oiiio  ", " oiia  ", "  ooiaa", "    bia", "    bb "},
        {"  o    ", " oio   ", "oiiia  ", " oiia  ", "  obiaa", "    bia", "    bb "},
        {"  o    ", " oid   ", "oiiia  ", " oiia  ", "  bbiaa", "    bia", "    bb "},
        {"  d    ", " did   ", "aiiia  ", " biia  ", "  bbiaa", "    bia", "    bb "},
        {"  o    ", " oio   ", "oiiio  ", " oiio  ", "  ooioo", "    oio", "    oo "}};
    const auto color = [negative](char pixel) -> std::uint32_t {
        switch (pixel) {
        case 'o': return 0x0A0A0A;
        case 'i': return 0x282828;
        case 'a': return negative ? 0xAB1200 : 0xC2A100;
        case 'b': return negative ? 0x9B1100 : 0x9F8609;
        default: return negative ? 0xE01700 : 0xDDC241;
        }
    };
    std::vector<PixelRun> runs;
    const auto& rows = kIcons[std::clamp(image, 0, 4)];
    for (int y = 0; y < 7; ++y) {
        int x = 0;
        while (x < 7) {
            if (rows[y][x] == ' ') { ++x; continue; }
            const char pixel = rows[y][x];
            const int start = x++;
            while (x < 7 && rows[y][x] == pixel) ++x;
            runs.push_back({start, y, x - start, color(pixel)});
        }
    }
    return runs;
}
}
