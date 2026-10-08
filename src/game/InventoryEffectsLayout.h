#pragma once

#include <string>

namespace tsukuyomi::invfx {

inline constexpr int kMaxRows = 38;

inline constexpr int kPanelHeight = 166;
inline constexpr int kSurvivalRight = 88;
inline constexpr int kWideRight = 163;

inline constexpr const char* kBindOn = "#tk_fx_on";
std::string rowBinding(int row);
std::string yBinding(int row);
std::string nameBinding(int row);
std::string timeBinding(int row);
std::string iconBinding(int row);
std::string bgBinding(int row);
std::string tipBinding(int row);

inline constexpr const char* kIgnoreVariable = "$tk_fx_ignored";

std::string layoutJson();

}
