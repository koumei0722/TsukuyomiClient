#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tsukuyomi::replay::jetables {
std::uint8_t stateFlags(int javaState);
int blockEntityType(int javaState);
int waterlogged(int javaState);
int bedrockToJava(std::string_view name, const std::vector<std::pair<std::string, std::string>>& states);
int biomeOf(std::string_view bedrockName);
std::span<const unsigned char> configPackets();
}
