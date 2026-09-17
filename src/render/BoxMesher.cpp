#include "render/BoxMesher.h"

#include <algorithm>
#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace tsukuyomi::boxmesh {
namespace {

struct Key3 {
    std::int32_t a = 0;
    std::int32_t b = 0;
    std::int32_t c = 0;
    bool operator==(const Key3& other) const noexcept
    {
        return a == other.a && b == other.b && c == other.c;
    }
};

struct Key3Hash {
    std::size_t operator()(const Key3& key) const noexcept
    {
        std::uint64_t h = static_cast<std::uint32_t>(key.a);
        h = h * 0x9E3779B97F4A7C15ULL ^ static_cast<std::uint32_t>(key.b);
        h = h * 0x9E3779B97F4A7C15ULL ^ static_cast<std::uint32_t>(key.c);
        return static_cast<std::size_t>(h ^ (h >> 29));
    }
};

struct Axis {
    std::int32_t dx;
    std::int32_t dy;
    std::int32_t dz;
};
constexpr Axis kNeighbor[6] = {
    {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0},
};

void planeOf(std::uint8_t dir, const Cell& cell, std::int32_t& plane, std::int32_t& u,
             std::int32_t& v)
{
    switch (dir) {
    case kDown:
    case kUp:
        plane = cell.y + (dir == kUp ? 1 : 0);
        u = cell.x;
        v = cell.z;
        break;
    case kNorth:
    case kSouth:
        plane = cell.z + (dir == kSouth ? 1 : 0);
        u = cell.x;
        v = cell.y;
        break;
    default:
        plane = cell.x + (dir == kEast ? 1 : 0);
        u = cell.z;
        v = cell.y;
        break;
    }
}

}

std::vector<Quad> build(const std::vector<Cell>& cells, bool cullShared)
{
    std::unordered_map<Key3, std::uint8_t, Key3Hash> occupied;
    occupied.reserve(cells.size() * 2 + 1);
    std::vector<Cell> unique;
    unique.reserve(cells.size());
    for (const Cell& cell : cells) {
        if (cell.color == 0) {
            continue;
        }
        if (occupied.emplace(Key3{cell.x, cell.y, cell.z}, cell.color).second) {
            unique.push_back(cell);
        }
    }

    std::unordered_map<Key3, std::vector<std::pair<std::int32_t, std::int32_t>>, Key3Hash>
        groups;
    for (const Cell& cell : unique) {
        for (std::uint8_t dir = 0; dir < 6; ++dir) {
            if (cullShared) {
                const Axis& n = kNeighbor[dir];
                if (occupied.count(Key3{cell.x + n.dx, cell.y + n.dy, cell.z + n.dz}) != 0) {
                    continue;
                }
            }
            std::int32_t plane = 0;
            std::int32_t u = 0;
            std::int32_t v = 0;
            planeOf(dir, cell, plane, u, v);
            groups[Key3{static_cast<std::int32_t>(dir) * 4 + cell.color, plane, 0}].emplace_back(u, v);
        }
    }

    std::vector<Quad> out;
    std::unordered_set<Key3, Key3Hash> left;
    for (auto& [key, faces] : groups) {
        std::sort(faces.begin(), faces.end(), [](const auto& p, const auto& q) {
            return p.second != q.second ? p.second < q.second : p.first < q.first;
        });
        left.clear();
        left.reserve(faces.size() * 2 + 1);
        for (const auto& [u, v] : faces) {
            left.insert(Key3{u, v, 0});
        }
        const auto dir = static_cast<std::uint8_t>(key.a / 4);
        const auto color = static_cast<std::uint8_t>(key.a % 4);
        for (const auto& [u, v] : faces) {
            if (left.count(Key3{u, v, 0}) == 0) {
                continue;
            }
            std::int32_t width = 1;
            while (left.count(Key3{u + width, v, 0}) != 0) {
                ++width;
            }
            std::int32_t height = 1;
            for (;;) {
                bool full = true;
                for (std::int32_t k = 0; k < width && full; ++k) {
                    full = left.count(Key3{u + k, v + height, 0}) != 0;
                }
                if (!full) {
                    break;
                }
                ++height;
            }
            for (std::int32_t j = 0; j < height; ++j) {
                for (std::int32_t k = 0; k < width; ++k) {
                    left.erase(Key3{u + k, v + j, 0});
                }
            }
            Quad quad;
            quad.dir = dir;
            quad.color = color;
            quad.plane = key.b;
            quad.u0 = u;
            quad.v0 = v;
            quad.u1 = u + width;
            quad.v1 = v + height;
            out.push_back(quad);
        }
    }
    std::sort(out.begin(), out.end(), [](const Quad& p, const Quad& q) {
        if (p.color != q.color) {
            return p.color < q.color;
        }
        if (p.dir != q.dir) {
            return p.dir < q.dir;
        }
        if (p.plane != q.plane) {
            return p.plane < q.plane;
        }
        if (p.v0 != q.v0) {
            return p.v0 < q.v0;
        }
        return p.u0 < q.u0;
    });
    return out;
}

void corners(const Quad& quad, std::int32_t out[4][3])
{
    std::int32_t uv[4][2] = {};
    const bool uFirst = (quad.dir == kDown || quad.dir == kSouth || quad.dir == kWest);
    if (uFirst) {
        const std::int32_t pts[4][2] = {
            {quad.u0, quad.v0}, {quad.u1, quad.v0}, {quad.u1, quad.v1}, {quad.u0, quad.v1}};
        std::copy(&pts[0][0], &pts[0][0] + 8, &uv[0][0]);
    } else {
        const std::int32_t pts[4][2] = {
            {quad.u0, quad.v0}, {quad.u0, quad.v1}, {quad.u1, quad.v1}, {quad.u1, quad.v0}};
        std::copy(&pts[0][0], &pts[0][0] + 8, &uv[0][0]);
    }
    for (int i = 0; i < 4; ++i) {
        const std::int32_t u = uv[i][0];
        const std::int32_t v = uv[i][1];
        switch (quad.dir) {
        case kDown:
        case kUp:
            out[i][0] = u;
            out[i][1] = quad.plane;
            out[i][2] = v;
            break;
        case kNorth:
        case kSouth:
            out[i][0] = u;
            out[i][1] = v;
            out[i][2] = quad.plane;
            break;
        default:
            out[i][0] = quad.plane;
            out[i][1] = v;
            out[i][2] = u;
            break;
        }
    }
}

}
