#pragma once
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace tsukuyomi::replay {
class BedrockLang {
public:
    void addFile(std::string_view text);
    bool empty() const { return values_.empty(); }
    std::optional<std::string> translate(std::string_view key, std::span<const std::string> params) const;
private:
    std::optional<std::string> translateAt(std::string_view key, std::span<const std::string> params, unsigned depth) const;
    std::map<std::string, std::string, std::less<>> values_;
};
}
