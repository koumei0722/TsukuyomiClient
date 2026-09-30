#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace tsukuyomi::fastblockbreak {

struct BlockPos {
    int x = 0;
    int y = 0;
    int z = 0;
};

struct Offsets {
    std::size_t player = 0;
    std::size_t until = 0;
};

bool readOffsets(std::span<const std::byte> continueBytes,
                 std::span<const std::byte> stopBytes, Offsets& out);

bool shouldStop(const void* armedGm, const void* gameMode, std::int64_t until,
                std::int64_t now, const BlockPos& brokenPos, const BlockPos& nextPos);

}
