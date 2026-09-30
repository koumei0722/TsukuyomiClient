#include "game/ChatCommand.h"

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <atomic>
#include <mutex>
#include <utility>

#include "core/Logger.h"
#include "core/Strings.h"
#include "game/ChatCommandParse.h"
#include "game/ClientChat.h"

namespace tsukuyomi::chatcommand {
namespace {
struct Command {
    std::vector<std::string> names;
    std::string usage;
    Handler handler;
    SuggestionFilter filter;
};
std::vector<Command> g_commands;
std::mutex g_queueMutex;
std::vector<std::string> g_queue;
bool g_overflowLogged = false;
int g_loggedCommands = 0;
int g_loggedReplies = 0;
std::atomic<bool> g_completingTk{false};
std::mutex g_completionMutex;
std::vector<std::string> g_completionWords;
std::size_t g_completionArgument = 0;

bool copyGuarded(const void* args, char* dst, std::size_t length)
{
    __try {
        const auto* bytes = static_cast<const unsigned char*>(args);
        const std::size_t storedLength = *reinterpret_cast<const std::size_t*>(bytes + 0x10);
        const std::size_t capacity = *reinterpret_cast<const std::size_t*>(bytes + 0x18);
        if (storedLength != length) return false;
        const char* source = capacity >= 16 ? *reinterpret_cast<const char* const*>(bytes)
                                            : reinterpret_cast<const char*>(bytes);
        std::memcpy(dst, source, length);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                    || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
                    ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

bool lengthGuarded(const void* args, std::size_t& length)
{
    __try {
        length = *reinterpret_cast<const std::size_t*>(static_cast<const unsigned char*>(args) + 0x10);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                    || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
                    ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

void reply(const std::string& line)
{
    const std::string text = "\xC2\xA7" "b[Tsukuyomi]" "\xC2\xA7" "r " + line;
    if (!clientchat::printLocal(text) && g_loggedReplies < 200) {
        ++g_loggedReplies;
        log().info(L"ChatCommands: {}", toUtf16(text));
    }
}
}

void registerModuleCommand(std::vector<std::string> names, std::string usage, Handler handler,
                           SuggestionFilter filter)
{
    g_commands.push_back({std::move(names), std::move(usage), std::move(handler), std::move(filter)});
}

bool intercept(const void* args, std::int32_t* out)
{
    if (args == nullptr) return false;
    std::size_t length = 0;
    if (!lengthGuarded(args, length)) return false;
    std::string text(length, '\0');
    if (!copyGuarded(args, text.data(), length) || !isTsukuyomiCommand(text)) return false;
    {
        std::lock_guard lock(g_queueMutex);
        if (g_queue.size() < 16) g_queue.push_back(std::move(text));
        else if (!g_overflowLogged) {
            g_overflowLogged = true;
            log().warn(L"ChatCommands: the command queue is full");
        }
    }
    if (out != nullptr) *out = 0;
    return true;
}

void noteCompletionText(const void* text)
{
    bool tk = false;
    std::size_t length = 0;
    std::string copyText;
    if (text != nullptr && lengthGuarded(text, length) && length >= 4 && length < 512) {
        std::string copy(length, '\0');
        tk = copyGuarded(text, copy.data(), length) && isTsukuyomiCommand(copy)
             && copy.find(' ', copy.find_first_not_of(' ')) != std::string::npos;
        if (tk) copyText = std::move(copy);
    }
    if (tk) {
        std::lock_guard lock(g_completionMutex);
        g_completionWords = words(copyText);
        const bool trailingSpace = !copyText.empty() && copyText.back() == ' ';
        g_completionArgument = g_completionWords.empty() ? 0
                               : g_completionWords.size() - (trailingSpace ? 0 : 1);
    }
    g_completingTk.store(tk, std::memory_order_relaxed);
}

bool allowTsukuyomiSuggestion(const void* suggestion)
{
    std::size_t length = 0;
    if (suggestion == nullptr || !lengthGuarded(suggestion, length) || length == 0 || length >= 256) return true;
    std::string text(length, ' ');
    if (!copyGuarded(suggestion, text.data(), length)) return true;
    if (text.starts_with("minecraft:")) return false;
    std::lock_guard lock(g_completionMutex);
    if (g_completionWords.size() < 2) return true;
    const auto found = std::find_if(g_commands.begin(), g_commands.end(), [](const Command& entry) {
        return std::find(entry.names.begin(), entry.names.end(), g_completionWords[1]) != entry.names.end();
    });
    if (found == g_commands.end() || !found->filter) return true;
    return found->filter(g_completionWords, g_completionArgument, text);
}

bool completingTsukuyomiArguments()
{
    return g_completingTk.load(std::memory_order_relaxed);
}

void pump()
{
    std::vector<std::string> queued;
    {
        std::lock_guard lock(g_queueMutex);
        queued.swap(g_queue);
    }
    for (const auto& command : queued) {
        if (g_loggedCommands < 200) {
            ++g_loggedCommands;
            log().info(L"ChatCommands: received {}", toUtf16(command));
        }
        const auto tokens = words(command);
        if (tokens.size() == 1 || tokens[1] == "help") {
            reply("/tk help");
            for (std::size_t i = 0; i < g_commands.size() && i < 11; ++i) reply(g_commands[i].usage);
            continue;
        }
        auto found = std::find_if(g_commands.begin(), g_commands.end(), [&](const Command& entry) {
            return std::find(entry.names.begin(), entry.names.end(), tokens[1]) != entry.names.end();
        });
        if (found == g_commands.end()) {
            reply("Unknown command: " + command + " (try /tk help)");
            continue;
        }
        const std::vector<std::string> args(tokens.begin() + 2, tokens.end());
        const auto lines = args.empty() ? std::vector<std::string>{found->usage} : found->handler(args);
        for (std::size_t i = 0; i < lines.size() && i < 12; ++i) reply(lines[i]);
    }
}
}
