#include "BotCommandSyntax.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <iterator>
#include <nlohmann/json.hpp>

namespace tsukuyomi::bot::syntax {
namespace {
constexpr double kPi = 3.14159265358979323846;
const char* const kUsages[][2] = {
    {"login", "/tk bot login"},
    {"status", "/tk bot status"},
    {"accounts", "/tk bot accounts"},
    {"logout", "/tk bot logout <name>"},
    {"join", "/tk bot join <name> <0-3> [true|false] (0 = the Xbox account <name>, 1-3 = a bot in that seat of <name>;"
             " your gamertag = your account)"},
    {"leave", "/tk bot leave <name> <0-3>"},
    {"leaveall", "/tk bot leaveall"},
    {"switch", "/tk bot switch [<name> <0-3>]"},
    {"walk", "/tk bot walk <name> <0-3> <x y z> [path|straight] | walk <name> <0-3> follow"},
    {"stop", "/tk bot stop <name> <0-3>"},
    {"build", "/tk bot build <name> <0-3> <x y z> <down|up|north|south|west|east>"},
    {"eat", "/tk bot eat <name> <0-3>"},
    {"use", "/tk bot use <name> <0-3>"},
    {"hotbar", "/tk bot hotbar <name> <0-3> <1-9>"},
    {"inventory", "/tk bot inventory <name> <0-3>"},
};
std::optional<std::int32_t> integer(const Word& word)
{
    if (word.quoted || word.text.empty()) return std::nullopt;
    std::int32_t value = 0;
    const auto* begin = word.text.data();
    const auto* end = begin + word.text.size();
    if (*begin == '+') ++begin;
    const auto [last, error] = std::from_chars(begin, end, value);
    if (error != std::errc{} || last != end) return std::nullopt;
    return value;
}
bool is(const Word& word, const char* text) { return !word.quoted && word.text == text; }
std::optional<bool> boolean(const Word& word)
{
    if (is(word, "true")) return true;
    if (is(word, "false")) return false;
    return std::nullopt;
}
std::optional<double> number(std::string_view text, bool& isInteger)
{
    isInteger = text.find_first_of(".eE") == std::string_view::npos;
    if (text.empty()) return 0.0;
    if (text.front() == '+') text.remove_prefix(1);
    double value = 0;
    const auto [last, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || last != text.data() + text.size() || !std::isfinite(value)) return std::nullopt;
    return value;
}
template <typename Rest>
bool withTarget(const std::vector<Word>& words, Command& command, Rest rest)
{
    if (words.size() < 3) return false;
    const auto sub = integer(words[2]);
    if (!sub) return false;
    command.target = Target{words[1].value, *sub};
    return rest(command, std::size_t{3});
}
}
const char* const kSummary = "/tk bot login|status|accounts|logout|join|leave|leaveall|switch|walk|stop|build|eat|use|hotbar|inventory"
                             " (/tk bot <command> shows its form)";
std::optional<std::vector<Word>> toWords(const std::vector<std::string>& args)
{
    std::vector<Word> words;
    words.reserve(args.size());
    for (const auto& arg : args) {
        Word word{arg, arg, false};
        if (arg.starts_with('"')) {
            const auto parsed = nlohmann::json::parse(arg, nullptr, false);
            if (!parsed.is_string()) return std::nullopt;
            word.value = parsed.get<std::string>();
            word.quoted = true;
        }
        words.push_back(std::move(word));
    }
    return words;
}
std::optional<std::size_t> readPosition(const std::vector<Word>& words, std::size_t at, Position& out)
{
    std::size_t count = 0;
    for (std::size_t i = at; i < words.size() && count < 3; ++i) {
        if (words[i].quoted) return std::nullopt;
        const std::string_view text = words[i].text;
        std::size_t p = 0;
        while (p < text.size()) {
            if (count == 3) return std::nullopt;
            Coordinate& c = out[count];
            if (text[p] == '~' || text[p] == '^') {
                c.kind = text[p] == '~' ? Coordinate::Kind::Relative : Coordinate::Kind::Local;
                ++p;
            } else c.kind = Coordinate::Kind::Absolute;
            const auto next = text.find_first_of("~^", p);
            const auto piece = text.substr(p, next == std::string_view::npos ? std::string_view::npos : next - p);
            if (c.kind == Coordinate::Kind::Absolute && piece.empty()) return std::nullopt;
            const auto value = number(piece, c.integer);
            if (!value) return std::nullopt;
            c.value = *value;
            ++count;
            p = next == std::string_view::npos ? text.size() : next;
        }
        if (count == 3) {
            const auto local = std::count_if(out.begin(), out.end(), [](const Coordinate& c) { return c.kind == Coordinate::Kind::Local; });
            if (local != 0 && local != 3) return std::nullopt;
            return i - at + 1;
        }
    }
    return std::nullopt;
}
namespace {
std::array<double, 3> place(const Position& position, const Origin& origin, bool center)
{
    if (position[0].kind == Coordinate::Kind::Local) {
        const double yaw = origin.yaw * kPi / 180.0, pitch = origin.pitch * kPi / 180.0;
        const double forward[3] = {-std::sin(yaw) * std::cos(pitch), -std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
        const double up[3] = {-std::sin(yaw) * std::sin(pitch), std::cos(pitch), std::cos(yaw) * std::sin(pitch)};
        const double left[3] = {-(forward[1] * up[2] - forward[2] * up[1]), -(forward[2] * up[0] - forward[0] * up[2]),
                                -(forward[0] * up[1] - forward[1] * up[0])};
        const double feet[3] = {origin.feet.x, origin.feet.y, origin.feet.z};
        std::array<double, 3> result{};
        for (int i = 0; i < 3; ++i)
            result[i] = feet[i] + left[i] * position[0].value + up[i] * position[1].value + forward[i] * position[2].value;
        return result;
    }
    const double feet[3] = {origin.feet.x, origin.feet.y, origin.feet.z};
    std::array<double, 3> result{};
    for (int i = 0; i < 3; ++i) {
        const auto& c = position[i];
        if (c.kind == Coordinate::Kind::Relative) result[i] = feet[i] + c.value;
        else result[i] = c.value + (center && i != 1 && c.integer ? 0.5 : 0.0);
    }
    return result;
}
}
protocol::Vec3 resolve(const Position& position, const Origin& origin)
{
    const auto p = place(position, origin, true);
    return {static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2])};
}
std::array<std::int32_t, 3> resolveBlock(const Position& position, const Origin& origin)
{
    const auto p = place(position, origin, false);
    return {static_cast<std::int32_t>(std::floor(p[0])), static_cast<std::int32_t>(std::floor(p[1])), static_cast<std::int32_t>(std::floor(p[2]))};
}
std::vector<std::string> usage(const std::string& verb)
{
    for (const auto& [word, line] : kUsages)
        if (verb == word) return {line};
    std::vector<std::string> lines;
    for (const auto& [word, line] : kUsages) lines.emplace_back(line);
    return lines;
}
Parsed parse(const std::vector<Word>& words)
{
    if (words.empty() || words[0].quoted) return {std::nullopt, usage({})};
    const std::string& verb = words[0].text;
    const auto fail = [&] { return Parsed{std::nullopt, usage(verb)}; };
    const auto ok = [](Command command) { return Parsed{std::move(command), {}}; };
    const std::size_t n = words.size();
    Command command;
    if (verb == "login" || verb == "status" || verb == "accounts") {
        if (n != 1) return fail();
        command.verb = verb == "login" ? Verb::Login : verb == "status" ? Verb::Status : Verb::Accounts;
        return ok(command);
    }
    if (verb == "logout") {
        if (n != 2) return fail();
        command.verb = Verb::Logout;
        command.account = words[1].value;
        return ok(command);
    }
    if (verb == "leaveall") {
        if (n != 1) return fail();
        command.verb = Verb::LeaveAll;
        return ok(command);
    }
    if (verb == "leave") {
        command.verb = Verb::Leave;
        if (!withTarget(words, command, [&](Command&, std::size_t at) { return at == n; })) return fail();
        return ok(command);
    }
    if (verb == "switch") {
        command.verb = Verb::Switch;
        if (n == 1) return ok(command);
        if (!withTarget(words, command, [&](Command&, std::size_t at) { return at == n; })) return fail();
        return ok(command);
    }
    if (verb == "join") {
        const bool parsed = withTarget(words, command, [&](Command& c, std::size_t at) {
            if (at == n) return true;
            const auto flag = at + 1 == n ? boolean(words[at]) : std::nullopt;
            if (!flag) return false;
            c.classic = *flag;
            return true;
        });
        if (!parsed) return fail();
        const Target target = *command.target;
        command.target.reset();
        if (target.sub == 0) {
            command.verb = Verb::JoinAccount;
            command.account = target.name;
        } else {
            command.verb = Verb::JoinGuest;
            command.host = target.name;
            command.seat = target.sub;
        }
        return ok(command);
    }
    if (verb == "walk") {
        const bool parsed = withTarget(words, command, [&](Command& c, std::size_t at) {
            if (at + 1 == n && is(words[at], "follow")) { c.verb = Verb::Follow; return true; }
            c.verb = Verb::Walk;
            const auto used = readPosition(words, at, c.position);
            if (!used) return false;
            at += *used;
            if (at == n) { c.walkMode = WalkMode::Path; return true; }
            if (at + 1 != n) return false;
            if (is(words[at], "path")) c.walkMode = WalkMode::Path;
            else if (is(words[at], "straight")) c.walkMode = WalkMode::Straight;
            else return false;
            return true;
        });
        return parsed ? ok(command) : fail();
    }
    if (verb == "build") {
        command.verb = Verb::Build;
        const bool parsed = withTarget(words, command, [&](Command& c, std::size_t at) {
            const auto used = readPosition(words, at, c.position);
            if (!used || at + *used + 1 != n) return false;
            const auto& faceWord = words[at + *used];
            if (faceWord.quoted) return false;
            const auto face = std::find(std::begin(kFaces), std::end(kFaces), faceWord.text);
            if (face == std::end(kFaces)) return false;
            c.face = static_cast<std::uint8_t>(face - std::begin(kFaces));
            return true;
        });
        return parsed ? ok(command) : fail();
    }
    if (verb == "hotbar") {
        command.verb = Verb::Hotbar;
        const bool parsed = withTarget(words, command, [&](Command& c, std::size_t at) {
            if (at + 1 != n) return false;
            const auto slot = integer(words[at]);
            if (!slot) return false;
            c.slot = *slot;
            return true;
        });
        return parsed ? ok(command) : fail();
    }
    if (verb == "stop" || verb == "eat" || verb == "use" || verb == "inventory") {
        command.verb = verb == "stop" ? Verb::Stop : verb == "eat" ? Verb::Eat : verb == "use" ? Verb::Use : Verb::Inventory;
        if (!withTarget(words, command, [&](Command&, std::size_t at) { return at == n; })) return fail();
        return ok(command);
    }
    return {std::nullopt, usage({})};
}
}
