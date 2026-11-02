#include "replay/BedrockLang.h"
#include <cctype>
#include <charconv>
#include <vector>

namespace tsukuyomi::replay {
void BedrockLang::addFile(std::string_view text) {
    if (text.starts_with("\xef\xbb\xbf")) text.remove_prefix(3);
    while (!text.empty()) {
        const auto end = text.find('\n'); auto line = text.substr(0, end);
        if (end == text.npos) text = {}; else text.remove_prefix(end + 1);
        if (line.ends_with('\r')) line.remove_suffix(1);
        if (line.starts_with('#')) continue;
        line = line.substr(0, line.find("	#"));
        while (line.ends_with('\t')) line.remove_suffix(1);
        const auto eq = line.find('=');
        if (eq != line.npos && eq) values_[std::string(line.substr(0, eq))] = line.substr(eq + 1);
    }
}
std::optional<std::string> BedrockLang::translate(std::string_view key, std::span<const std::string> params) const { return translateAt(key, params, 0); }
std::optional<std::string> BedrockLang::translateAt(std::string_view key, std::span<const std::string> params, unsigned depth) const {
    if (key.starts_with('%')) key.remove_prefix(1);
    const auto it = values_.find(key); if (it == values_.end()) return std::nullopt;
    std::vector<std::string> args(params.begin(), params.end());
    if (depth < 2) for (auto& arg : args) {
        unsigned level = depth;
        while (level < 2 && arg.starts_with('%')) { auto value = translateAt(arg, {}, ++level); if (!value) break; arg = *value; }
    }
    std::string out; const std::string_view format = it->second; std::size_t sequential = 0;
    for (std::size_t i = 0; i < format.size();) {
        if (format[i] != '%' || i + 1 == format.size()) { out += format[i++]; continue; }
        const auto start = i++;
        if (format[i] == '%') { out += '%'; ++i; continue; }
        std::size_t index = sequential;
        if (format[i] == 's') { ++sequential; ++i; }
        else if (format[i] >= '0' && format[i] <= '9') {
            const auto begin = i; while (i < format.size() && format[i] >= '0' && format[i] <= '9') ++i;
            std::size_t number = 0;
            const auto result = std::from_chars(format.data() + begin, format.data() + i, number);
            index = result.ec == std::errc{} && number ? number - 1 : args.size();
            if (i + 1 < format.size() && format[i] == '$' && std::isalpha(static_cast<unsigned char>(format[i + 1]))) i += 2;
        } else if (format[i] == 'd') { ++sequential; ++i; }
        else { out += '%'; continue; }
        if (index < args.size()) out += args[index]; else out += format.substr(start, i - start);
    }
    return out;
}
}
