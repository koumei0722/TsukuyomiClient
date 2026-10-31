#pragma once
#include "game/ChatCommandComplete.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tsukuyomi::bot {
inline constexpr const char* kUserLabel = "your account";
std::string guestKey(const std::string& host, std::uint8_t sub);
bool isGuest(const std::string& name);
inline constexpr const char* kAccountSoftEnum = "TsukuyomiBotAccount";
std::vector<std::string> accountEnumValues(const std::vector<std::string>& names);
void addCompletion(chatcommand::TkCommandSpec& spec, std::uint32_t firstEnumIndex, const std::vector<std::string>& accounts,
                   std::optional<std::uint32_t> booleanIndex = std::nullopt);
}
