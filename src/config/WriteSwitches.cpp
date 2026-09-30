#include "config/WriteSwitches.h"

#include "core/Logger.h"
#include "core/Paths.h"
#include "core/Strings.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <set>
#include <unordered_map>

namespace tsukuyomi::writes {

namespace {

std::unordered_map<std::string, bool> g_values;
std::vector<std::pair<std::string, bool>> g_readHooks;
std::vector<std::pair<std::string, bool>> g_readPatches;
std::vector<std::string> g_off;
bool g_fileExisted = false;
bool g_fileBroken = false;
std::vector<std::string> g_invalid;
std::atomic<bool> g_loaded{false};

std::mutex g_unknownLock;
std::set<std::string> g_unknownNoted;

std::string readWholeFile(const std::filesystem::path& path, bool& existed)
{
    existed = false;
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return {};
    }
    existed = true;
    std::string text;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < (4 << 20)) {
        text.resize(static_cast<std::size_t>(size.QuadPart));
        DWORD got = 0;
        if (!ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &got, nullptr)) {
            text.clear();
        } else {
            text.resize(got);
        }
    }
    CloseHandle(file);
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB
        && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    return text;
}

bool isKnown(const std::string& name)
{
    for (const Entry& e : table()) {
        if (name == e.name) {
            return true;
        }
    }
    return false;
}

bool isRetired(const std::string& name)
{
    return !isKnown(name);
}

}

void load()
{
    if (g_loaded.exchange(true)) {
        return;
    }
    try {
        const std::filesystem::path path = paths::hooksFile();
        if (path.empty()) {
            return;
        }
        const std::string text = readWholeFile(path, g_fileExisted);
        if (!g_fileExisted) {
            return;
        }
        if (!parse(text, g_readHooks, g_readPatches, &g_invalid)) {
            g_fileBroken = true;
            return;
        }
        std::erase_if(g_invalid, [](const std::string& key) {
            const std::size_t dot = key.find('.');
            return !isKnown(dot == std::string::npos ? key : key.substr(dot + 1));
        });
        for (const auto& [name, on] : g_readHooks) {
            if (!isRetired(name)) {
                g_values[name] = on;
            }
        }
        for (const auto& [name, on] : g_readPatches) {
            if (!isRetired(name)) {
                g_values[name] = on;
            }
        }
        for (const auto& [name, on] : g_values) {
            if (!on) {
                g_off.push_back(name);
            }
        }
        std::sort(g_off.begin(), g_off.end());
    } catch (...) {
        g_values.clear();
        g_off.clear();
        g_fileBroken = true;
    }
}

bool allowed(std::string_view name)
{
    if (g_off.empty()) {
        return true;
    }
    const auto it = g_values.find(std::string(name));
    return it == g_values.end() || it->second;
}

bool allowed(const wchar_t* name)
{
    if (name == nullptr || g_off.empty()) {
        return true;
    }
    std::string ascii;
    for (const wchar_t* p = name; *p != 0; ++p) {
        ascii += static_cast<char>(*p < 0x80 ? *p : '?');
    }
    return allowed(ascii);
}

bool blocked(std::string_view feature)
{
    return !g_off.empty() && blockedBy(feature, g_off);
}

std::vector<std::string> disabledNames()
{
    return g_off;
}

void noteUnknown(std::string_view name)
{
    const std::string key(name);
    if (isKnown(key)) {
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(g_unknownLock);
        if (!g_unknownNoted.insert(key).second) {
            return;
        }
    }
    log().warn(L"WriteSwitches: \"{}\" is not in the table of hooks.json (add it to WriteSwitchesCore.cpp)",
               toUtf16(key));
}

void finishStartup()
{
    const std::filesystem::path path = paths::hooksFile();
    if (g_fileBroken) {
        log().warn(L"hooks.json could not be read; every hook and patch stays on (fix or delete {})",
                   path.wstring());
        return;
    }
    std::vector<std::pair<std::string, bool>> known;
    bool missing = false;
    std::size_t retired = 0;
    for (const auto& [name, on] : g_readHooks) {
        if (isRetired(name)) {
            ++retired;
            continue;
        }
        known.emplace_back(name, on);
    }
    for (const auto& [name, on] : g_readPatches) {
        if (isRetired(name)) {
            ++retired;
            continue;
        }
        known.emplace_back(name, on);
    }
    for (const Entry& e : table()) {
        if (g_values.find(e.name) == g_values.end()) {
            missing = true;
        }
    }
    if (!g_off.empty()) {
        std::wstring list;
        for (const std::string& name : g_off) {
            if (!list.empty()) {
                list += L", ";
            }
            list += toUtf16(name);
        }
        log().warn(L"hooks.json: {} write(s) turned off: {}", g_off.size(), list);
    } else {
        log().info(L"hooks.json: every hook and patch is on");
    }
    for (const std::string& name : g_invalid) {
        log().warn(L"hooks.json: \"{}\" is not true/false; it stays on (write true or false)", toUtf16(name));
    }
    if (g_fileExisted && !missing && retired == 0) {
        return;
    }
    if (!g_invalid.empty()) {
        log().warn(L"hooks.json is not updated with the missing names until the values above are fixed");
        return;
    }
    const std::string text = render(known, {}, {});
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        log().warn(L"hooks.json could not be written ({})", path.wstring());
        return;
    }
    DWORD wrote = 0;
    const bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr) != 0
                    && wrote == text.size();
    CloseHandle(file);
    log().info(L"hooks.json {} ({} names)", g_fileExisted ? L"was updated" : L"was created", table().size());
    if (retired != 0) {
        log().info(L"hooks.json: dropped {} name(s) that are no longer used", retired);
    }
    (void)ok;
}

}
