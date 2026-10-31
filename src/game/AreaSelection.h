#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tsukuyomi::selection {
struct Box {
    std::string name;
    bool has1 = false, has2 = false;
    std::int32_t p1[3]{}, p2[3]{};
};
struct Area {
    std::string name = "Unnamed";
    std::vector<Box> boxes;
    int selected = -1;
    bool originSet = false;
    std::int32_t origin[3]{};
    bool boundsOf(std::int32_t lo[3], std::int32_t hi[3]) const;
    bool insideAnyBox(std::int32_t x, std::int32_t y, std::int32_t z) const;
    std::uint64_t blockCount() const;
};
std::string toJson(const Area& area);
bool fromJson(std::string_view text, Area& area);
}
