#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tsukuyomi::chatcommand {

struct CompletionEnum {
    std::string name;
    std::vector<std::string> values;
};

enum class ParamType { Enum, Json, Int, Float, String };

struct CompletionParam {
    std::string name;
    std::uint32_t enumIndex;
    bool optional;
    ParamType type = ParamType::Enum;
};

struct TkCommandSpec {
    std::vector<CompletionEnum> enums;
    std::vector<std::vector<CompletionParam>> overloads;
};

struct SettingsCompletion {
    std::vector<std::string> modules, settings;
    std::vector<std::string> toggle, number, integer, choice, key, action;
    std::vector<std::string> choiceValues, keyValues;
};

bool isEnumWord(std::string_view value);

std::vector<std::string> normalizeItemCandidates(const std::vector<std::string>& names);

TkCommandSpec buildTkCommandSpec(std::uint32_t firstEnumIndex, std::optional<std::uint32_t> existingItem,
                                 const std::vector<std::string>& items, const SettingsCompletion& settings = {},
                                 std::optional<std::uint32_t> existingBoolean = std::nullopt);

std::vector<std::string> itemEnumValues(const std::vector<std::string>& names);

}
