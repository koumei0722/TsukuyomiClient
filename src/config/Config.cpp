#include "config/Config.h"

#include "core/Logger.h"
#include "core/Paths.h"
#include "core/Strings.h"

#include <fstream>

namespace tsukuyomi {

Config& Config::instance()
{
    static Config config;
    return config;
}

void Config::load()
{
    m_root = nlohmann::json::object();

    const auto path = paths::configFile();
    if (path.empty()) {
        log().warn(L"Could not resolve the config path. Using defaults");
        return;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        log().info(L"No config file found. Using defaults");
        return;
    }

    try {
        nlohmann::json parsed = nlohmann::json::parse(file);
        if (!parsed.is_object()) {
            log().warn(L"Config file is not an object. Using defaults");
            return;
        }
        m_root = std::move(parsed);
        log().info(L"Config loaded");
    } catch (const nlohmann::json::exception& error) {

        log().warn(L"Could not read the config file ({}). Using defaults",
                   toUtf16(error.what()));
    }
}

bool Config::save()
{
    const auto path = paths::configFile();
    if (path.empty()) {
        return false;
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        log().error(L"Could not write the config file");
        return false;
    }

    file << m_root.dump(4);
    if (!file.good()) {
        log().error(L"Error while writing the config file");
        return false;
    }
    return true;
}

nlohmann::json& Config::section(std::string_view name)
{
    const std::string key(name);

    const auto it = m_root.find(key);
    if (it == m_root.end() || !it->is_object()) {
        m_root[key] = nlohmann::json::object();
    }
    return m_root[key];
}

void Config::eraseSection(std::string_view name)
{
    m_root.erase(std::string(name));
}

int Config::getInt(const nlohmann::json& node, std::string_view key, int fallback)
{
    const auto it = node.find(std::string(key));
    if (it == node.end() || !it->is_number_integer()) {
        return fallback;
    }
    return it->get<int>();
}

float Config::getFloat(const nlohmann::json& node, std::string_view key, float fallback)
{
    const auto it = node.find(std::string(key));
    if (it == node.end() || !it->is_number()) {
        return fallback;
    }
    return it->get<float>();
}

bool Config::getBool(const nlohmann::json& node, std::string_view key, bool fallback)
{
    const auto it = node.find(std::string(key));
    if (it == node.end() || !it->is_boolean()) {
        return fallback;
    }
    return it->get<bool>();
}

}
