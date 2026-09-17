#include "game/StackCount.h"

#include <cstdio>

namespace tsukuyomi::stackcount {

namespace {

std::string_view bare(std::string_view name)
{
    const std::size_t colon = name.rfind(':');
    return (colon == std::string_view::npos) ? name : name.substr(colon + 1);
}

bool endsWith(std::string_view text, std::string_view tail)
{
    return text.size() >= tail.size() && text.compare(text.size() - tail.size(), tail.size(), tail) == 0;
}

bool contains(std::string_view text, std::string_view part)
{
    return text.find(part) != std::string_view::npos;
}

}

int maxStackSize(std::string_view name)
{
    const std::string_view body = bare(name);
    if (body.empty()) {
        return 64;
    }
    if (body == "bed" || endsWith(body, "_bed")) {
        return 1;
    }
    if (contains(body, "shulker_box")) {
        return 1;
    }
    if (contains(body, "sign")) {
        return 16;
    }
    if (contains(body, "banner")) {
        return 16;
    }
    return 64;
}

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

std::string formatStacksOf(std::size_t count, std::string_view name)
{
    return formatStacks(count, maxStackSize(name));
}

}
