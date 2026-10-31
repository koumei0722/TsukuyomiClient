#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <atomic>

namespace tsukuyomi::schematic {
struct Box;
struct GridSpec {
    bool enabled = false;
    int size[3] = {0, 0, 0};
    int repeatNeg[3] = {0, 0, 0};
    int repeatPos[3] = {0, 0, 0};
    bool operator==(const GridSpec&) const = default;
};
struct CopyRange {
    std::array<std::int64_t, 3> first{}, last{}, step{};
    bool empty() const { return first[0] > last[0] || first[1] > last[1] || first[2] > last[2]; }
    bool operator==(const CopyRange&) const = default;
    std::array<int, 3> origin(const std::array<int, 3>& base, const std::array<std::int64_t, 3>& index) const;
    bool count(std::size_t& out) const;
};
CopyRange copiesInBox(const GridSpec& grid, const Box& baseBox, const std::array<int, 3>& origin, const Box& range);
CopyRange copiesOverlapping(const CopyRange& copies, const Box& baseBox, const Box& range);
using GridEdge = std::array<std::int64_t,6>;
bool gridOutline(const Box& baseBox, const CopyRange& copies, std::vector<GridEdge>& out, const std::atomic<bool>* cancel = nullptr);
Box copiesBox(const Box& baseBox, const CopyRange& copies);
}
