#pragma once

#include <string>
#include <vector>

namespace tsukuyomi::efxtimer {

inline constexpr int kFirstId = 1;
inline constexpr int kMaxId = 0x25;
inline constexpr int kRows = kMaxId - kFirstId + 1;
inline constexpr int rowOf(int id) { return id - kFirstId; }

inline constexpr int kBoolSlot = 427;
inline constexpr int kXSlot = 74;
inline constexpr int kYSlot = kXSlot + kRows;
inline constexpr int kTextSlot = 346;

inline constexpr float kGap = 1.0f;

std::string visibleBinding(int row);
std::string xBinding(int row);
std::string yBinding(int row);
std::string textBinding(int row);

std::string layoutJson();

struct Rect {
    float x0 = 0, x1 = 0, y0 = 0, y1 = 0;
};
bool anchorOf(const Rect& background, float scale, float ownerX, float ownerY, float& x, float& y);

inline constexpr int kColumnMargin = 2;

std::vector<int> columnsOf(const std::vector<float>& tops);

float columnShift(int column, int textWidth);

inline constexpr int secondsKey(int ticks) { return ticks < 0 ? -1 : ticks / 20; }

}
