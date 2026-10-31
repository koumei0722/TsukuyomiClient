#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace tsukuyomi::materialrules {

using States = std::map<std::string, std::string>;
struct Item {
    std::string name;
    std::size_t count = 0;
    bool noItem = false;
};
States parseStates(const std::vector<std::string>& lines);
bool isLiquid(std::string_view name);
std::vector<Item> itemsFor(std::string_view block, const States& states, const Item& picked,
                          std::string_view layer1 = {}, const States& layer1States = {});

}
