#pragma once

#include <string>

namespace tsukuyomi::invhud {

inline constexpr int kColumns = 9, kRows = 3, kCells = 27, kPitch = 18;
inline constexpr int kWidth = kColumns * kPitch;
inline constexpr int kHeight = kRows * kPitch;
inline constexpr int kFirstSlot = 9;
inline constexpr int kAnchorCount = 9;
inline constexpr int kDefaultAnchor = 8;
inline constexpr int kDefaultOffsetX = 0;
inline constexpr int kDefaultOffsetY = 0;
inline constexpr int kOffsetMin = -400;
inline constexpr int kOffsetMax = 400;
inline constexpr int kOffhandSlotWidth = 20;
inline constexpr int kOffhandWidth = 1 + kOffhandSlotWidth + 1;
inline constexpr int kOffhandHeight = 22;
inline constexpr int kOffhandGap = 7;
inline constexpr const char* kOffhandCondition = "always_when_visible";

inline constexpr const char* kBindVisible = "#tk_invhud_visible";
inline constexpr const char* kBindVMid = "#tk_invhud_v_mid";
inline constexpr const char* kBindVBottom = "#tk_invhud_v_bottom";
inline constexpr const char* kBindHMid = "#tk_invhud_h_mid";
inline constexpr const char* kBindHRight = "#tk_invhud_h_right";
inline constexpr const char* kBindX = "#tk_invhud_x";
inline constexpr const char* kBindY = "#tk_invhud_y";
inline constexpr const char* kBindOffhand = "#tk_invhud_offhand";

int clampAnchor(int anchor);
int clampOffset(int value);
const wchar_t* anchorLabel(int anchor);

struct Placement {
    bool vMid;
    bool vBottom;
    bool hMid;
    bool hRight;
};

Placement placementOf(int anchor);
std::string layoutJson();
std::string offhandJson();

}
