#include "game/Litematic.h"

#include <algorithm>
#include <limits>
#include <map>
#include <new>
#include <string>
#include <vector>
#include <Windows.h>

#include "game/Inflate.h"
#include "game/JavaBlocks.h"
#include "game/Nbt.h"

namespace tsukuyomi::litematic {

namespace {

constexpr std::size_t kMaxInflated = std::size_t{3} << 30;

bool readXyz(const nbt::Value* node, std::int32_t& x, std::int32_t& y, std::int32_t& z)
{
    if (node == nullptr || node->tag != nbt::Tag::Compound) return false;
    const nbt::Value* const vx = node->find("x", nbt::Tag::Int);
    const nbt::Value* const vy = node->find("y", nbt::Tag::Int);
    const nbt::Value* const vz = node->find("z", nbt::Tag::Int);
    if (vx == nullptr || vy == nullptr || vz == nullptr) return false;
    x = static_cast<std::int32_t>(vx->number);
    y = static_cast<std::int32_t>(vy->number);
    z = static_cast<std::int32_t>(vz->number);
    return true;
}

struct Region {
    std::string name;
    std::int64_t minX = 0, minY = 0, minZ = 0;
    std::int64_t sizeX = 0, sizeY = 0, sizeZ = 0;
    const nbt::Value* palette = nullptr;
    const nbt::Value* states = nullptr;
};

void axis(std::int32_t position, std::int32_t size, std::int64_t& min, std::int64_t& length)
{
    length = size < 0 ? -static_cast<std::int64_t>(size) : size;
    min = size < 0 ? static_cast<std::int64_t>(position) + size + 1 : position;
}

bool isAir(const std::string& name)
{
    return name == "minecraft:air" || name == "minecraft:cave_air" || name == "minecraft:void_air"
           || name == "air" || name == "cave_air" || name == "void_air";
}

}

structure::LoadResult loadNbt(const char* bytes, std::size_t size)
try {
    structure::LoadResult result;
    const nbt::Result parsed = nbt::read(bytes, size, nbt::Endian::Big);
    if (!parsed.root) {
        result.why = parsed.why != nullptr ? parsed.why : "could not read the NBT";
        return result;
    }
    const nbt::Value& root = *parsed.root;
    const nbt::Value* const regions = root.find("Regions", nbt::Tag::Compound);
    if (regions == nullptr) {
        result.why = "no Regions (not a .litematic?)";
        return result;
    }

    std::vector<Region> list;
    for (const auto& [name, node] : regions->compound) {
        if (!node || node->tag != nbt::Tag::Compound) continue;
        std::int32_t px = 0, py = 0, pz = 0, sx = 0, sy = 0, sz = 0;
        if (!readXyz(node->find("Position"), px, py, pz) || !readXyz(node->find("Size"), sx, sy, sz)) {
            result.why = "region without Position / Size";
            return result;
        }
        Region r;
        r.name = name;
        axis(px, sx, r.minX, r.sizeX);
        axis(py, sy, r.minY, r.sizeY);
        axis(pz, sz, r.minZ, r.sizeZ);
        if (r.sizeX == 0 || r.sizeY == 0 || r.sizeZ == 0) continue;
        r.palette = node->find("BlockStatePalette", nbt::Tag::List);
        r.states = node->find("BlockStates", nbt::Tag::LongArray);
        if (r.palette == nullptr || r.states == nullptr) {
            result.why = "region without BlockStatePalette / BlockStates";
            return result;
        }
        list.push_back(std::move(r));
    }
    if (list.empty()) {
        result.why = "no usable region";
        return result;
    }
    std::int64_t minX = list[0].minX, minY = list[0].minY, minZ = list[0].minZ;
    std::int64_t maxX = minX, maxY = minY, maxZ = minZ;
    for (const Region& r : list) {
        minX = (std::min)(minX, r.minX); minY = (std::min)(minY, r.minY); minZ = (std::min)(minZ, r.minZ);
        maxX = (std::max)(maxX, r.minX + r.sizeX); maxY = (std::max)(maxY, r.minY + r.sizeY);
        maxZ = (std::max)(maxZ, r.minZ + r.sizeZ);
    }
    constexpr std::int64_t kAxisMax = (std::numeric_limits<std::int32_t>::max)();
    if (maxX - minX > kAxisMax || maxY - minY > kAxisMax || maxZ - minZ > kAxisMax) {
        result.why = "size is too large";
        return result;
    }
    structure::Structure& out = result.value;
    out.sizeX = static_cast<std::int32_t>(maxX - minX);
    out.sizeY = static_cast<std::int32_t>(maxY - minY);
    out.sizeZ = static_cast<std::int32_t>(maxZ - minZ);
    const std::size_t volume = out.volume();

    for (const Region& r : list) {
        const std::size_t count = r.palette->list.size();
        if (count == 0) {
            result.why = "empty BlockStatePalette";
            return result;
        }
        int bits = 2;
        while (bits < 32 && (std::size_t{1} << bits) < count) ++bits;
        const auto cells = static_cast<unsigned long long>(r.sizeX) * static_cast<unsigned long long>(r.sizeY)
                           * static_cast<unsigned long long>(r.sizeZ);
        if (cells > (std::numeric_limits<unsigned long long>::max)() / static_cast<unsigned>(bits)
            || r.states->numbers.size() < (cells * static_cast<unsigned>(bits) + 63) / 64) {
            result.why = "BlockStates shorter than the region";
            return result;
        }
    }

    {
        constexpr std::uint64_t margin = 256ull * 1024 * 1024;
        const std::uint64_t layers = static_cast<std::uint64_t>(volume) * 4;
        result.requiredMemory = layers > (std::numeric_limits<std::uint64_t>::max)() - margin
                                    ? (std::numeric_limits<std::uint64_t>::max)() : layers + margin;
        MEMORYSTATUSEX status{};
        status.dwLength = sizeof(status);
        if (GlobalMemoryStatusEx(&status)) {
            result.availableMemory = status.ullAvailPhys;
            if (result.availableMemory < result.requiredMemory) {
                result.why = "not enough free memory";
                return result;
            }
        }
    }

    std::map<std::string, std::uint16_t> seat;
    std::map<std::string, std::size_t> unknown;
    const auto seatOf = [&](structure::PaletteEntry&& entry) -> std::uint16_t {
        const std::string key = entry.key();
        const auto at = seat.find(key);
        if (at != seat.end()) return at->second;
        if (out.palette.size() >= 65534) return 0xFFFFu;
        out.palette.push_back(std::move(entry));
        const auto index = static_cast<std::uint16_t>(out.palette.size());
        seat.emplace(key, index);
        return index;
    };
    std::uint16_t waterSeat = 0;
    struct Slot { std::uint16_t block = 0; bool air = false; bool water = false; };
    std::vector<std::vector<Slot>> slots(list.size());
    for (std::size_t ri = 0; ri < list.size(); ++ri) {
        for (const auto& one : list[ri].palette->list) {
            Slot slot;
            std::string name;
            std::vector<std::pair<std::string, std::string>> properties;
            if (one && one->tag == nbt::Tag::Compound) {
                if (const nbt::Value* const n = one->find("Name", nbt::Tag::String)) name = n->text;
                if (const nbt::Value* const p = one->find("Properties", nbt::Tag::Compound)) {
                    for (const auto& [key, value] : p->compound)
                        if (value && value->tag == nbt::Tag::String) properties.emplace_back(key, value->text);
                }
            }
            slot.air = isAir(name);
            javablocks::Converted converted = javablocks::toBedrock(name, properties);
            if (converted.known) {
                slot.block = seatOf(std::move(converted.entry));
                if (converted.waterlogged) {
                    if (waterSeat == 0) waterSeat = seatOf(javablocks::bedrockWater());
                    slot.water = true;
                }
            } else {
                ++unknown[name.empty() ? std::string{"(no name)"} : name];
            }
            slots[ri].push_back(slot);
        }
    }

    out.blocks.assign(volume, 0);
    std::vector<std::uint16_t> water;
    std::size_t unknownCells = 0;
    for (std::size_t ri = 0; ri < list.size(); ++ri) {
        const Region& r = list[ri];
        const std::vector<Slot>& table = slots[ri];
        unsigned bits = 2;
        while (bits < 32 && (std::size_t{1} << bits) < table.size()) ++bits;
        const std::uint64_t mask = (std::uint64_t{1} << bits) - 1;
        const std::vector<std::int64_t>& longs = r.states->numbers;
        const auto ox = static_cast<std::size_t>(r.minX - minX);
        const auto oy = static_cast<std::size_t>(r.minY - minY);
        const auto oz = static_cast<std::size_t>(r.minZ - minZ);
        const auto rsx = static_cast<std::size_t>(r.sizeX), rsy = static_cast<std::size_t>(r.sizeY),
                   rsz = static_cast<std::size_t>(r.sizeZ);
        const auto sy = static_cast<std::size_t>(out.sizeY), sz = static_cast<std::size_t>(out.sizeZ);
        std::uint64_t bit = 0;
        for (std::size_t y = 0; y < rsy; ++y) {
            for (std::size_t z = 0; z < rsz; ++z) {
                for (std::size_t x = 0; x < rsx; ++x, bit += bits) {
                    const std::size_t word = static_cast<std::size_t>(bit >> 6);
                    const unsigned shift = static_cast<unsigned>(bit & 63);
                    std::uint64_t value = static_cast<std::uint64_t>(longs[word]) >> shift;
                    if (shift + bits > 64) value |= static_cast<std::uint64_t>(longs[word + 1]) << (64 - shift);
                    value &= mask;
                    const std::size_t at = ((ox + x) * sy + (oy + y)) * sz + (oz + z);
                    if (value >= table.size()) {
                        out.blocks[at] = 0xFFFFu;
                        continue;
                    }
                    const Slot& slot = table[static_cast<std::size_t>(value)];
                    if (slot.block == 0) {
                        if (!slot.air) ++unknownCells;
                        continue;
                    }
                    if (slot.air && out.blocks[at] != 0) continue;
                    out.blocks[at] = slot.block;
                    if (slot.water) {
                        if (water.empty()) water.assign(volume, 0);
                        water[at] = waterSeat;
                    } else if (!water.empty()) {
                        water[at] = 0;
                    }
                }
            }
        }
    }
    out.blocks2 = std::move(water);

    if (!unknown.empty()) {
        std::string note = "unknown Java blocks (" + std::to_string(unknownCells) + " cells):";
        std::size_t shown = 0;
        for (const auto& [name, count] : unknown) {
            if (shown++ == 8) { note += " ..."; break; }
            note += " " + name;
        }
        result.note = std::move(note);
    }
    return result;
}
catch (const std::bad_alloc&) {
    structure::LoadResult result;
    result.why = "out of memory";
    return result;
}

structure::LoadResult loadBytes(const char* bytes, std::size_t size)
try {
    structure::LoadResult result;
    std::vector<char> inflated;
    if (const char* why = inflate::gunzip(bytes, size, inflated, kMaxInflated)) {
        result.why = why;
        return result;
    }
    return loadNbt(inflated.data(), inflated.size());
}
catch (const std::bad_alloc&) {
    structure::LoadResult result;
    result.why = "out of memory";
    return result;
}

}
