#pragma once

#include <functional>
#include <string>
#include <vector>

#include "game/ChatCommandComplete.h"
#include "ui/Menu.h"

namespace tsukuyomi::settingscommand {

void registerCommand();
void loadPadKeys();
void savePadKeys();
chatcommand::SettingsCompletion completion();
void addExtra(std::wstring moduleLabel, std::function<std::vector<MenuItem>()> factory);

}
