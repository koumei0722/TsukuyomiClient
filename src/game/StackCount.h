#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace tsukuyomi::stackcount {

int maxStackSize(std::string_view name);

constexpr int kSlotsPerBox = 27;
inline int perBox(int stackSize) { return kSlotsPerBox * stackSize; }

std::string formatStacks(std::size_t count, int stackSize);

std::string formatStacksOf(std::size_t count, std::string_view name);

}
