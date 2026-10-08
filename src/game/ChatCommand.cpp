#include "game/ChatCommand.h"

#include <optional>
#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <atomic>
#include <mutex>
#include <utility>

#include "core/Logger.h"
#include "core/Notice.h"
#include "core/Strings.h"
#include "game/ChatCommandParse.h"
#include "game/ClientChat.h"
#include "game/CommandSyntax.h"

namespace tsukuyomi::chatcommand {
namespace {
struct Command {
    std::vector<std::string> names;
    std::string usage;
    RawHandler handler;
    SuggestionFilter filter;
    bool handlesNoArgs = false;
};
std::vector<Command> g_commands;
std::mutex g_commandsMutex;

std::optional<Command> findCommand(const std::string& name)
{
    std::lock_guard lock(g_commandsMutex);
    const auto found = std::find_if(g_commands.begin(), g_commands.end(), [&](const Command& entry) {
        return std::find(entry.names.begin(), entry.names.end(), name) != entry.names.end();
    });
    if (found == g_commands.end()) return std::nullopt;
    return *found;
}
std::mutex g_queueMutex;
std::vector<std::string> g_queue;
std::vector<std::string> g_notices;
bool g_overflowLogged = false;
int g_loggedCommands = 0;
int g_loggedReplies = 0;
std::atomic<bool> g_completingTk{false};
std::mutex g_completionMutex;
std::vector<std::string> g_completionWords;
std::size_t g_completionArgument = 0;
std::vector<std::string> g_shownSuggestions;

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

void reply(const std::string& line, bool error = false)
{
    const std::string text = error ? "\xC2\xA7" "c" + line
                                   : "\xC2\xA7" "b[Tsukuyomi]" "\xC2\xA7" "r " + line;
    if (!clientchat::printLocal(text) && g_loggedReplies < 200) {
        ++g_loggedReplies;
        log().info(L"ChatCommands: {}", toUtf16(text));
    }
}
}

void registerModuleCommand(std::vector<std::string> names, std::string usage, Handler handler,
                           SuggestionFilter filter)
{
    registerRawCommand(std::move(names), std::move(usage),
        [handler = std::move(handler)](const auto& args, const auto&) { return handler(args); },
        std::move(filter));
}

void registerRawCommand(std::vector<std::string> names, std::string usage, RawHandler handler,
                        SuggestionFilter filter, bool handlesNoArgs)
{
    std::lock_guard lock(g_commandsMutex);
    g_commands.push_back({std::move(names), std::move(usage), std::move(handler), std::move(filter), handlesNoArgs});
}

bool intercept(const void* args, std::int32_t* out)
{
    if (args == nullptr) return false;
    std::size_t length = 0;
    if (!lengthGuarded(args, length)) return false;
    std::string text(length, '\0');
    if (!copyGuarded(args, text.data(), length) || !isTsukuyomiCommand(text)) return false;
    {
        std::string message;
        const auto checked = commandsyntax::check(text, message);
        if (checked == commandsyntax::Result::SyntaxError) {
            clientchat::printLocal("\xC2\xA7" "c" + message);
            if (out != nullptr) *out = 0;
            return true;
        }
        if (checked == commandsyntax::Result::Unavailable)
            notice::failOnce("ChatCommand.syntax", L"ChatCommand: /tk syntax checking is unavailable",
                             "Syntax checking for /tk is unavailable");
    }
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
    {
        std::lock_guard lock(g_completionMutex);
        g_shownSuggestions.clear();
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
    if (g_completionWords.size() >= 2) {
        const std::optional<Command> found = findCommand(g_completionWords[1]);
        if (found && found->filter && !found->filter(g_completionWords, g_completionArgument, text)) return false;
    }
    if (std::find(g_shownSuggestions.begin(), g_shownSuggestions.end(), text) != g_shownSuggestions.end()) return false;
    g_shownSuggestions.push_back(std::move(text));
    return true;
}

bool completingTsukuyomiArguments()
{
    return g_completingTk.load(std::memory_order_relaxed);
}

void postNotice(std::string line)
{
    std::lock_guard lock(g_queueMutex);
    if (g_notices.size() < 8) g_notices.push_back(std::move(line));
}

void pump()
{
    std::vector<std::string> queued;
    std::vector<std::string> notices;
    {
        std::lock_guard lock(g_queueMutex);
        queued.swap(g_queue);
        notices.swap(g_notices);
    }
    for (const auto& line : notices) reply(line);
    for (const auto& command : queued) {
        if (g_loggedCommands < 200) {
            ++g_loggedCommands;
            log().info(L"ChatCommands: received {}", toUtf16(command));
        }
        const auto tokens = words(command);
        if (tokens.size() > 2 && tokens[1] == "help") {
            reply("Usage: /tk [help]", true);
            continue;
        }
        if (tokens.size() == 1 || tokens[1] == "help") {
            reply("/tk help");
            std::vector<std::string> usages;
            {
                std::lock_guard lock(g_commandsMutex);
                for (std::size_t i = 0; i < g_commands.size() && i < 11; ++i) usages.push_back(g_commands[i].usage);
            }
            for (const auto& usage : usages) reply(usage);
            continue;
        }
        const std::optional<Command> found = findCommand(tokens[1]);
        if (!found) {
            reply("Unknown command: " + command + " (try /tk help)", true);
            continue;
        }
        const std::vector<std::string> args(tokens.begin() + 2, tokens.end());
        const auto rawTokens = rawWords(command);
        const std::vector<std::string> rawArgs(rawTokens.begin() + 2, rawTokens.end());
        const auto response = args.empty() && !found->handlesNoArgs ? Reply{{found->usage}, true}
                                                                  : found->handler(args, rawArgs);
        for (std::size_t i = 0; i < response.lines.size() && i < 24; ++i) reply(response.lines[i], response.error);
    }
}
}
