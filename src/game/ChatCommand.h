#pragma once

#include <cstdint>
#include <functional>
#include <string_view>
#include <string>
#include <vector>

namespace tsukuyomi::chatcommand {

using Handler = std::function<std::vector<std::string>(const std::vector<std::string>&)>;
using SuggestionFilter =
    std::function<bool(const std::vector<std::string>& words, std::size_t argument, std::string_view suggestion)>;
void registerModuleCommand(std::vector<std::string> names, std::string usage, Handler handler,
                           SuggestionFilter filter = {});
bool intercept(const void* args, std::int32_t* out);
void pump();
void noteCompletionText(const void* text);
bool completingTsukuyomiArguments();
bool allowTsukuyomiSuggestion(const void* suggestion);

}
