#include "game/WorldIdentity.h"
#include "game/RejoinLogic.h"

#include <atomic>
#include <cstdio>
#include <mutex>

namespace tsukuyomi::worldidentity {
namespace {
std::mutex g_mutex;
std::atomic<std::uint64_t> g_entry{0};
Snapshot g_snapshot;
}

void onStartGame(std::string_view levelId, std::string_view name)
{
    std::lock_guard lock(g_mutex);
    g_snapshot.levelId = levelId;
    g_snapshot.name = name;
    g_snapshot.received = true;
    g_snapshot.entry = g_entry.fetch_add(1, std::memory_order_relaxed) + 1;
}

Snapshot snapshot()
{
    std::lock_guard lock(g_mutex);
    return g_snapshot;
}

void clear(std::uint64_t entry)
{
    std::lock_guard lock(g_mutex);
    if (g_snapshot.entry == entry) {
        g_snapshot.received = false;
        g_snapshot.levelId.clear();
        g_snapshot.name.clear();
    }
}

std::wstring sanitizeKeyPart(std::string_view text)
{
    std::wstring out;
    for (const unsigned char c : text) {
        if (out.size() == 80) break;
        const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
            || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        out.push_back(safe ? static_cast<wchar_t>(c) : L'_');
    }
    return out;
}

std::string worldKey(bool received, bool integrated, std::string_view levelId,
                     std::string_view name, std::string_view folder, std::string_view connectionUrl)
{
    const auto ascii = [](std::string_view text) {
        const auto safe = sanitizeKeyPart(text);
        return std::string(safe.begin(), safe.end());
    };
    if (integrated) {
        const auto id = received ? levelId : folder;
        return id.empty() ? std::string{} : "local-" + ascii(id);
    }
    const auto address = rejoin::parseConnectionUrl(connectionUrl);
    if (!address && !received) return {};
    const auto identity = address ? address->host + "\n" + std::to_string(address->port)
                                  : std::string(levelId) + "\n" + std::string(name);
    std::uint32_t hash = 2166136261u;
    for (const unsigned char c : identity) {
        hash = (hash ^ c) * 16777619u;
    }
    char suffix[10]{};
    std::snprintf(suffix, sizeof(suffix), "-%08x", hash);
    return "server-" + ascii(address ? std::string_view(address->host) : name) + suffix;
}

}
