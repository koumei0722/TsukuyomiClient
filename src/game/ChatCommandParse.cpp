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

RestockAction restockAction(const std::vector<std::string>& args)
{
    if (args.empty()) return RestockAction::Invalid;
    if (args.size() == 1 && args[0] == "list") return RestockAction::List;
    if (args.size() == 1 && args[0] == "clear") return RestockAction::Clear;
    if (args.size() <= 2 && args[0] == "add") return RestockAction::Add;
    if (args.size() <= 2 && args[0] == "remove") return RestockAction::Remove;
    return RestockAction::Invalid;
}

std::vector<std::string> rawWords(std::string_view text)
{
    std::vector<std::string> result;
    for (std::size_t at = 0; at < text.size();) {
        while (at < text.size() && space(text[at])) ++at;
        const std::size_t first = at;
        bool quoted = false, escaped = false;
        int depth = 0;
        while (at < text.size()) {
            const char c = text[at];
            if (!quoted && depth == 0 && space(c)) break;
            if (quoted) {
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"') quoted = false;
            } else if (c == '"') quoted = true;
            else if (c == '{' || c == '[') ++depth;
            else if ((c == '}' || c == ']') && depth > 0) --depth;
            ++at;
        }
        if (first != at) {
            std::string word(text.substr(first, at - first));
            result.push_back(std::move(word));
        }
    }
    return result;
}

std::vector<std::string> words(std::string_view text)
{
    auto result = rawWords(text);
    for (auto& word : result) std::transform(word.begin(), word.end(), word.begin(), lower);
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
