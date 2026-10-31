#pragma once

#include <cstdint>

namespace tsukuyomi::fastplace {

enum class Restriction { Plane = 0, Face = 1, Column = 2, Line = 3, Layer = 4, Diagonal = 5 };
inline constexpr int kRestrictionCount = 6;

inline constexpr int kMinLayerHeight = -1000;
inline constexpr int kMaxLayerHeight = 1000;

bool allowed(Restriction mode, int layerHeight, const std::int32_t pos[3], int side,
             const std::int32_t first[3], int sideFirst);

void supportClick(const float inPlaced[3], int sideFirst, float out[3]);

}
