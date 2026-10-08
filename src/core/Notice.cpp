#include "core/Notice.h"

#include "core/Logger.h"
#include "game/ClientChat.h"

#include <mutex>
#include <set>
#include <vector>

namespace tsukuyomi::notice {

namespace {

std::mutex g_mutex;
std::set<std::string> g_told;
std::vector<std::string> g_pending;

}

void failOnce(const char* key, const std::wstring& logText, const std::string& chatText)
{
    {
        const std::lock_guard lock(g_mutex);
        if (key == nullptr || !g_told.insert(key).second) {
            return;
        }
        if (g_pending.size() < 16) {
            g_pending.push_back("\xC2\xA7" "b[Tsukuyomi]" "\xC2\xA7" "c " + chatText + "\xC2\xA7" "r (see Tsukuyomi.log)");
        }
    }
    log().write(LogLevel::Error, logText);
}

void pump()
{
    std::vector<std::string> pending;
    {
        const std::lock_guard lock(g_mutex);
        if (g_pending.empty()) {
            return;
        }
        pending.swap(g_pending);
    }
    std::vector<std::string> left;
    for (std::string& text : pending) {
        if (!clientchat::printLocal(text)) {
            left.push_back(std::move(text));
        }
    }
    if (!left.empty()) {
        const std::lock_guard lock(g_mutex);
        g_pending.insert(g_pending.begin(), std::make_move_iterator(left.begin()), std::make_move_iterator(left.end()));
    }
}

}
