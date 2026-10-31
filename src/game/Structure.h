#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <limits>
#include <string>
#include <vector>

#include "game/Nbt.h"

namespace tsukuyomi::structure {

struct StateValue {
    std::string name;
    nbt::Tag tag = nbt::Tag::End;
    std::int64_t number = 0;
    std::string text;

    std::string normalized() const;
};

struct PaletteEntry {
    std::string name;
    std::int32_t version = 0;

    std::vector<StateValue> states;

    std::string key() const;
};

struct Structure {
    std::int32_t sizeX = 0;
    std::int32_t sizeY = 0;
    std::int32_t sizeZ = 0;

    std::vector<PaletteEntry> palette;

    std::vector<std::uint16_t> blocks;

    std::vector<std::uint16_t> blocks2;

    bool valid() const { return sizeX > 0 && sizeY > 0 && sizeZ > 0 && !blocks.empty(); }

    std::size_t volume() const
    {
        if (sizeX <= 0 || sizeY <= 0 || sizeZ <= 0) return 0;
        const auto max = (std::numeric_limits<std::size_t>::max)();
        const auto xy = static_cast<std::size_t>(sizeX) * static_cast<std::size_t>(sizeY);
        return xy > max / static_cast<std::size_t>(sizeZ) ? max : xy * static_cast<std::size_t>(sizeZ);
    }

    std::int32_t entryAt(std::size_t at) const
    { return at < blocks.size() ? static_cast<std::int32_t>(blocks[at]) - 1 : -1; }
    std::int32_t entry2At(std::size_t at) const
    { return at < blocks2.size() ? static_cast<std::int32_t>(blocks2[at]) - 1 : -1; }

    std::size_t indexOf(std::int32_t x, std::int32_t y, std::int32_t z) const;

    const std::string& nameAt(std::int32_t x, std::int32_t y, std::int32_t z) const;

};

struct LoadResult {
    Structure value;
    std::uint64_t requiredMemory = 0, availableMemory = 0;
    const char* why = nullptr;
    bool ok() const { return why == nullptr; }
};

LoadResult loadFile(const std::filesystem::path& path);

LoadResult loadBytes(const char* bytes, std::size_t size);

}
