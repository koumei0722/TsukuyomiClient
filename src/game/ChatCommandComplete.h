#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tsukuyomi::chatcommand {

struct CompletionEnum {
    std::string name;
    std::vector<std::string> values;
};

struct CompletionParam {
    std::string name;
    std::uint32_t enumIndex;
    bool optional;
};

struct TkCommandSpec {
    std::vector<CompletionEnum> enums;
    std::vector<std::vector<CompletionParam>> overloads;
};

std::vector<std::string> normalizeItemCandidates(const std::vector<std::string>& names);

TkCommandSpec buildTkCommandSpec(std::uint32_t firstEnumIndex, std::optional<std::uint32_t> existingItem,
                                 const std::vector<std::string>& items);

std::vector<std::string> itemEnumValues(const std::vector<std::string>& names);

}
