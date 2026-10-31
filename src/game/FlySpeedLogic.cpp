#include "game/FlySpeedLogic.h"

#include <charconv>
#include <cmath>
#include <iterator>

namespace tsukuyomi::flyspeed {
namespace {
char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; }
bool equalFold(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) if (lower(a[i]) != lower(b[i])) return false;
    return true;
}
bool number(std::string_view text, float maximum, float& out)
{
    const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size()
        && std::isfinite(out) && out >= 0 && out <= maximum;
}
}

bool parseFlySpeedArgs(const std::vector<std::string>& rawArgs, FlySpeedRequest& out, std::string& error)
{
    error.clear();
    out = {};
    if (rawArgs.empty() || rawArgs.size() > 3) {
        error = "Usage: /tk flyspeed <player> [<horizontal> [vertical] | reset]";
        return false;
    }
    std::string_view target = rawArgs[0];
    if (!target.empty() && target.front() == '"') {
        if (target.size() < 2 || target.back() != '"') {
            error = "Invalid quoted player name";
            return false;
        }
        target.remove_prefix(1);
        target.remove_suffix(1);
        for (std::size_t i = 0; i < target.size(); ++i) {
            char c = target[i];
            if (c == '\\') {
                if (++i == target.size() || (target[i] != '\\' && target[i] != '"')) {
                    error = "Invalid quoted player name";
                    return false;
                }
                c = target[i];
            } else if (c == '"') {
                error = "Invalid quoted player name";
                return false;
            }
            out.target += c;
        }
    } else {
        if (target.find('"') != std::string_view::npos) {
            error = "Invalid quoted player name";
            return false;
        }
        out.target = target;
    }
    if (out.target.empty()) { error = "A player name is required"; return false; }
    if (rawArgs.size() == 1) return true;
    if (equalFold(rawArgs[1], "reset")) {
        if (rawArgs.size() != 2) { error = "reset takes no speed arguments"; return false; }
        out.kind = FlySpeedRequest::Kind::Reset;
        return true;
    }
    out.kind = FlySpeedRequest::Kind::Set;
    if (!number(rawArgs[1], 2.0f, out.horizontal)) {
        error = "Horizontal fly speed must be a number from 0 to 2";
        return false;
    }
    if (rawArgs.size() == 3) {
        out.hasVertical = true;
        if (!number(rawArgs[2], 20.0f, out.vertical)) {
            error = "Vertical fly speed must be a number from 0 to 20";
            return false;
        }
    }
    return true;
}

int matchPlayerName(const std::vector<std::string>& names, std::string_view query)
{
    for (std::size_t i = 0; i < names.size(); ++i) if (names[i] == query) return static_cast<int>(i);
    int match = -1;
    for (std::size_t i = 0; i < names.size(); ++i) if (equalFold(names[i], query)) {
        if (match != -1) return -1;
        match = static_cast<int>(i);
    }
    return match;
}

std::string formatSpeed(float value)
{
    char buffer[64]{};
    const auto result = std::to_chars(std::begin(buffer), std::end(buffer), value, std::chars_format::fixed, 4);
    if (result.ec != std::errc{}) return {};
    std::string text(buffer, result.ptr);
    while (text.back() == '0') text.pop_back();
    if (text.back() == '.') text.pop_back();
    return text == "-0" ? "0" : text;
}

std::string speedReply(std::string_view name, float horizontal, float vertical, bool show, bool saved)
{
    return "Fly speed of " + std::string(name) + ": horizontal " + formatSpeed(horizontal)
        + ", vertical " + formatSpeed(vertical) + (show ? " (default 0.05, 1)"
        : saved ? " (saved in this world)" : " (this server: your client only, not saved)");
}
std::string missingPlayerReply(std::string_view query) { return "No player named " + std::string(query) + " in this world"; }
}
