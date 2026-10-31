#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <functional>
#include <utility>
#include <vector>
#include <unordered_map>
#include <limits>

#include "game/Structure.h"
#include "game/SchematicGrid.h"

namespace tsukuyomi::schematic {

struct Cell {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
    std::size_t entry = 0;
    std::int32_t entry2 = -1;
    int owner = -1;
    const std::string* name = nullptr;
};
inline constexpr std::size_t kAirCell = static_cast<std::size_t>(-1);

struct Box {
    std::int32_t x0 = 0, y0 = 0, z0 = 0;
    std::int32_t x1 = 0, y1 = 0, z1 = 0;
};

struct CellPos {
    std::int32_t x = 0, y = 0, z = 0;
    bool operator==(const CellPos&) const = default;
};
struct CellPosHash {
    std::size_t operator()(const CellPos& pos) const noexcept;
};

std::vector<CellPos> regionChunks(const std::vector<Box>& boxes, std::size_t limit,
                                  bool& truncated);

struct Loaded {
    std::shared_ptr<const structure::Structure> data;
    const char* why = nullptr;
    std::int32_t sizeX = 0;
    std::int32_t sizeY = 0;
    std::int32_t sizeZ = 0;
    std::size_t solid = 0;
    std::vector<std::size_t> paletteCounts;
    std::uint64_t requiredMemory = 0, availableMemory = 0;
    bool ok() const { return data != nullptr; }
};
Loaded loadBlueprint(const std::filesystem::path& path);

struct Placement {
    std::shared_ptr<const structure::Structure> data;
    std::size_t paletteBase = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
    int rotation = 0;
    int mirror = 0;
    int owner = -1;
    std::uint64_t placementId = 0;
    std::wstring name;
    CopyRange copies;
};

std::size_t assignPaletteBases(std::vector<Placement>& placements);

struct LayoutKey {
    const void* data = nullptr;
    std::size_t paletteBase = 0;
    int x = 0;
    int y = 0;
    int z = 0;
    int rotation = 0;
    int mirror = 0;
    int owner = -1;
    std::uint64_t placementId = 0;
    int sizeX = 0;
    int sizeY = 0;
    int sizeZ = 0;
    CopyRange copies;
    bool operator==(const LayoutKey&) const = default;
};
std::vector<LayoutKey> layoutOf(const std::vector<Placement>& placements);

struct CellSet {
    static constexpr std::size_t kNone = static_cast<std::size_t>(-1);
    struct Source {
        Placement placed;
        Box box, baseBox;
        std::size_t offset = 0, copyCount = 1;
        int owner = -1;
        std::vector<std::size_t> earlier;
        std::vector<std::uint8_t> air;
    };
    std::vector<Source> sources;
    std::unordered_map<CellPos, std::vector<std::uint32_t>, CellPosHash> sourcesByChunk;
    std::size_t total = 0;
    std::vector<Box> boxes;
    std::vector<CellPos> chunks;
    const char* why = nullptr;
    Box region;
    std::size_t overlapped = 0;
    std::vector<std::shared_ptr<const structure::Structure>> keep;
    std::vector<LayoutKey> key;
    std::size_t size() const { return total; }
    bool empty() const { return total == 0; }
    bool cellAt(std::size_t at, Cell& out) const;
    std::size_t find(std::int32_t x, std::int32_t y, std::int32_t z) const;
    bool contains(std::int32_t x, std::int32_t y, std::int32_t z) const;
    template<class F>
    std::size_t forEachInChunk(std::int32_t cx, std::int32_t cy, std::int32_t cz, F&& f) const
    {
        constexpr auto low = (std::numeric_limits<std::int32_t>::min)() / 16;
        constexpr auto high = (std::numeric_limits<std::int32_t>::max)() / 16;
        if (cx < low || cx > high || cy < low || cy > high || cz < low || cz > high) return 0;
        std::size_t count = 0;
        const auto found = sourcesByChunk.find({cx*16,cy*16,cz*16});
        if (found == sourcesByChunk.end()) return 0;
        Box walk{INT32_MAX, INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN, INT32_MIN};
        for (const auto which : found->second) {
            const Box b = chunkIntersection(sources[which].box, cx, cy, cz);
            if (b.x0 >= b.x1 || b.y0 >= b.y1 || b.z0 >= b.z1) continue;
            walk.x0 = (std::min)(walk.x0, b.x0); walk.y0 = (std::min)(walk.y0, b.y0); walk.z0 = (std::min)(walk.z0, b.z0);
            walk.x1 = (std::max)(walk.x1, b.x1); walk.y1 = (std::max)(walk.y1, b.y1); walk.z1 = (std::max)(walk.z1, b.z1);
        }
        for (auto x = walk.x0; x < walk.x1; ++x)
            for (auto y = walk.y0; y < walk.y1; ++y)
                for (auto z = walk.z0; z < walk.z1; ++z) {
                    const auto at = find(x, y, z);
                    if (at != kNone) { f(at); ++count; }
                }
        return count;
    }
    friend bool buildCells(const std::vector<Placement>& placements, CellSet& out,
                           const std::atomic<bool>* cancel);
private:
    static Box chunkIntersection(const Box& b, std::int32_t cx, std::int32_t cy, std::int32_t cz);
    static std::size_t localAt(const Source& source, std::int32_t x, std::int32_t y, std::int32_t z);
};

Box unionBox(const std::vector<Box>& boxes);

bool buildCells(const std::vector<Placement>& placements, CellSet& out,
                const std::atomic<bool>* cancel = nullptr);

}
