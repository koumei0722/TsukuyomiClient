#include "DebugScreenSurface.h"

namespace tsukuyomi::dbgsurface {

bool surfaceConstantsLookRight(const SurfaceConstants& k)
{
    return k.negInv128 == -0.0078125f && k.one == 1.0f && k.minus90 == -90.0f && k.half == 0.5f
           && k.minus10 == -10.0f && k.three == 3.0f && k.minus15 == -15.0f && k.plus15 == 15.0f
           && k.fifty == 50.0f && k.table[0] == 1.0f && k.table[1] == 4.0f && k.topCell == 39;
}

void surfaceCells(int minY, int maxY, int& cellMin, int& count)
{
    const int c = minY >> 31;
    const int r8 = c - minY;
    const int r9 = ((r8 < 0) ? r8 + 7 : r8) >> 3;
    cellMin = c - r9;
    const int s = maxY >> 31;
    const int r10 = s - maxY;
    const int e = ((r10 < 0) ? r10 + 7 : r10) >> 3;
    count = static_cast<int>(static_cast<unsigned>(minY) >> 31) + s + r9 - e - 8;
}

bool findTopSurface(const SurfaceConstants& k, float offset, float factor, int cellMin, int count, int& y)
{
    for (int i = k.topCell; i >= 0; --i) {
        const int cy = (i + cellMin) * 8;
        float d = static_cast<float>(cy) * k.negInv128 + k.one + offset;
        d *= factor;
        d *= (d > 0.0f) ? k.table[1] : k.table[0];
        d += k.minus90;
        const float tTop = static_cast<float>(count - i) * k.half;
        float top = k.minus10;
        if (!(0.0f > tTop)) {
            top = (tTop <= k.one) ? tTop * (d - k.minus10) + k.minus10 : d;
        }
        const float tBot = static_cast<float>(i) / k.three;
        const float v = (k.one < tBot) ? top : (top + k.minus15) * tBot + k.plus15;
        if (v > k.fifty) {
            y = cy;
            return true;
        }
    }
    return false;
}

bool interpolateGrid(const float* grid, int cellMin, int x, int y, int z, float& out)
{
    const int lx = x & 15;
    const int lz = z & 15;
    const int ry = y - cellMin * 8;
    if (ry < 0) {
        return false;
    }
    const int cx = lx >> 2;
    const int cz = lz >> 2;
    const int cy = ry >> 3;
    if (cy + 1 >= kGridY) {
        return false;
    }
    const float tx = static_cast<float>(lx & 3) / 4.0f;
    const float tz = static_cast<float>(lz & 3) / 4.0f;
    const float ty = static_cast<float>(ry & 7) / 8.0f;
    const auto at = [grid](int ix, int iy, int iz) { return grid[(ix * kGridXZ + iz) * kGridY + iy]; };
    const auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    const float c00 = lerp(at(cx, cy, cz), at(cx + 1, cy, cz), tx);
    const float c10 = lerp(at(cx, cy + 1, cz), at(cx + 1, cy + 1, cz), tx);
    const float c01 = lerp(at(cx, cy, cz + 1), at(cx + 1, cy, cz + 1), tx);
    const float c11 = lerp(at(cx, cy + 1, cz + 1), at(cx + 1, cy + 1, cz + 1), tx);
    out = lerp(lerp(c00, c10, ty), lerp(c01, c11, ty), tz);
    return true;
}

}
