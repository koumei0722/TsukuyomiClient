#pragma once

#include <cstddef>
#include <vector>

namespace tsukuyomi::lightlevel {

enum class Spawn { Never, NightOnly, Always };
Spawn classify(int blockLight, int skyLight);

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
};
inline constexpr int kGlyphWidth = 3;
inline constexpr int kGlyphHeight = 5;
const std::vector<Rect>& digitRects(int digit);
std::vector<Rect> numberRects(int value, int* width = nullptr);

struct Quad {
    double p[4][3];
};
std::vector<Quad> numberQuads(int value, int x, int y, int z, int facing, double pixel, double lift);
Quad faceQuad(int x, int y, int z, double lift, double inset);

}
