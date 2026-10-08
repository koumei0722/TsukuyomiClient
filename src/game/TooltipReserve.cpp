#include "game/TooltipReserve.h"

#include <algorithm>
#include <cmath>

namespace tsukuyomi::tooltipreserve {

namespace {

constexpr std::string_view kSection = "\xC2\xA7";

int hexValue(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool sectionCodeAt(std::string_view text, std::size_t at, char& code)
{
    if (at + kSection.size() >= text.size() || text.substr(at, kSection.size()) != kSection) {
        return false;
    }
    code = text[at + kSection.size()];
    return true;
}

bool readId(std::string_view text, std::size_t at, std::uint32_t& id, std::size_t& end)
{
    std::size_t i = at;
    std::uint32_t value = 0;
    char code = 0;
    for (int digit = 0; digit < kIdDigits; ++digit) {
        if (!sectionCodeAt(text, i, code) || hexValue(code) < 0) {
            return false;
        }
        value = (value << 4) | static_cast<std::uint32_t>(hexValue(code));
        i += kSection.size() + 1;
    }
    if (!sectionCodeAt(text, i, code) || code != 'r') {
        return false;
    }
    id = value;
    end = i + kSection.size() + 1;
    return true;
}

}

std::string reserveText(int rows, int spaces, std::uint32_t id)
{
    return reserveText(kMarker, rows, spaces, id);
}

std::string reserveText(std::string_view marker, int rows, int spaces, std::uint32_t id)
{
    rows = std::max(rows, 1);
    spaces = std::max(spaces, 0);
    std::string out(marker);
    static constexpr char kHex[] = "0123456789abcdef";
    for (int digit = kIdDigits - 1; digit >= 0; --digit) {
        out += kSection;
        out += kHex[(id >> (digit * 4)) & 0xFu];
    }
    out += kSection;
    out += 'r';
    out.append(static_cast<std::size_t>(spaces), ' ');
    out.append(static_cast<std::size_t>(rows), '\n');
    return out;
}

bool findMarker(std::string_view text, std::string_view marker, int& line, int& lines, int& reserved,
                int& spaces, std::uint32_t& id)
{
    if (marker.empty()) return false;
    std::size_t at = std::string_view::npos;
    std::size_t end = 0;
    for (std::size_t from = text.find(marker); from != std::string_view::npos;
         from = text.find(marker, from + 1)) {
        std::size_t after = 0;
        std::uint32_t value = 0;
        if (readId(text, from + marker.size(), value, after)) {
            at = from;
            end = after;
            id = value;
        }
    }
    if (at == std::string_view::npos) {
        return false;
    }
    line = static_cast<int>(std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(at), '\n'));
    lines = static_cast<int>(std::count(text.begin(), text.end(), '\n')) + 1;
    std::size_t i = end;
    spaces = 0;
    while (i < text.size() && text[i] == ' ') {
        ++spaces;
        ++i;
    }
    int newlines = 0;
    while (i < text.size() && text[i] == '\n') {
        ++newlines;
        ++i;
    }
    reserved = (i >= text.size()) ? newlines + 1 : std::max(newlines, 1);
    return true;
}

int rowsFor(float lineHeight, float needHeight)
{
    if (!(lineHeight > 0.5f) || !(needHeight > 0.0f)) {
        return 1;
    }
    return std::max(1, static_cast<int>(std::ceil(needHeight / lineHeight - 1.0e-3f)));
}

int spacesFor(int spaces, float innerWidth, float needWidth)
{
    spaces = std::max(spaces, 1);
    if (innerWidth >= needWidth || !(innerWidth > 0.0f)) {
        return spaces;
    }
    const float perSpace = innerWidth / static_cast<float>(spaces);
    const int want = static_cast<int>(std::ceil(needWidth / perSpace)) + 1;
    return std::max(want, spaces + 1);
}

}
