#pragma once

#include <cstdint>

namespace tsukuyomi::dbgsurface {

struct SurfaceConstants {
    float negInv128 = 0.0f;
    float one = 0.0f;
    float minus90 = 0.0f;
    float half = 0.0f;
    float minus10 = 0.0f;
    float three = 0.0f;
    float minus15 = 0.0f;
    float plus15 = 0.0f;
    float fifty = 0.0f;
    float table[2] = {};
    int topCell = 0;
};

bool surfaceConstantsLookRight(const SurfaceConstants& k);

void surfaceCells(int minY, int maxY, int& cellMin, int& count);

bool findTopSurface(const SurfaceConstants& k, float offset, float factor, int cellMin, int count, int& y);

inline constexpr int kGridXZ = 5;
inline constexpr int kGridY = 41;
inline constexpr int kGridCells = kGridXZ * kGridY * kGridXZ;

bool interpolateGrid(const float* grid, int cellMin, int x, int y, int z, float& out);

}
