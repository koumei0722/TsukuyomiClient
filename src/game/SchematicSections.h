#pragma once

#include "game/SchematicCells.h"

#include <atomic>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tsukuyomi::schematic {

enum class DiffColor : std::uint8_t {
    None = 0,
    Missing = 1,
    Wrong = 2,
    State = 3,
    Extra = 4,
};
inline constexpr std::uint8_t kDiffColorMax = static_cast<std::uint8_t>(DiffColor::Extra);

enum class DiffKind : std::uint8_t {
    Match = 0,
    Missing = 1,
    Wrong = 2,
    State = 3,
    UnknownWorld = 4,
    UnknownWant = 5,
    Extra = 6,
};
inline constexpr std::size_t kDiffKindCount = 7;

using LegacyOfFn = const void* (*)(const void* block);
DiffKind diffKindOf(const void* real, const void* want, bool wantAir, const void* air, LegacyOfFn legacyOf);
DiffKind diffKindOf(const void* real, const void* want, bool wantAir,
                    const void* realExtra, bool realExtraKnown, bool wantWater,
                    const void* air, LegacyOfFn legacyOf);
DiffColor colorOfDiffKind(DiffKind kind);

struct SectionKey {
    std::int32_t cx = 0, cy = 0, cz = 0;
    bool operator==(const SectionKey&) const = default;
};
struct SectionKeyHash { std::size_t operator()(const SectionKey&) const noexcept; };
inline SectionKey sectionOf(std::int32_t x, std::int32_t y, std::int32_t z) { return {x >> 4, y >> 4, z >> 4}; }

inline constexpr std::uint32_t slotOf(std::int32_t x, std::int32_t y, std::int32_t z)
{
    return static_cast<std::uint32_t>(((x & 15) * 16 + (y & 15)) * 16 + (z & 15));
}
inline constexpr std::uint16_t kWantAir = 0;
inline constexpr std::uint16_t kWantOutside = 0xFFFE;
inline constexpr std::uint16_t kWantUnresolved = 0xFFFF;
struct WantSection {
    std::uint16_t want[4096]{};
    std::uint16_t want2[4096]{};
    std::uint32_t solid = 0;
    bool anyWater = false;
};
struct DiffSection {
    std::uint8_t color[4096]{};
    std::uint32_t counts[kDiffKindCount]{};
    std::vector<std::pair<std::uint16_t, std::uint32_t>> missingByEntry;
    bool anyColor = false;
    std::map<std::uint16_t, std::uint32_t> missingBuild;
};
struct LayerFilter {
    bool on = false;
    int axis = 1;
    std::int32_t lo = 0, hi = 0;
    bool shows(std::int32_t x, std::int32_t y, std::int32_t z) const;
};
class SectionIndex {
public:
    explicit SectionIndex(std::vector<SectionKey> keys);
    std::size_t size() const { return keys_.size(); }
    const SectionKey& key(std::size_t slot) const { return keys_[slot]; }
    std::size_t find(const SectionKey& key) const;
    bool markDirty(const SectionKey& key);
    std::size_t takeDirty(std::vector<std::size_t>& out);
    static constexpr std::size_t kNone = ~std::size_t{0};
private:
    std::vector<SectionKey> keys_;
    std::unordered_map<SectionKey, std::uint32_t, SectionKeyHash> at_;
    std::unique_ptr<std::atomic<std::uint8_t>[]> dirty_;
    std::atomic<std::size_t> dirtyCount_{0};
};
bool buildWantSection(const CellSet& cells, const SectionKey& key, WantSection& out);
using WorldReadFn = bool (*)(void* ctx, std::uint32_t index, const void*& block0, const void*& block1, bool& layer1Known);
void compareSection(const WantSection* want, const std::vector<const void*>& palette, const void* air,
                    WorldReadFn read, void* readCtx, LegacyOfFn legacyOf, std::uint32_t from, std::uint32_t to, DiffSection& out);
void finishDiffSection(DiffSection& out);

struct BoxCell {
    std::int32_t x = 0, y = 0, z = 0;
    std::uint8_t color = 0;
    std::uint8_t covered = 0;
    std::uint32_t nearMask = 0;
};
using NeighborDiffFn = const DiffSection* (*)(void* ctx, const SectionKey& key);
using HighlightMap = std::unordered_map<SectionKey, std::shared_ptr<const std::bitset<4096>>, SectionKeyHash>;
std::size_t collectBoxesInSection(const SectionKey& key, const DiffSection& self, NeighborDiffFn neighbor, void* ctx,
                                const LayerFilter& layer, bool dropInterior, std::vector<BoxCell>& out,
                                DiffColor selectedColor = DiffColor::None, const HighlightMap* highlight = nullptr);
void sectionsInRange(const SectionIndex& index, std::int32_t camCx, std::int32_t camCy, std::int32_t camCz, int radius,
                     std::vector<std::size_t>& out);
struct SectionView {
    SectionKey key;
    std::shared_ptr<const WantSection> want;
    std::shared_ptr<const DiffSection> diff;
    std::uint64_t generation = 0;
};
struct RenderSnapshot {
    std::vector<SectionView> sections;
    std::unordered_map<SectionKey, std::uint32_t, SectionKeyHash> at;
    std::shared_ptr<const std::vector<const void*>> palette;
    const void* air = nullptr;
    bool ghostOverMismatch = true;
    LayerFilter layer;
    std::uint64_t serial = 0;
    bool verifierActive = false;
    DiffColor selectedColor = DiffColor::None;
    bool highlightFiltered = false;
    HighlightMap highlight;
    std::uint64_t highlightGeneration = 0;
};

}
