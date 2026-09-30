#pragma once

#include <span>
#include <string>
#include <string_view>

namespace tsukuyomi::debugkeys {

std::string locationCommand(int dimensionId, double x, double y, double z,
                            double yaw, double pitch);
std::string setblockCommand(int x, int y, int z, std::string_view blockName,
                            std::span<const std::string_view> states);
std::string summonCommand(std::string_view entityName, double x, double y, double z);

}
