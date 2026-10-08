#include "input/PadKeys.h"

#include <algorithm>
#include <cctype>

namespace tsukuyomi::padkeys {
namespace {
std::string_view trim(std::string_view text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return text;
}
std::string upper(std::string_view text)
{
    std::string result(text);
    for (char& c : result) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return result;
}
}

bool isPadValue(std::string_view value)
{
    value = trim(value);
    return value.size() >= 4 && upper(value.substr(0, 4)) == "PAD_";
}

bool parseCombo(std::string_view text, std::vector<int>& out)
{
    text = trim(text);
    const auto whole = upper(text);
    if (whole.empty() || whole == "NONE" || whole == "UNASSIGNED" || whole == "PAD_NONE") {
        out.clear();
        return true;
    }
    std::vector<int> result;
    while (true) {
        const auto plus = text.find('+');
        std::string token = upper(trim(text.substr(0, plus)));
        if (token.starts_with("PAD_")) token.erase(0, 4);
        if (token == "DPAD_UP") token = "UP";
        else if (token == "DPAD_DOWN") token = "DOWN";
        else if (token == "DPAD_LEFT") token = "LEFT";
        else if (token == "DPAD_RIGHT") token = "RIGHT";
        else if (token == "SELECT" || token == "BACK") token = "VIEW";
        else if (token == "L3") token = "LS";
        else if (token == "R3") token = "RS";
        const auto it = std::find_if(kButtons.begin(), kButtons.end(), [&token](const Button& b) { return b.name == token; });
        if (it == kButtons.end()) return false;
        if (std::find(result.begin(), result.end(), it->value) == result.end()) result.push_back(it->value);
        if (result.size() > 4) return false;
        if (plus == std::string_view::npos) break;
        text.remove_prefix(plus + 1);
    }
    out = std::move(result);
    return true;
}

std::string comboName(std::span<const int> combo)
{
    if (combo.empty()) return "unassigned";
    std::string result;
    for (int value : combo) {
        const auto it = std::find_if(kButtons.begin(), kButtons.end(), [value](const Button& b) { return b.value == value; });
        if (!result.empty()) result += " + ";
        result += it == kButtons.end() ? "?" : it->name;
    }
    return result;
}
}
