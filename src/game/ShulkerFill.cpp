#include "game/ShulkerFill.h"

namespace tsukuyomi::shulkerfill {

int uniformFull(const std::vector<Slot>& items, int slots,
                const std::function<int(const std::string& name)>& maxOf)
{
    if (slots <= 0 || static_cast<int>(items.size()) != slots || !maxOf) {
        return -1;
    }
    std::vector<bool> seen(static_cast<std::size_t>(slots), false);
    const Slot& first = items.front();
    if (first.name.empty()) {
        return -1;
    }
    const int max = maxOf(first.name);
    if (max <= 0) {
        return -1;
    }
    for (const Slot& one : items) {
        if (one.slot < 0 || one.slot >= slots || seen[static_cast<std::size_t>(one.slot)]) {
            return -1;
        }
        seen[static_cast<std::size_t>(one.slot)] = true;
        if (one.name != first.name || one.aux != first.aux || one.count != max) {
            return -1;
        }
    }
    return 0;
}

}
