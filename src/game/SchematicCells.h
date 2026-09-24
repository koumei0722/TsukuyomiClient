#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "game/Structure.h"

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

std::uint64_t packCell(std::int32_t x, std::int32_t y, std::int32_t z);

class CellIndex {
public:
    static constexpr std::size_t kNone = static_cast<std::size_t>(-1);

    void reset(const Box& region);
    bool insert(std::int32_t x, std::int32_t y, std::int32_t z, std::size_t index);
    std::size_t find(std::int32_t x, std::int32_t y, std::int32_t z) const;
    std::size_t size() const { return m_count; }
    bool dense() const { return m_dense; }
    void clear();
    void swap(CellIndex& other) noexcept;

private:
    bool slotOf(std::int32_t x, std::int32_t y, std::int32_t z, std::size_t& out) const;

    Box m_box;
    std::uint64_t m_sy = 0;
    std::uint64_t m_sz = 0;
    bool m_dense = false;
    std::vector<std::uint32_t> m_slots;
    std::unordered_map<std::uint64_t, std::uint32_t> m_sparse;
    std::size_t m_count = 0;
};

struct Loaded {
    std::shared_ptr<const structure::Structure> data;
    const char* why = nullptr;
    std::int32_t sizeX = 0;
    std::int32_t sizeY = 0;
    std::int32_t sizeZ = 0;
    std::size_t solid = 0;
    std::vector<std::pair<std::string, std::size_t>> materials;
    bool ok() const { return data != nullptr; }
};
Loaded loadBlueprint(const std::filesystem::path& path);
Loaded loadBlueprintBytes(const char* bytes, std::size_t size);

struct Placement {
    std::shared_ptr<const structure::Structure> data;
    std::size_t paletteBase = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
    int rotation = 0;
};

std::size_t assignPaletteBases(std::vector<Placement>& placements);

struct LayoutKey {
    const void* data = nullptr;
    std::size_t paletteBase = 0;
    int x = 0;
    int y = 0;
    int z = 0;
    int rotation = 0;
    int sizeX = 0;
    int sizeY = 0;
    int sizeZ = 0;
    bool operator==(const LayoutKey&) const = default;
};
std::vector<LayoutKey> layoutOf(const std::vector<Placement>& placements);

struct CellSet {
    std::vector<Cell> cells;
    CellIndex cellAt;
    std::vector<Box> boxes;
    Box region;
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> chunkCells;
    std::size_t overlapped = 0;
    std::vector<std::shared_ptr<const structure::Structure>> keep;
    std::vector<LayoutKey> key;
};

Box unionBox(const std::vector<Box>& boxes);

bool buildCells(const std::vector<Placement>& placements, CellSet& out,
                const std::atomic<bool>* cancel = nullptr);

}
