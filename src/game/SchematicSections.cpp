#include "game/SchematicSections.h"

#include "game/Rotation.h"
#include "render/BoxMesher.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <tuple>

namespace tsukuyomi::schematic {

DiffKind diffKindOf(const void* real, const void* want, bool wantAir, const void* air, LegacyOfFn legacyOf)
{
    if (real == nullptr) {
        return DiffKind::UnknownWorld;
    }
    if (air == nullptr) {
        return DiffKind::UnknownWorld;
    }
    if (wantAir) {
        return real != air ? DiffKind::Extra : DiffKind::Match;
    }
    if (real == air) {
        return DiffKind::Missing;
    }
    if (want == nullptr) {
        return DiffKind::UnknownWant;
    }
    if (real == want) {
        return DiffKind::Match;
    }
    const void* const legacyReal = legacyOf(real);
    const void* const legacyWant = legacyOf(want);
    if (legacyReal != nullptr && legacyReal == legacyWant) {
        return DiffKind::State;
    }
    return DiffKind::Wrong;
}

DiffKind diffKindOf(const void* real, const void* want, bool wantAir,
                    const void* realExtra, bool realExtraKnown, bool wantWater, const void* air, LegacyOfFn legacyOf)
{
    const DiffKind base = diffKindOf(real, want, wantAir, air, legacyOf);
    if (base != DiffKind::Match || wantAir || !realExtraKnown) {
        return base;
    }
    const bool realWater = (realExtra != nullptr && realExtra != air);
    return realWater == wantWater ? DiffKind::Match : DiffKind::State;
}

DiffColor colorOfDiffKind(DiffKind kind)
{
    switch (kind) {
    case DiffKind::Missing:
        return DiffColor::Missing;
    case DiffKind::Wrong:
        return DiffColor::Wrong;
    case DiffKind::State:
        return DiffColor::State;
    case DiffKind::Extra:
        return DiffColor::Extra;
    case DiffKind::Match:
    case DiffKind::UnknownWorld:
    case DiffKind::UnknownWant:
        break;
    }
    return DiffColor::None;
}

std::size_t SectionKeyHash::operator()(const SectionKey& key) const noexcept
{
    return CellPosHash{}({key.cx, key.cy, key.cz});
}

bool LayerFilter::shows(std::int32_t x, std::int32_t y, std::int32_t z) const
{
    const auto v = axis == 0 ? x : (axis == 2 ? z : y);
    return !on || (v >= lo && v <= hi);
}

SectionIndex::SectionIndex(std::vector<SectionKey> keys) : keys_(std::move(keys))
{
    std::sort(keys_.begin(), keys_.end(), [](const SectionKey& a, const SectionKey& b) {
        return std::tie(a.cx, a.cy, a.cz) < std::tie(b.cx, b.cy, b.cz);
    });
    keys_.erase(std::unique(keys_.begin(), keys_.end()), keys_.end());
    at_.reserve(keys_.size());
    dirty_ = std::make_unique<std::atomic<std::uint8_t>[]>(keys_.size());
    for (std::size_t i = 0; i < keys_.size(); ++i) {
        at_.emplace(keys_[i], static_cast<std::uint32_t>(i));
        dirty_[i].store(0, std::memory_order_relaxed);
    }
}

std::size_t SectionIndex::find(const SectionKey& key) const
{
    const auto it = at_.find(key);
    return it == at_.end() ? kNone : it->second;
}

bool SectionIndex::markDirty(const SectionKey& key)
{
    const auto at = find(key);
    if (at == kNone) return false;
    dirtyCount_.fetch_add(1, std::memory_order_seq_cst);
    if (dirty_[at].exchange(1, std::memory_order_seq_cst) == 0) return true;
    dirtyCount_.fetch_sub(1, std::memory_order_seq_cst);
    return false;
}

std::size_t SectionIndex::takeDirty(std::vector<std::size_t>& out)
{
    out.clear();
    if (dirtyCount_.load(std::memory_order_seq_cst) == 0) return 0;
    for (std::size_t i = 0; i < keys_.size(); ++i) {
        if (dirty_[i].exchange(0, std::memory_order_seq_cst) != 0) {
            dirtyCount_.fetch_sub(1, std::memory_order_seq_cst);
            out.push_back(i);
        }
    }
    return out.size();
}

bool buildWantSection(const CellSet& cells, const SectionKey& key, WantSection& out)
{
    out = WantSection{};
    std::fill(std::begin(out.want), std::end(out.want), kWantOutside);
    bool found = false;
    for (int dx = 0; dx < 16; ++dx) for (int dy = 0; dy < 16; ++dy) for (int dz = 0; dz < 16; ++dz) {
        const auto x = key.cx * 16 + dx, y = key.cy * 16 + dy, z = key.cz * 16 + dz;
        const auto slot = slotOf(x, y, z);
        const auto at = cells.find(x, y, z);
        Cell cell;
        if (at != CellSet::kNone && cells.cellAt(at, cell)) {
            found = true;
            out.want[slot] = cell.entry == kAirCell ? kWantAir
                : (cell.entry < kWantOutside - 1u ? static_cast<std::uint16_t>(cell.entry + 1) : kWantUnresolved);
            if (out.want[slot] != kWantAir && out.want[slot] != kWantUnresolved) ++out.solid;
            if (cell.entry2 >= 0) {
                out.want2[slot] = static_cast<std::uint32_t>(cell.entry2) < kWantOutside - 1u
                    ? static_cast<std::uint16_t>(cell.entry2 + 1) : kWantUnresolved;
                out.anyWater = true;
            }
            continue;
        }
        if (cells.contains(x,y,z)) { out.want[slot] = kWantUnresolved; found = true; }
    }
    return found;
}

void compareSection(const WantSection* want, const std::vector<const void*>& palette, const void* air,
                    WorldReadFn read, void* readCtx, LegacyOfFn legacyOf, std::uint32_t from, std::uint32_t to, DiffSection& out)
{
    for (auto i = from; i < (std::min)(to, 4096u); ++i) {
        const auto tag = want == nullptr ? kWantAir : want->want[i];
        if (tag == kWantOutside) {
            out.color[i] = 0;
            continue;
        }
        const void* block0 = nullptr;
        const void* block1 = nullptr;
        bool known = false;
        if (read == nullptr || !read(readCtx, i, block0, block1, known)) block0 = nullptr;
        const void* expected = tag != kWantAir && tag != kWantUnresolved && tag - 1u < palette.size()
            ? palette[tag - 1u] : nullptr;
        const auto kind = diffKindOf(block0, expected, tag == kWantAir, block1, known,
                                    want != nullptr && want->anyWater && want->want2[i] != 0, air, legacyOf);
        ++out.counts[static_cast<std::size_t>(kind)];
        if (kind == DiffKind::Missing && tag > kWantAir && tag < kWantOutside) {
            ++out.missingBuild[static_cast<std::uint16_t>(tag - 1)];
        }
        out.color[i] = static_cast<std::uint8_t>(colorOfDiffKind(kind));
    }
}

void finishDiffSection(DiffSection& out)
{
    out.anyColor = false;
    for (std::uint32_t i = 0; i < 4096; ++i) {
        if (out.color[i] >= 1 && out.color[i] <= kDiffColorMax) {
            out.anyColor = true;
            break;
        }
    }
    out.missingByEntry.assign(out.missingBuild.begin(), out.missingBuild.end());
    out.missingBuild.clear();
}

std::size_t collectBoxesInSection(const SectionKey& key, const DiffSection& self, NeighborDiffFn neighbor, void* ctx,
                                const LayerFilter& layer, bool dropInterior, std::vector<BoxCell>& out,
                                DiffColor selectedColor, const HighlightMap* highlight)
{
    out.clear();
    const DiffSection* around[3][3][3]{};
    for (int dx = -1; dx <= 1; ++dx) for (int dy = -1; dy <= 1; ++dy) for (int dz = -1; dz <= 1; ++dz) {
        around[dx + 1][dy + 1][dz + 1] = dx == 0 && dy == 0 && dz == 0 ? &self
            : (neighbor == nullptr ? nullptr : neighbor(ctx, {key.cx + dx, key.cy + dy, key.cz + dz}));
    }
    const auto lit = [&](std::int32_t x, std::int32_t y, std::int32_t z) {
        if (!layer.shows(x, y, z)) return false;
        const auto k = sectionOf(x, y, z);
        const auto* diff = around[k.cx - key.cx + 1][k.cy - key.cy + 1][k.cz - key.cz + 1];
        if (diff == nullptr) return false;
        const auto color = diff->color[slotOf(x, y, z)];
        if (selectedColor != DiffColor::None && color != static_cast<std::uint8_t>(selectedColor)) return false;
        if (highlight) {
            const auto it = highlight->find(k);
            if (it == highlight->end() || !it->second || !it->second->test(slotOf(x, y, z))) return false;
        }
        return color >= 1 && color <= kDiffColorMax;
    };
    for (int dx = 0; dx < 16; ++dx) for (int dy = 0; dy < 16; ++dy) for (int dz = 0; dz < 16; ++dz) {
        const auto x = key.cx * 16 + dx, y = key.cy * 16 + dy, z = key.cz * 16 + dz;
        if (!lit(x, y, z)) continue;
        std::uint32_t nearMask = 0;
        for (const auto& n : boxmesh::kNearOffsets) {
            if (lit(x + n.x, y + n.y, z + n.z)) nearMask |= boxmesh::nearBit(n.x, n.y, n.z);
        }
        if (dropInterior && nearMask == boxmesh::kNearAll) continue;
        out.push_back({x, y, z, self.color[slotOf(x, y, z)], static_cast<std::uint8_t>(nearMask & 0x3Fu), nearMask});
    }
    return out.size();
}

void sectionsInRange(const SectionIndex& index, std::int32_t camCx, std::int32_t camCy, std::int32_t camCz, int radius,
                     std::vector<std::size_t>& out)
{
    out.clear();
    if (radius < 0) return;
    const auto distance = [&](std::size_t at) {
        const auto& k = index.key(at);
        const double dx = static_cast<double>(k.cx) - camCx, dy = static_cast<double>(k.cy) - camCy, dz = static_cast<double>(k.cz) - camCz;
        return dx * dx + dy * dy + dz * dz;
    };
    for (std::size_t i = 0; i < index.size(); ++i) {
        const auto& k = index.key(i);
        const auto dx = static_cast<std::int64_t>(k.cx) - camCx, dz = static_cast<std::int64_t>(k.cz) - camCz;
        if (dx >= -static_cast<std::int64_t>(radius) && dx <= radius && dz >= -static_cast<std::int64_t>(radius) && dz <= radius) out.push_back(i);
    }
    std::sort(out.begin(), out.end(), [&](std::size_t a, std::size_t b) {
        const auto da = distance(a), db = distance(b);
        return da == db ? a < b : da < db;
    });
}

}
