#pragma once

#include <cstdint>

namespace tsukuyomi::rotation {

int normalize(int quarters);

void rotateLocal(int quarters, std::int32_t x, std::int32_t z, std::int32_t& outX,
                 std::int32_t& outZ);

struct Footprint {
    std::int32_t x0 = 0;
    std::int32_t z0 = 0;
    std::int32_t x1 = 0;
    std::int32_t z1 = 0;
};
Footprint rotatedFootprint(int quarters, std::int32_t originX, std::int32_t originZ,
                           std::int32_t sizeX, std::int32_t sizeZ);

}
