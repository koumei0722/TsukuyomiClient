#include "config/Config.h"

#include "core/Logger.h"
#include "core/Notice.h"
#include "core/Paths.h"
#include "core/Strings.h"

#include <fstream>
#include <algorithm>
#include <mutex>
#include <set>

namespace tsukuyomi {

namespace {

void noteWrongType(std::string_view key)
{
    static std::mutex mutex;
    static std::set<std::string> told;
    {
        const std::lock_guard lock(mutex);
        if (!told.insert(std::string(key)).second) {
            return;
        }
    }
    log().error(L"Config: \"{}\" has the wrong type in Tsukuyomi.json; it is reset to its default", toUtf16(key));
    notice::failOnce("Config.wrongType",
                     L"Config: some values in Tsukuyomi.json have the wrong type (the first one is \""
                         + toUtf16(key) + L"\"); they are reset to their defaults",
                     "Some settings in Tsukuyomi.json have the wrong type and are reset to their defaults");
}

void noteUnreadable(const std::wstring& why)
{
    notice::failOnce("Config.unreadable",
                     L"Could not read the config file (" + why + L"). Running with the defaults and NOT saving, so "
                         L"the file is kept as it is (fix or delete it, then inject again)",
                     "Tsukuyomi.json could not be read: running with default settings, and changes are not saved");
}

}

Config& Config::instance()
{
    static Config config;
    return config;
}

void Config::load()
{
    m_root = nlohmann::json::object();
    m_claimed.clear();
    m_dropped.clear();
    m_unreadable = false;

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
            m_unreadable = true;
            noteUnreadable(L"not a JSON object");
            return;
        }
        m_root = std::move(parsed);
        m_written = m_root.dump(4);
        log().info(L"Config loaded");
    } catch (const nlohmann::json::exception& error) {
        m_unreadable = true;
        noteUnreadable(toUtf16(error.what()));
    }
}

bool Config::save()
{
    const auto path = paths::configFile();
    if (path.empty() || m_unreadable) {
        return false;
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        log().error(L"Could not write the config file");
        return false;
    }

    std::string text = m_root.dump(4);
    file << text;
    if (!file.good()) {
        log().error(L"Error while writing the config file");
        return false;
    }
    m_written = std::move(text);
    return true;
}

bool Config::saveIfChanged()
{
    if (m_root.dump(4) == m_written) {
        return false;
    }
    return save();
}

nlohmann::json& Config::section(std::string_view name)
{
    const std::string key(name);
    m_claimed.insert(key);

    const auto it = m_root.find(key);
    if (it == m_root.end() || !it->is_object()) {
        m_root[key] = nlohmann::json::object();
    }
    return m_root[key];
}

void Config::pruneUnclaimed()
{
    for (auto it = m_root.begin(); it != m_root.end();) {
        if (m_claimed.contains(it.key())) {
            ++it;
        } else {
            if (m_dropped.insert(it.key()).second) {
                log().info(L"Config: dropped the unknown section \"{}\"", toUtf16(it.key()));
            }
            it = m_root.erase(it);
        }
    }
}

bool Config::ensureBool(nlohmann::json& node, std::string_view key, bool fallback)
{
    const bool value = getBool(node, key, fallback);
    node[std::string(key)] = value;
    return value;
}

void Config::keepOnly(nlohmann::json& node, std::initializer_list<std::string_view> keys)
{
    for (auto it = node.begin(); it != node.end();) {
        if (std::find(keys.begin(), keys.end(), std::string_view(it.key())) != keys.end()) {
            ++it;
        } else {
            it = node.erase(it);
        }
    }
}

int Config::getInt(const nlohmann::json& node, std::string_view key, int fallback)
{
    const auto it = node.find(std::string(key));
    if (it == node.end()) {
        return fallback;
    }
    if (!it->is_number_integer()) {
        noteWrongType(key);
        return fallback;
    }
    return it->get<int>();
}

float Config::getFloat(const nlohmann::json& node, std::string_view key, float fallback)
{
    const auto it = node.find(std::string(key));
    if (it == node.end()) {
        return fallback;
    }
    if (!it->is_number()) {
        noteWrongType(key);
        return fallback;
    }
    return it->get<float>();
}

bool Config::getBool(const nlohmann::json& node, std::string_view key, bool fallback)
{
    const auto it = node.find(std::string(key));
    if (it == node.end()) {
        return fallback;
    }
    if (!it->is_boolean()) {
        noteWrongType(key);
        return fallback;
    }
    return it->get<bool>();
}

}
