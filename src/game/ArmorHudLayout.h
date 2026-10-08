#pragma once

#include <string>

namespace tsukuyomi::armorhud {

inline constexpr int kItems = 6;
inline constexpr int kMainhand = 4, kOffhand = 5;
inline constexpr char kCollection[] = "tk_armor_items";
static_assert(sizeof(kCollection) - 1 <= 15);

inline constexpr int kPitch = 18;
inline constexpr int kTextWidth = 26;
inline constexpr int kTextHeight = 10;
inline constexpr int kTextGap = 2;

enum Orientation { kHorizontal = 0, kVertical = 1, kOrientationCount = 2 };
enum TextMode { kNoText = 0, kNumber = 1, kPercent = 2, kTextModeCount = 3 };

inline constexpr int kForms = 4;
int formOf(int orientation, int textMode);
struct FormSize {
    int width;
    int height;
};
FormSize formSize(int form);

inline constexpr int kDefaultOrientation = kVertical;
inline constexpr int kDefaultAnchor = 3;
inline constexpr bool kDefaultBar = true;
inline constexpr int kDefaultText = kNoText;
inline constexpr int kDefaultOffsetX = 0;
inline constexpr int kDefaultOffsetY = 0;
inline constexpr int kOffsetMin = -400;
inline constexpr int kOffsetMax = 400;

int clampOrientation(int value);
int clampTextMode(int value);
int clampAnchor(int value);
int clampOffset(int value);
const wchar_t* orientationLabel(int value);
const wchar_t* textModeLabel(int value);

std::string durabilityText(int mode, int maxDamage, int damage);

inline constexpr const char* kBindVisible = "#tk_ahud_visible";
inline constexpr const char* kBindVMid = "#tk_ahud_v_mid";
inline constexpr const char* kBindVBottom = "#tk_ahud_v_bottom";
inline constexpr const char* kBindHMid = "#tk_ahud_h_mid";
inline constexpr const char* kBindHRight = "#tk_ahud_h_right";
inline constexpr const char* kBindBar = "#tk_ahud_bar";
std::string formVisibleBinding(int form);
std::string formXBinding(int form);
std::string formYBinding(int form);
std::string textBinding(int item);

std::string layoutJson();

}
