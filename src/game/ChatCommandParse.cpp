#include "game/ChatCommandParse.h"

#include <algorithm>
#include <cctype>

namespace tsukuyomi::chatcommand {
namespace {
bool space(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }
char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
}

bool isTsukuyomiCommand(std::string_view text)
{
    while (!text.empty() && space(text.front())) text.remove_prefix(1);
    return text.size() >= 3 && text[0] == '/'
           && lower(text[1]) == 't' && lower(text[2]) == 'k'
           && (text.size() == 3 || space(text[3]));
}

std::vector<std::string> words(std::string_view text)
{
    std::vector<std::string> result;
    for (std::size_t at = 0; at < text.size();) {
        while (at < text.size() && space(text[at])) ++at;
        const std::size_t first = at;
        while (at < text.size() && !space(text[at])) ++at;
        if (first != at) {
            std::string word(text.substr(first, at - first));
            std::transform(word.begin(), word.end(), word.begin(), lower);
            result.push_back(std::move(word));
        }
    }
    return result;
}

std::string normalizeItem(std::string_view text)
{
    std::string name(text);
    std::transform(name.begin(), name.end(), name.begin(), lower);
    if (name.find(':') == std::string::npos) name.insert(0, "minecraft:");
    return name;
}

std::vector<std::string> listLines(const std::vector<std::string>& names, std::size_t maxLines)
{
    if (names.empty()) return {"No items are excluded"};
    std::vector<std::string> lines;
    for (std::size_t i = 0; i < names.size() && lines.size() < maxLines; i += 8) {
        std::string line = i == 0 ? "HandRestock exclusions (" + std::to_string(names.size()) + "): " : "";
        for (std::size_t j = i; j < std::min(i + 8, names.size()); ++j) {
            if (j != i) line += ", ";
            line += names[j];
        }
        lines.push_back(std::move(line));
    }
    return lines;
}
}
