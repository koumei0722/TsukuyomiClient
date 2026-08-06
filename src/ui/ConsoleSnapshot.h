#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "core/Logger.h"
#include "ui/Menu.h"

namespace tsukuyomi {

struct MenuRow {
    MenuItemKind kind = MenuItemKind::Action;

    std::wstring label;
    std::wstring value;

    bool available = true;
    bool hasState = false;
    bool on = false;
};

struct ConsoleSnapshot {

    static constexpr size_t kNoRow = static_cast<size_t>(-1);

    std::wstring title;
    Menu::Mode mode = Menu::Mode::Normal;
    bool atRoot = true;
    bool closeHovered = false;

    std::vector<MenuRow> rows;

    float selectionRow = 0.0f;

    bool selectionVisible = true;

    size_t selectedRow = kNoRow;

    std::wstring promptLabel;
    std::wstring promptBody;

    std::vector<LogEntry> logs;
    size_t logScroll = 0;
    bool showLog = false;

    float originX = 0.0f;
    float originY = 0.0f;

    float cursorX = 0.0f;
    float cursorY = 0.0f;
    bool cursorValid = false;
};

}
