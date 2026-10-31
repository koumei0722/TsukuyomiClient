#pragma once

#include <cstdint>
#include "game/MaterialRules.h"

namespace tsukuyomi::pickitem {
struct Result {
    materialrules::Item item;
    std::int32_t iconIdAux = 0;
    bool hasIcon = false;
};
Result ofBlock(const void* block);
}
