#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace tsukuyomi::chatcommand {

bool isTsukuyomiCommand(std::string_view text);
std::vector<std::string> words(std::string_view text);
std::string normalizeItem(std::string_view text);
std::vector<std::string> listLines(const std::vector<std::string>& names,
                                   std::size_t maxLines = 12);

}
