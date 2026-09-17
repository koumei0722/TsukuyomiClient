#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
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

    std::int32_t originX = 0;
    std::int32_t originY = 0;
    std::int32_t originZ = 0;

    std::vector<PaletteEntry> palette;

    std::vector<std::int32_t> blocks;

    std::vector<std::int32_t> blocks2;

    std::map<std::size_t, std::vector<StateValue>> entityStates;

    const std::vector<StateValue>& entityAt(std::int32_t x, std::int32_t y,
                                            std::int32_t z) const;

    bool valid() const { return sizeX > 0 && sizeY > 0 && sizeZ > 0 && !blocks.empty(); }

    std::size_t volume() const
    {
        return static_cast<std::size_t>(sizeX) * static_cast<std::size_t>(sizeY)
               * static_cast<std::size_t>(sizeZ);
    }

    std::size_t indexOf(std::int32_t x, std::int32_t y, std::int32_t z) const;

    const std::string& nameAt(std::int32_t x, std::int32_t y, std::int32_t z) const;

    std::int32_t entryAt(std::int32_t x, std::int32_t y, std::int32_t z, int layer) const;
};

struct LoadResult {
    Structure value;
    const char* why = nullptr;
    bool ok() const { return why == nullptr; }
};

LoadResult loadFile(const std::filesystem::path& path);

LoadResult loadBytes(const char* bytes, std::size_t size);

}
