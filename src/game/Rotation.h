#pragma once

#include <cstdint>

namespace tsukuyomi::rotation {

int normalize(int quarters);

void transformLocal(int quarters, int mirror, int x, int z, int& outX, int& outZ);
void inverseLocal(int quarters, int mirror, int x, int z, int& outX, int& outZ);

void rotateLocal(int quarters, std::int32_t x, std::int32_t z, std::int32_t& outX,
                 std::int32_t& outZ);

struct Footprint {
    std::int32_t x0 = 0;
    std::int32_t z0 = 0;
    std::int32_t x1 = 0;
    std::int32_t z1 = 0;
};
Footprint transformedFootprint(int quarters, int mirror, int originX, int originZ,
                               int sizeX, int sizeZ);

}
