#pragma once
#include <cstdint>

namespace tsukuyomi::levelplayers {

const void* map();
bool uuidOf(std::int64_t uniqueId, std::uint8_t out[16]);
bool uuidOf(const void* players, std::int64_t uniqueId, std::uint8_t out[16]);

}
