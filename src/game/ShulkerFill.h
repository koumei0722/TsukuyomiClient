#pragma once

#include <functional>
#include <string>
#include <vector>

namespace tsukuyomi::shulkerfill {

struct Slot {
    int slot = -1;
    std::string name;
    int aux = 0;
    int count = 0;
};

inline constexpr int kShulkerSlots = 27;

int uniformFull(const std::vector<Slot>& items, int slots,
                const std::function<int(const std::string& name)>& maxOf);

}
