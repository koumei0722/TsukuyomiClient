#include "game/StackCount.h"

#include <cstdio>

namespace tsukuyomi::stackcount {

std::string formatStacks(std::size_t count, int stackSize)
{
    if (stackSize < 1) {
        stackSize = 1;
    }
    if (count == 0) {
        return "0";
    }
    const std::size_t box = static_cast<std::size_t>(perBox(stackSize));
    const std::size_t boxes = count / box;
    const std::size_t rest = count % box;
    const std::size_t stacks = (stackSize > 1) ? rest / static_cast<std::size_t>(stackSize) : 0;
    const std::size_t left =
        (stackSize > 1) ? rest % static_cast<std::size_t>(stackSize) : rest;

    std::string out;
    char buffer[32]{};
    const auto add = [&out, &buffer](std::size_t value, const char* unit) {
        if (!out.empty()) {
            out += '+';
        }
        std::snprintf(buffer, sizeof(buffer), "%llu%s", static_cast<unsigned long long>(value),
                      unit);
        out += buffer;
    };
    if (boxes != 0) {
        add(boxes, "SB");
    }
    if (stacks != 0) {
        add(stacks, "st");
    }
    if (left != 0 || out.empty()) {
        add(left, "");
    }
    return out;
}

}
