#include "game/SchematicCells.h"

#include <algorithm>
#include <array>
#include <map>
#include <set>

#include "game/Rotation.h"
#include "game/SchematicLimits.h"

namespace tsukuyomi::schematic {

namespace {

const std::string kAirName{"minecraft:air"};

Loaded finishLoad(structure::LoadResult&& got)
{
    Loaded out;
    out.sizeX = got.value.sizeX;
    out.sizeY = got.value.sizeY;
    out.sizeZ = got.value.sizeZ;
    if (!got.ok()) {
        out.why = got.why;
        return out;
    }
    std::size_t solid = 0;
    std::vector<std::size_t> perEntry(got.value.palette.size(), 0);
    for (const std::int32_t entry : got.value.blocks) {
        if (entry >= 0 && static_cast<std::size_t>(entry) < got.value.palette.size()
            && got.value.palette[static_cast<std::size_t>(entry)].name != kAirName) {
            ++solid;
            ++perEntry[static_cast<std::size_t>(entry)];
        }
    }
    std::map<std::string, std::size_t> byName;
    for (std::size_t e = 0; e < perEntry.size(); ++e) {
        if (perEntry[e] != 0) {
            byName[got.value.palette[e].name] += perEntry[e];
        }
    }
    std::vector<std::pair<std::string, std::size_t>> materials(byName.begin(), byName.end());
    std::sort(materials.begin(), materials.end(), [](const auto& a, const auto& b) {
        return (a.second != b.second) ? (a.second > b.second) : (a.first < b.first);
    });
    out.solid = solid;
    out.materials = std::move(materials);
    out.data = std::make_shared<const structure::Structure>(std::move(got.value));
    return out;
}

}

std::uint64_t packCell(std::int32_t x, std::int32_t y, std::int32_t z)
{
    const std::uint64_t ux = static_cast<std::uint32_t>(x) & 0x1FFFFFu;
    const std::uint64_t uy = static_cast<std::uint32_t>(y) & 0x1FFFFFu;
    const std::uint64_t uz = static_cast<std::uint32_t>(z) & 0x1FFFFFu;
    return (ux << 42) | (uy << 21) | uz;
}

std::size_t CellPosHash::operator()(const CellPos& pos) const noexcept
{
    std::size_t hash = std::hash<std::int32_t>{}(pos.x);
    hash ^= std::hash<std::int32_t>{}(pos.y) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
    hash ^= std::hash<std::int32_t>{}(pos.z) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
    return hash;
}

std::vector<CellPos> regionChunks(const std::vector<Box>& boxes, std::size_t limit,
                                  bool& truncated)
{
    truncated = false;
    std::set<std::array<std::int32_t, 3>> seen;
    for (const Box& box : boxes) {
        if (box.x0 >= box.x1 || box.y0 >= box.y1 || box.z0 >= box.z1) continue;
        for (std::int32_t x = box.x0 >> 4; x <= ((box.x1 - 1) >> 4); ++x) {
            for (std::int32_t y = box.y0 >> 4; y <= ((box.y1 - 1) >> 4); ++y) {
                for (std::int32_t z = box.z0 >> 4; z <= ((box.z1 - 1) >> 4); ++z) {
                    const std::array<std::int32_t, 3> key{x, y, z};
                    if (seen.find(key) != seen.end()) continue;
                    if (seen.size() == limit) {
                        truncated = true;
                        goto finished;
                    }
                    seen.insert(key);
                }
            }
        }
    }
finished:
    std::vector<CellPos> out;
    out.reserve(seen.size());
    for (const auto& key : seen) out.push_back({key[0] * 16, key[1] * 16, key[2] * 16});
    return out;
}

void CellIndex::reset(const Box& region)
{
    clear();
    m_box = region;
    const std::int64_t sx = static_cast<std::int64_t>(region.x1) - region.x0;
    const std::int64_t sy = static_cast<std::int64_t>(region.y1) - region.y0;
    const std::int64_t sz = static_cast<std::int64_t>(region.z1) - region.z0;
    if (sx <= 0 || sy <= 0 || sz <= 0) {
        return;
    }
    m_sy = static_cast<std::uint64_t>(sy);
    m_sz = static_cast<std::uint64_t>(sz);
    const std::uint64_t volume = static_cast<std::uint64_t>(sx) * m_sy * m_sz;
    if (volume <= kMaxSchematicCells) {
        m_dense = true;
        m_slots.assign(static_cast<std::size_t>(volume), 0u);
    }
}

bool CellIndex::slotOf(std::int32_t x, std::int32_t y, std::int32_t z, std::size_t& out) const
{
    if (x < m_box.x0 || x >= m_box.x1 || y < m_box.y0 || y >= m_box.y1 || z < m_box.z0
        || z >= m_box.z1) {
        return false;
    }
    const std::uint64_t dx = static_cast<std::uint64_t>(static_cast<std::int64_t>(x) - m_box.x0);
    const std::uint64_t dy = static_cast<std::uint64_t>(static_cast<std::int64_t>(y) - m_box.y0);
    const std::uint64_t dz = static_cast<std::uint64_t>(static_cast<std::int64_t>(z) - m_box.z0);
    out = static_cast<std::size_t>((dx * m_sy + dy) * m_sz + dz);
    return true;
}

bool CellIndex::insert(std::int32_t x, std::int32_t y, std::int32_t z, std::size_t index)
{
    const std::uint32_t value = static_cast<std::uint32_t>(index + 1);
    if (m_dense) {
        std::size_t slot = 0;
        if (!slotOf(x, y, z, slot) || slot >= m_slots.size()) {
            return false;
        }
        if (m_slots[slot] != 0) {
            return false;
        }
        m_slots[slot] = value;
        ++m_count;
        return true;
    }
    if (!m_sparse.emplace(CellPos{x, y, z}, value).second) {
        return false;
    }
    ++m_count;
    return true;
}

std::size_t CellIndex::find(std::int32_t x, std::int32_t y, std::int32_t z) const
{
    if (m_dense) {
        std::size_t slot = 0;
        if (!slotOf(x, y, z, slot) || slot >= m_slots.size() || m_slots[slot] == 0) {
            return kNone;
        }
        return static_cast<std::size_t>(m_slots[slot]) - 1;
    }
    const auto it = m_sparse.find(CellPos{x, y, z});
    return it == m_sparse.end() ? kNone : static_cast<std::size_t>(it->second) - 1;
}

void CellIndex::clear()
{
    m_box = Box{};
    m_sy = 0;
    m_sz = 0;
    m_dense = false;
    m_slots.clear();
    m_slots.shrink_to_fit();
    m_sparse.clear();
    m_count = 0;
}

void CellIndex::swap(CellIndex& other) noexcept
{
    std::swap(m_box, other.m_box);
    std::swap(m_sy, other.m_sy);
    std::swap(m_sz, other.m_sz);
    std::swap(m_dense, other.m_dense);
    m_slots.swap(other.m_slots);
    m_sparse.swap(other.m_sparse);
    std::swap(m_count, other.m_count);
}

Loaded loadBlueprint(const std::filesystem::path& path)
{
    return finishLoad(structure::loadFile(path));
}

std::size_t assignPaletteBases(std::vector<Placement>& placements)
{
    std::size_t base = 0;
    for (Placement& one : placements) {
        one.paletteBase = base;
        base += one.data ? one.data->palette.size() : 0;
    }
    return base;
}

std::vector<LayoutKey> layoutOf(const std::vector<Placement>& placements)
{
    std::vector<LayoutKey> keys;
    keys.reserve(placements.size());
    for (const Placement& one : placements) {
        LayoutKey key;
        key.data = one.data.get();
        key.paletteBase = one.paletteBase;
        key.x = one.x;
        key.y = one.y;
        key.z = one.z;
        key.rotation = one.rotation & 3;
        if (one.data) {
            key.sizeX = one.data->sizeX;
            key.sizeY = one.data->sizeY;
            key.sizeZ = one.data->sizeZ;
        }
        keys.push_back(key);
    }
    return keys;
}

Box unionBox(const std::vector<Box>& boxes)
{
    Box all;
    bool first = true;
    for (const Box& one : boxes) {
        if (first) {
            all = one;
            first = false;
            continue;
        }
        all.x0 = std::min(all.x0, one.x0);
        all.y0 = std::min(all.y0, one.y0);
        all.z0 = std::min(all.z0, one.z0);
        all.x1 = std::max(all.x1, one.x1);
        all.y1 = std::max(all.y1, one.y1);
        all.z1 = std::max(all.z1, one.z1);
    }
    return all;
}

bool buildCells(const std::vector<Placement>& placements, CellSet& out,
                const std::atomic<bool>* cancel)
{
    out.cells.clear();
    out.cellAt.clear();
    out.boxes.clear();
    out.chunkCells.clear();
    out.keep.clear();
    out.overlapped = 0;
    out.region = Box{};
    out.key = layoutOf(placements);

    std::size_t total = 0;
    for (const Placement& one : placements) {
        if (one.data) {
            total += one.data->blocks.size();
        }
    }
    out.cells.reserve(total);

    for (const Placement& placed : placements) {
        if (!placed.data || !placed.data->valid()) {
            continue;
        }
        const structure::Structure& bp = *placed.data;
        const rotation::Footprint foot = rotation::rotatedFootprint(
            placed.rotation & 3, placed.x, placed.z, bp.sizeX, bp.sizeZ);
        out.boxes.push_back(Box{foot.x0, placed.y, foot.z0, foot.x1, placed.y + bp.sizeY, foot.z1});
    }
    out.region = unionBox(out.boxes);
    out.cellAt.reset(out.region);

    constexpr std::size_t kCancelEvery = 1u << 16;

    for (std::size_t which = 0; which < placements.size(); ++which) {
        const Placement& placed = placements[which];
        if (!placed.data || !placed.data->valid()) {
            continue;
        }
        const structure::Structure& bp = *placed.data;
        out.keep.push_back(placed.data);
        const std::int32_t x0 = placed.x;
        const std::int32_t y0 = placed.y;
        const std::int32_t z0 = placed.z;
        const int quarters = placed.rotation & 3;

        const std::int32_t sizeY = bp.sizeY;
        const std::int32_t sizeZ = bp.sizeZ;
        const std::size_t count = bp.blocks.size();
        for (std::size_t at = 0; at < count; ++at) {
            if (cancel != nullptr && (at % kCancelEvery) == 0
                && cancel->load(std::memory_order_relaxed)) {
                return false;
            }
            const std::int32_t entry = bp.blocks[at];
            if (entry >= 0 && static_cast<std::size_t>(entry) >= bp.palette.size()) {
                continue;
            }
            const std::int32_t local = static_cast<std::int32_t>(at);
            const std::int32_t lz = local % sizeZ;
            const std::int32_t ly = (local / sizeZ) % sizeY;
            const std::int32_t lx = local / (sizeZ * sizeY);

            const std::string& name =
                entry < 0 ? kAirName : bp.palette[static_cast<std::size_t>(entry)].name;
            Cell cell;
            {
                std::int32_t rx = lx;
                std::int32_t rz = lz;
                rotation::rotateLocal(quarters, lx, lz, rx, rz);
                cell.x = x0 + rx;
                cell.y = y0 + ly;
                cell.z = z0 + rz;
            }
            cell.entry = (name == kAirName)
                             ? kAirCell
                             : placed.paletteBase + static_cast<std::size_t>(entry);
            cell.owner = static_cast<int>(which);
            cell.name = &name;
            if (cell.entry != kAirCell && at < bp.blocks2.size()) {
                const std::int32_t second = bp.blocks2[at];
                if (second >= 0 && static_cast<std::size_t>(second) < bp.palette.size()
                    && bp.palette[static_cast<std::size_t>(second)].name != kAirName) {
                    cell.entry2 = static_cast<std::int32_t>(placed.paletteBase) + second;
                }
            }
            if (!out.cellAt.insert(cell.x, cell.y, cell.z, out.cells.size())) {
                ++out.overlapped;
                continue;
            }
            out.chunkCells[packCell(cell.x >> 4, cell.y >> 4, cell.z >> 4)].push_back(
                static_cast<std::uint32_t>(out.cells.size()));
            out.cells.push_back(cell);
        }
    }
    return true;
}

}
