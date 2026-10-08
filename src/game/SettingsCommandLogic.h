#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "ui/Menu.h"
#include "game/ChatCommandComplete.h"

namespace tsukuyomi::settingscommand {

std::string slug(std::wstring_view label);
struct SettingRef { std::string name; const MenuItem* item; };
std::vector<SettingRef> settingsOf(const MenuItem& module, bool includeHidden = false);
struct Result {
    std::vector<std::string> lines;
    bool changed = false;
    bool keysChanged = false;
    bool padChanged = false;
    bool error = false;
};
Result run(const std::vector<MenuItem>& tree, const std::vector<std::string>& args);
struct SettingInfo { std::string name; std::vector<std::string> values; MenuItemKind kind; };
struct ModuleInfo { std::string name; std::vector<SettingInfo> settings; };
std::vector<ModuleInfo> buildIndex(const std::vector<MenuItem>& tree);
chatcommand::SettingsCompletion completionOf(const std::vector<ModuleInfo>& index);
bool allowSuggestion(const std::vector<ModuleInfo>& index, const std::vector<std::string>& words,
                     std::size_t argument, std::string_view suggestion);

}
