#include "modules/FastBlockBreakLogic.h"

#include <cstring>

namespace tsukuyomi::fastblockbreak {

bool readOffsets(std::span<const std::byte> continueBytes,
                 std::span<const std::byte> stopBytes, Offsets& out)
{
    out = {};
    if (continueBytes.size() < 94 || stopBytes.size() < 47) {
        return false;
    }

    const auto player = std::to_integer<unsigned char>(continueBytes[93]);
    std::int32_t until = 0;
    std::memcpy(&until, stopBytes.data() + 43, sizeof(until));
    if (player >= 0x40 || player % 8 != 0 || until < 0x40 || until > 0x400
        || until % 8 != 0) {
        return false;
    }
    out.player = player;
    out.until = static_cast<std::size_t>(until);
    return true;
}

bool shouldStop(const void* armedGm, const void* gameMode, std::int64_t until,
                std::int64_t now, const BlockPos& brokenPos, const BlockPos& nextPos)
{
    return armedGm != nullptr && armedGm == gameMode && until > now
        && (brokenPos.x != nextPos.x || brokenPos.y != nextPos.y || brokenPos.z != nextPos.z);
}

}
