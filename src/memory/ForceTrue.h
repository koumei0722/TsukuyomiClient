#pragma once

#include <cstddef>
#include <vector>

namespace tsukuyomi::memory {

bool makeForceTrueBytes(const std::byte* at, std::size_t& sizeOut,
                        std::vector<std::byte>& out);

bool makeForceTrueInstruction(const std::byte* at, std::size_t& sizeOut, std::vector<std::byte>& out);

}
