#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace tsukuyomi {

class Config {
public:
    static Config& instance();

    void load();
    bool save();

    nlohmann::json& section(std::string_view name);

    static int getInt(const nlohmann::json& node, std::string_view key, int fallback);
    static float getFloat(const nlohmann::json& node, std::string_view key, float fallback);
    static bool getBool(const nlohmann::json& node, std::string_view key, bool fallback);

private:
    Config() = default;

    nlohmann::json m_root = nlohmann::json::object();
};

}
