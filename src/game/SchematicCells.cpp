#include "game/SchematicCells.h"

#include <algorithm>
#include <array>
#include <map>
#include <limits>
#include <new>
#include <tuple>

#include "game/Rotation.h"

namespace tsukuyomi::schematic {

namespace {

const std::string kAirName{"minecraft:air"};

Loaded finishLoad(structure::LoadResult&& got)
{
    Loaded out;
    out.requiredMemory = got.requiredMemory;
    out.availableMemory = got.availableMemory;
    out.sizeX = got.value.sizeX;
    out.sizeY = got.value.sizeY;
    out.sizeZ = got.value.sizeZ;
    if (!got.ok()) {
        out.why = got.why;
        return out;
    }
    out.paletteCounts.assign(got.value.palette.size(), 0);
    for (std::size_t at = 0; at < got.value.blocks.size(); ++at) {
        const auto entry = got.value.entryAt(at);
        if (entry >= 0 && static_cast<std::size_t>(entry) < out.paletteCounts.size()) {
            ++out.paletteCounts[static_cast<std::size_t>(entry)];
            if (got.value.palette[static_cast<std::size_t>(entry)].name != kAirName) ++out.solid;
        }
        const auto water = got.value.entry2At(at);
        if (water >= 0 && static_cast<std::size_t>(water) < out.paletteCounts.size())
            ++out.paletteCounts[static_cast<std::size_t>(water)];
    }
    out.data = std::make_shared<const structure::Structure>(std::move(got.value));
    return out;
}

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
    std::vector<CellPos> out;
    for (const Box& box : boxes) {
        if (box.x0 >= box.x1 || box.y0 >= box.y1 || box.z0 >= box.z1) continue;
        for (std::int32_t x = box.x0 >> 4; x <= ((box.x1 - 1) >> 4); ++x)
            for (std::int32_t y = box.y0 >> 4; y <= ((box.y1 - 1) >> 4); ++y)
                for (std::int32_t z = box.z0 >> 4; z <= ((box.z1 - 1) >> 4); ++z)
                    out.push_back({x * 16, y * 16, z * 16});
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return std::tie(a.x,a.y,a.z) < std::tie(b.x,b.y,b.z);
    });
    out.erase(std::unique(out.begin(), out.end()), out.end());
    truncated = out.size() > limit;
    if (truncated) out.resize(limit);
    return out;
}

