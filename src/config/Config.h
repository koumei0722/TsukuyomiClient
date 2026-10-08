#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <initializer_list>
#include <set>

namespace tsukuyomi {

class Config {
public:
    static Config& instance();

    void load();
    bool save();
    bool saveIfChanged();

    nlohmann::json& section(std::string_view name);

    void pruneUnclaimed();
    static bool ensureBool(nlohmann::json& node, std::string_view key, bool fallback);
    static void keepOnly(nlohmann::json& node, std::initializer_list<std::string_view> keys);

    static int getInt(const nlohmann::json& node, std::string_view key, int fallback);
    static float getFloat(const nlohmann::json& node, std::string_view key, float fallback);
    static bool getBool(const nlohmann::json& node, std::string_view key, bool fallback);

private:
    Config() = default;

    nlohmann::json m_root = nlohmann::json::object();
    std::set<std::string, std::less<>> m_claimed;
    std::set<std::string, std::less<>> m_dropped;
    std::string m_written;
    bool m_unreadable = false;
};

}
