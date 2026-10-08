#pragma once

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tsukuyomi::padkeys {
inline constexpr int A = 1, B = 2, X = 3, Y = 4;
inline constexpr int UP = 5, DOWN = 6, LEFT = 7, RIGHT = 8;
inline constexpr int LS = 9, RS = 10, LB = 11, RB = 12, VIEW = 13;
inline constexpr int RT = -99, LT = -100;
struct Button { std::string_view name; int value; };
inline constexpr std::array<Button, 15> kButtons{{
    {"A", A}, {"B", B}, {"X", X}, {"Y", Y}, {"UP", UP}, {"DOWN", DOWN},
    {"LEFT", LEFT}, {"RIGHT", RIGHT}, {"LS", LS}, {"RS", RS}, {"LB", LB},
    {"RB", RB}, {"VIEW", VIEW}, {"RT", RT}, {"LT", LT}}};
bool parseCombo(std::string_view text, std::vector<int>& out);
std::string comboName(std::span<const int> combo);
bool isPadValue(std::string_view value);
}
