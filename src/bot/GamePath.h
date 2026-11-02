#pragma once
#include "BotSession.h"
#include <optional>
#include <string>

namespace tsukuyomi::bot::gamepath {

bool initialize();
bool available();
struct Found {
    std::vector<PathNode> nodes;
    bool complete = false;
};
std::optional<Found> find(protocol::Vec3 feet, PathNode target, float radius, bool inLiquid, std::string& error);
std::optional<Found> findIn(void* level, void* dimension, void* source, protocol::Vec3 feet, PathNode target, float radius, bool inLiquid,
                            std::string& error);
std::optional<bool> inLiquid(protocol::Vec3 feet);
std::optional<bool> inLiquidIn(void* level, void* dimension, void* source, protocol::Vec3 feet);
std::optional<std::int32_t> playerDimension();

}
