#pragma once

#include <cstdint>
#include <vector>

namespace tsukuyomi::boxmesh {

struct Cell {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
    std::uint8_t color = 0;
};

enum Dir : std::uint8_t {
    kDown = 0,
    kUp = 1,
    kNorth = 2,
    kSouth = 3,
    kWest = 4,
    kEast = 5,
};

struct Quad {
    std::uint8_t dir = 0;
    std::uint8_t color = 0;
    std::int32_t plane = 0;
    std::int32_t u0 = 0;
    std::int32_t v0 = 0;
    std::int32_t u1 = 0;
    std::int32_t v1 = 0;
};

std::vector<Quad> build(const std::vector<Cell>& cells, bool cullShared);

void corners(const Quad& quad, std::int32_t out[4][3]);

}
