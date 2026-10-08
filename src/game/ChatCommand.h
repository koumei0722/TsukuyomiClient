#pragma once

#include <cstdint>
#include <functional>
#include <string_view>
#include <string>
#include <vector>

namespace tsukuyomi::chatcommand {

struct Reply { std::vector<std::string> lines; bool error = false; };
using Handler = std::function<Reply(const std::vector<std::string>&)>;
using RawHandler = std::function<Reply(const std::vector<std::string>& args,
                                                        const std::vector<std::string>& rawArgs)>;
using SuggestionFilter =
    std::function<bool(const std::vector<std::string>& words, std::size_t argument, std::string_view suggestion)>;
void registerModuleCommand(std::vector<std::string> names, std::string usage, Handler handler,
                           SuggestionFilter filter = {});
void registerRawCommand(std::vector<std::string> names, std::string usage, RawHandler handler,
                        SuggestionFilter filter = {}, bool handlesNoArgs = false);
bool intercept(const void* args, std::int32_t* out);
void pump();
void postNotice(std::string line);
void noteCompletionText(const void* text);
bool completingTsukuyomiArguments();
bool allowTsukuyomiSuggestion(const void* suggestion);

}