Loaded loadBlueprint(const std::filesystem::path& path)
{
    auto got = structure::loadFile(path);
    try {
        return finishLoad(std::move(got));
    } catch (const std::bad_alloc&) {
        Loaded out;
        out.why = "out of memory";
        out.sizeX = got.value.sizeX; out.sizeY = got.value.sizeY; out.sizeZ = got.value.sizeZ;
        out.requiredMemory = got.requiredMemory; out.availableMemory = got.availableMemory;
        return out;
    }
}
std::size_t assignPaletteBases(std::vector<Placement>& placements)
{
    std::size_t base = 0;
    std::map<std::tuple<const void*, int, int>, std::size_t> bases;
    for (Placement& one : placements) {
        const auto key = std::make_tuple(one.data.get(), rotation::normalize(one.rotation), one.mirror);
        const auto [at, fresh] = bases.emplace(key, base);
        one.paletteBase = at->second;
        if (fresh) base += one.data ? one.data->palette.size() : 0;
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
        key.mirror = one.mirror;
        key.owner = one.owner;
        key.placementId = one.placementId;
        key.copies = one.copies;
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

namespace {
Box intersection(const Box& a, const Box& b)
{
    return {std::max(a.x0,b.x0), std::max(a.y0,b.y0), std::max(a.z0,b.z0),
            std::min(a.x1,b.x1), std::min(a.y1,b.y1), std::min(a.z1,b.z1)};
}
bool nonempty(const Box& b) { return b.x0 < b.x1 && b.y0 < b.y1 && b.z0 < b.z1; }
}

Box CellSet::chunkIntersection(const Box& b, std::int32_t cx, std::int32_t cy, std::int32_t cz)
{
    const auto x = static_cast<std::int64_t>(cx) * 16;
    const auto y = static_cast<std::int64_t>(cy) * 16;
    const auto z = static_cast<std::int64_t>(cz) * 16;
    if (x >= b.x1 || x+16 <= b.x0 || y >= b.y1 || y+16 <= b.y0
        || z >= b.z1 || z+16 <= b.z0) return {};
    return {static_cast<std::int32_t>(std::max<std::int64_t>(b.x0,x)),
            static_cast<std::int32_t>(std::max<std::int64_t>(b.y0,y)),
            static_cast<std::int32_t>(std::max<std::int64_t>(b.z0,z)),
            static_cast<std::int32_t>(std::min<std::int64_t>(b.x1,x+16)),
            static_cast<std::int32_t>(std::min<std::int64_t>(b.y1,y+16)),
            static_cast<std::int32_t>(std::min<std::int64_t>(b.z1,z+16))};
}

std::size_t CellSet::localAt(const Source& source, std::int32_t x, std::int32_t y, std::int32_t z)
{
    if (x == INT32_MAX || y == INT32_MAX || z == INT32_MAX) return kNone;
    const auto& p = source.placed;
    const auto copies = copiesOverlapping(p.copies,source.baseBox,{x,y,z,x+1,y+1,z+1});
    if (copies.empty()) return kNone;
    const auto& bp = *p.data;
    const auto ny = static_cast<std::size_t>(p.copies.last[1]-p.copies.first[1]+1);
    const auto nz = static_cast<std::size_t>(p.copies.last[2]-p.copies.first[2]+1);
    for (auto ix=copies.first[0]; ix<=copies.last[0]; ++ix)
        for (auto iy=copies.first[1]; iy<=copies.last[1]; ++iy)
            for (auto iz=copies.first[2]; iz<=copies.last[2]; ++iz) {
                const auto origin = p.copies.origin({p.x,p.y,p.z},{ix,iy,iz});
                std::int32_t lx=0,lz=0;
                const auto dx=static_cast<std::int64_t>(x)-origin[0], dz=static_cast<std::int64_t>(z)-origin[2];
                if (dx < INT32_MIN || dx > INT32_MAX || dz < INT32_MIN || dz > INT32_MAX) continue;
                rotation::inverseLocal(p.rotation,p.mirror,static_cast<int>(dx),static_cast<int>(dz),lx,lz);
                const auto at = bp.indexOf(lx,y-origin[1],lz);
                if (at >= bp.blocks.size()) continue;
                const auto entry = bp.entryAt(at);
                if (entry >= 0 && static_cast<std::size_t>(entry) >= bp.palette.size()) continue;
                const auto copy = (static_cast<std::size_t>(ix-p.copies.first[0])*ny
                    +static_cast<std::size_t>(iy-p.copies.first[1]))*nz+static_cast<std::size_t>(iz-p.copies.first[2]);
                return copy*bp.volume()+at;
            }
    return kNone;
}

std::size_t CellSet::find(std::int32_t x, std::int32_t y, std::int32_t z) const
{
    const auto found = sourcesByChunk.find({(x >> 4) * 16, (y >> 4) * 16, (z >> 4) * 16});
    if (found == sourcesByChunk.end()) return kNone;
    for (const auto which : found->second) {
        const Source& source = sources[which];
        const auto at = localAt(source,x,y,z);
        if (at != kNone) return source.offset + at;
    }
    return kNone;
}

bool CellSet::cellAt(std::size_t at, Cell& out) const
{
    if (at >= total) return false;
    auto sourceAt = std::upper_bound(sources.begin(), sources.end(), at,
        [](std::size_t value, const Source& source) { return value < source.offset; });
    if (sourceAt == sources.begin()) return false;
    const Source& source = *--sourceAt;
    {
        const auto& p = source.placed;
        const auto& bp = *p.data;
        if (at < source.offset || (at-source.offset)/bp.volume() >= source.copyCount) return false;
        const auto local = (at-source.offset)%bp.volume();
        auto copy = (at-source.offset)/bp.volume();
        std::array<std::int64_t,3> index{};
        for (int axis=2; axis>=0; --axis) {
            const auto n = static_cast<std::size_t>(p.copies.last[axis]-p.copies.first[axis]+1);
            index[axis] = p.copies.first[axis]+static_cast<std::int64_t>(copy%n);
            copy /= n;
        }
        const auto origin = p.copies.origin({p.x,p.y,p.z},index);
        if (local >= bp.blocks.size()) return false;
        const auto entry = bp.entryAt(local);
        if (entry >= 0 && static_cast<std::size_t>(entry) >= bp.palette.size()) return false;
        const auto sz = static_cast<std::size_t>(bp.sizeZ);
        const auto sy = static_cast<std::size_t>(bp.sizeY);
        const auto lx = static_cast<std::int32_t>(local/(sz*sy));
        const auto ly = static_cast<std::int32_t>((local/sz)%sy);
        const auto lz = static_cast<std::int32_t>(local%sz);
        std::int32_t rx = 0, rz = 0;
        rotation::transformLocal(p.rotation,p.mirror,lx,lz,rx,rz);
        Cell cell;
        const std::int64_t wx = static_cast<std::int64_t>(origin[0])+rx, wy=static_cast<std::int64_t>(origin[1])+ly,
            wz=static_cast<std::int64_t>(origin[2])+rz;
        if (wx < INT32_MIN || wx >= INT32_MAX || wy < INT32_MIN || wy >= INT32_MAX || wz < INT32_MIN || wz >= INT32_MAX) return false;
        cell.x = static_cast<int>(wx); cell.y = static_cast<int>(wy); cell.z = static_cast<int>(wz);
        if (find(cell.x,cell.y,cell.z) != at) return false;
        const bool air = entry < 0 || source.air[static_cast<std::size_t>(entry)] != 0;
        cell.name = entry < 0 ? &kAirName : &bp.palette[static_cast<std::size_t>(entry)].name;
        cell.entry = air ? kAirCell : p.paletteBase+static_cast<std::size_t>(entry);
        cell.owner = source.owner;
        if (!air && local < bp.blocks2.size()) {
            const auto second = bp.entry2At(local);
            if (second >= 0 && static_cast<std::size_t>(second) < source.air.size()
                && !source.air[static_cast<std::size_t>(second)])
                cell.entry2 = static_cast<std::int32_t>(p.paletteBase)+second;
        }
        out = cell;
        return true;
    }
    return false;
}

bool CellSet::contains(std::int32_t x, std::int32_t y, std::int32_t z) const
{
    if (x == INT32_MAX || y == INT32_MAX || z == INT32_MAX) return false;
    const auto found = sourcesByChunk.find({(x>>4)*16,(y>>4)*16,(z>>4)*16});
    if (found == sourcesByChunk.end()) return false;
    for (const auto which : found->second) {
        const auto& source = sources[which];
        if (!copiesOverlapping(source.placed.copies,source.baseBox,{x,y,z,x+1,y+1,z+1}).empty()) return true;
    }
    return false;
}

bool buildCells(const std::vector<Placement>& placements, CellSet& out,
                const std::atomic<bool>* cancel)
{
    out = CellSet{};
    try {
        out.key = layoutOf(placements);
        for (std::size_t which = 0; which < placements.size(); ++which) {
            if (cancel && cancel->load(std::memory_order_relaxed)) return false;
            const auto& p = placements[which];
            if (!p.data || !p.data->valid()) continue;
            const auto& bp = *p.data;
            const auto foot = rotation::transformedFootprint(p.rotation,p.mirror,p.x,p.z,bp.sizeX,bp.sizeZ);
            CellSet::Source source;
            source.placed = p;
            source.owner = p.owner >= 0 ? p.owner : static_cast<int>(which);
            source.offset = out.total;
            source.baseBox = {foot.x0,p.y,foot.z0,foot.x1,p.y+bp.sizeY,foot.z1};
            if (!p.copies.count(source.copyCount) || source.copyCount > (SIZE_MAX-out.total)/bp.volume()) {
                out = CellSet{}; out.why = "placement index overflow"; return false;
            }
            if (!source.copyCount) continue;
            source.box = copiesBox(source.baseBox,p.copies);
            for (const auto& entry : bp.palette) source.air.push_back(entry.name == kAirName ? 1 : 0);
            bool unused = false;
            const auto chunks = regionChunks({source.box}, (std::numeric_limits<std::size_t>::max)(), unused);
            const auto sourceIndex = static_cast<std::uint32_t>(out.sources.size());
            for (const auto& chunk : chunks) {
                if (cancel && cancel->load(std::memory_order_relaxed)) return false;
                const Box section = CellSet::chunkIntersection(source.box,chunk.x>>4,chunk.y>>4,chunk.z>>4);
                if (copiesOverlapping(p.copies,source.baseBox,section).empty()) continue;
                auto& candidates = out.sourcesByChunk[chunk];
                for (const auto i : candidates)
                    if (nonempty(intersection(source.box,out.sources[i].box))) source.earlier.push_back(i);
                candidates.push_back(sourceIndex);
            }
            std::sort(source.earlier.begin(),source.earlier.end());
            source.earlier.erase(std::unique(source.earlier.begin(),source.earlier.end()),source.earlier.end());
            out.total += source.copyCount*bp.volume();
            out.boxes.push_back(source.box);
            out.keep.push_back(p.data);
            out.sources.push_back(std::move(source));
        }
        out.region = unionBox(out.boxes);
        bool truncated = false;
        out.chunks = regionChunks(out.boxes, (std::numeric_limits<std::size_t>::max)(), truncated);
        std::erase_if(out.chunks,[&](const auto& chunk) { return !out.sourcesByChunk.contains(chunk); });
        const bool grid = std::any_of(out.sources.begin(),out.sources.end(),[](const auto& source) { return source.copyCount > 1; });
        if (grid) {
            const bool overlaps = std::any_of(out.sources.begin(),out.sources.end(),[](const auto& source) {
                if (!source.earlier.empty()) return true;
                const auto& c=source.placed.copies;
                const std::int64_t extent[3]{static_cast<std::int64_t>(source.baseBox.x1)-source.baseBox.x0,
                    static_cast<std::int64_t>(source.baseBox.y1)-source.baseBox.y0,static_cast<std::int64_t>(source.baseBox.z1)-source.baseBox.z0};
                for(int a=0;a<3;++a) if(c.last[a]>c.first[a] && c.step[a]<extent[a]) return true;
                return false;
            });
            if (!overlaps) return true;
            std::size_t valid = 0, unique = 0;
            for (const auto& source : out.sources) {
                std::size_t n = 0;
                for (std::size_t i=0; i<source.placed.data->blocks.size(); ++i) {
                    const auto entry = source.placed.data->entryAt(i);
                    if (entry < 0 || static_cast<std::size_t>(entry) < source.air.size()) ++n;
                }
                valid += n*source.copyCount;
            }
            for (const auto& chunk : out.chunks) {
                if (cancel && cancel->load(std::memory_order_relaxed)) return false;
                unique += out.forEachInChunk(chunk.x>>4,chunk.y>>4,chunk.z>>4,[](std::size_t) {});
            }
            out.overlapped = valid-unique;
            return true;
        }
        std::size_t visited = 0;
        for (std::size_t i = 0; i < out.sources.size(); ++i) {
            const auto& source = out.sources[i];
            for (const auto earlier : source.earlier) {
                const auto b = intersection(source.box,out.sources[earlier].box);
                for (auto x=b.x0; x<b.x1; ++x) for (auto y=b.y0; y<b.y1; ++y) for (auto z=b.z0; z<b.z1; ++z) {
                    if ((visited++ % (1u << 16)) == 0 && cancel && cancel->load(std::memory_order_relaxed)) return false;
                    if (CellSet::kNone == out.localAt(source,x,y,z)) continue;
                    const auto first = out.find(x,y,z);
                    if (first >= out.sources[earlier].offset
                        && first < out.sources[earlier].offset + out.sources[earlier].placed.data->volume()) ++out.overlapped;
                }
            }
        }
        return true;

    } catch (const std::bad_alloc&) {
        out = CellSet{};
        out.why = "out of memory";
        return false;
    }
}

}
