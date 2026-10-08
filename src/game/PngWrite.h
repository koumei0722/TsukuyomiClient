#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tsukuyomi::png {

std::vector<std::uint8_t> encodeRgba(const std::uint8_t* rgba, int width, int height);

}
