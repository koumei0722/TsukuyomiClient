#pragma once

#include <cstddef>
#include <cstdint>

namespace tsukuyomi::itemicon {

constexpr std::int32_t blockItemIdFromLegacyId(std::uint16_t raw)
{
    return raw <= 255 ? static_cast<std::int32_t>(raw) : 255 - static_cast<std::int32_t>(raw);
}

constexpr std::int32_t packIdAux(std::int32_t id, std::int32_t aux)
{
    const std::uint32_t high = static_cast<std::uint32_t>(id) << 16;
    const std::uint32_t low = static_cast<std::uint32_t>(aux) & 0x7FFFu;
    return static_cast<std::int32_t>(high | low);
}

inline constexpr std::ptrdiff_t kOffsetNotFound = -1;
inline constexpr std::ptrdiff_t kOffsetAmbiguous = -2;

inline std::ptrdiff_t findUniqueU16Offset(const std::uint8_t* const* images,
                                          const std::uint16_t* want, std::size_t count,
                                          std::size_t bytes, std::size_t from, std::size_t to,
                                          std::size_t step)
{
    if (images == nullptr || want == nullptr || count == 0 || step == 0) {
        return kOffsetNotFound;
    }
    std::ptrdiff_t found = kOffsetNotFound;
    for (std::size_t at = from; at + 2 <= to && at + 2 <= bytes; at += step) {
        bool all = true;
        for (std::size_t i = 0; i < count && all; ++i) {
            if (images[i] == nullptr) {
                all = false;
                break;
            }
            const std::uint16_t got = static_cast<std::uint16_t>(
                images[i][at] | (static_cast<std::uint16_t>(images[i][at + 1]) << 8));
            all = (got == want[i]);
        }
        if (!all) {
            continue;
        }
        if (found != kOffsetNotFound) {
            return kOffsetAmbiguous;
        }
        found = static_cast<std::ptrdiff_t>(at);
    }
    return found;
}

}
