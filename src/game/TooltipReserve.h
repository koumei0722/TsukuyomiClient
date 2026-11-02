#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace tsukuyomi::tooltipreserve {

inline constexpr int kIdDigits = 8;

std::string reserveText(std::string_view marker, int rows, int spaces, std::uint32_t id);

bool findMarker(std::string_view text, std::string_view marker, int& line, int& lines, int& reserved,
                int& spaces, std::uint32_t& id);

int rowsFor(float lineHeight, float needHeight);

int spacesFor(int spaces, float innerWidth, float needWidth);

}
