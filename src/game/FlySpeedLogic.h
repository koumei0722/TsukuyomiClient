#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace tsukuyomi::flyspeed {
struct FlySpeedRequest {
    enum class Kind { Show, Set, Reset } kind = Kind::Show;
    std::string target;
    float horizontal = 0.05f, vertical = 1.0f;
    bool hasVertical = false;
};
bool parseFlySpeedArgs(const std::vector<std::string>& rawArgs, FlySpeedRequest& out, std::string& error);
int matchPlayerName(const std::vector<std::string>& names, std::string_view query);
std::string formatSpeed(float value);
std::string speedReply(std::string_view name, float horizontal, float vertical, bool show, bool saved);
std::string missingPlayerReply(std::string_view query);
inline constexpr const char* kOwnPlayerOnly = "On this server only your own fly speed can be changed";
}
