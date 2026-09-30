#include "game/GameOptions.h"

#include <charconv>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string_view>
#include <system_error>
#include <Windows.h>

namespace tsukuyomi::gameoptions {
namespace {

std::mutex g_mutex;
Options g_options;
bool g_loaded = false;

std::string_view trimmed(std::string_view value)
{
    const auto first = value.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
}

std::filesystem::path appData()
{
    const DWORD length = GetEnvironmentVariableW(L"APPDATA", nullptr, 0);
    if (length < 2) return {};
    std::wstring buffer(length, L'\0');
    const DWORD copied = GetEnvironmentVariableW(L"APPDATA", buffer.data(), length);
    if (copied == 0 || copied >= length) return {};
    buffer.resize(copied);
    return std::filesystem::path(buffer);
}

bool numericId(const std::wstring& name)
{
    if (name.empty()) return false;
    for (const wchar_t c : name) {
        if (c < L'0' || c > L'9') return false;
    }
    return true;
}

std::filesystem::path latestOptionsFile()
{
    const auto base = appData();
    if (base.empty()) return {};
    const auto users = base / L"Minecraft Bedrock" / L"Users";
    std::error_code ec;
    std::filesystem::directory_iterator it(users, ec);
    if (ec) return {};
    std::filesystem::path latest;
    std::filesystem::file_time_type latestTime{};
    const std::filesystem::directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        const auto& entry = *it;
        if (!entry.is_directory(ec)) { ec.clear(); continue; }
        if (!numericId(entry.path().filename().wstring())) continue;
        const auto candidate = entry.path() / L"games" / L"com.mojang" / L"minecraftpe" / L"options.txt";
        if (!std::filesystem::is_regular_file(candidate, ec)) { ec.clear(); continue; }
        const auto time = std::filesystem::last_write_time(candidate, ec);
        if (ec) { ec.clear(); continue; }
        if (latest.empty() || time > latestTime) { latest = candidate; latestTime = time; }
    }
    return latest;
}

}

const std::string* Options::find(const char* key) const
{
    if (!key || !*key) return nullptr;
    for (const auto& [name, value] : entries_) {
        if (name == key) return &value;
    }
    return nullptr;
}

bool Options::getInt(const char* key, int& out) const
{
    const auto* value = find(key);
    if (!value) return false;
    const auto s = trimmed(*value);
    if (s.empty()) return false;
    int parsed = 0;
    const auto result = std::from_chars(s.data(), s.data() + s.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != s.data() + s.size()) return false;
    out = parsed;
    return true;
}

bool Options::getFloat(const char* key, float& out) const
{
    const auto* value = find(key);
    if (!value) return false;
    const auto s = trimmed(*value);
    if (s.empty()) return false;
    float parsed = 0.0f;
    const auto result = std::from_chars(s.data(), s.data() + s.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != s.data() + s.size()) return false;
    out = parsed;
    return true;
}

bool Options::getString(const char* key, char* out, std::size_t cap) const
{
    const auto* value = find(key);
    if (!value || !out || cap <= value->size()) return false;
    std::memcpy(out, value->data(), value->size());
    out[value->size()] = '\0';
    return true;
}

bool parseOptions(const char* text, std::size_t length, Options& out)
{
    if (!text && length != 0) return false;
    Options parsed;
    const std::string_view input(text ? text : "", length);
    std::size_t pos = 0;
    while (pos < input.size()) {
        const auto end = input.find('\n', pos);
        auto line = input.substr(pos, end == std::string_view::npos ? end : end - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (pos == 0 && line.size() >= 3 && line.substr(0, 3) == "\xEF\xBB\xBF") line.remove_prefix(3);
        const auto colon = line.find(':');
        if (colon != std::string_view::npos && colon != 0) {
            const auto key = trimmed(line.substr(0, colon));
            if (!key.empty()) {
                const auto value = line.substr(colon + 1);
                bool replaced = false;
                for (auto& [name, stored] : parsed.entries_) {
                    if (name == key) { stored.assign(value); replaced = true; break; }
                }
                if (!replaced) parsed.entries_.emplace_back(key, value);
            }
        }
        if (end == std::string_view::npos) break;
        pos = end + 1;
    }
    out = std::move(parsed);
    return true;
}

bool reload()
{
    const auto file = latestOptionsFile();
    Options parsed;
    bool success = false;
    if (!file.empty()) {
        std::error_code ec;
        const auto size = std::filesystem::file_size(file, ec);
        if (!ec && size <= 1024 * 1024) {
            std::ifstream stream(file, std::ios::binary);
            if (stream) {
                std::string contents(static_cast<std::size_t>(size), '\0');
                if (stream.read(contents.data(), static_cast<std::streamsize>(contents.size())) || contents.empty()) {
                    success = parseOptions(contents.data(), contents.size(), parsed);
                }
            }
        }
    }
    std::lock_guard lock(g_mutex);
    g_options = std::move(parsed);
    g_loaded = success;
    return success;
}

bool getInt(const char* key, int& out)
{
    std::lock_guard lock(g_mutex);
    return g_loaded && g_options.getInt(key, out);
}

}
