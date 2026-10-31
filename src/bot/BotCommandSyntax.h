#pragma once
#include "BotProtocol.h"
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tsukuyomi::bot::syntax {
struct Word {
    std::string text, value;
    bool quoted = false;
};
std::optional<std::vector<Word>> toWords(const std::vector<std::string>& args);

struct Coordinate {
    enum class Kind : std::uint8_t { Absolute, Relative, Local };
    Kind kind = Kind::Absolute;
    double value = 0;
    bool integer = false;
};
using Position = std::array<Coordinate, 3>;
std::optional<std::size_t> readPosition(const std::vector<Word>& words, std::size_t at, Position& out);
struct Origin {
    protocol::Vec3 feet;
    float pitch = 0, yaw = 0;
};
protocol::Vec3 resolve(const Position& position, const Origin& origin);
std::array<std::int32_t, 3> resolveBlock(const Position& position, const Origin& origin);

struct Target {
    std::string name;
    std::int32_t sub = 0;
};
enum class Verb : std::uint8_t {
    Login, Status, Accounts, Logout, Leave, LeaveAll, Switch, JoinAccount, JoinGuest,
    Walk, Follow, Stop, Build, Eat, Use, Hotbar, Inventory,
};
enum class WalkMode : std::uint8_t { Path, Straight };
struct Command {
    Verb verb = Verb::Status;
    std::optional<Target> target;
    std::string account;
    std::string host;
    std::int32_t seat = 0;
    bool classic = false;
    Position position{};
    WalkMode walkMode = WalkMode::Path;
    std::uint8_t face = 0;
    std::int32_t slot = 0;
};
struct Parsed {
    std::optional<Command> command;
    std::vector<std::string> error;
};
Parsed parse(const std::vector<Word>& words);
std::vector<std::string> usage(const std::string& verb);
extern const char* const kSummary;
inline constexpr const char* kFaces[] = {"down", "up", "north", "south", "west", "east"};
}
