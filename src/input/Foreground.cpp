#include "input/Foreground.h"

namespace tsukuyomi::input {

namespace {

bool belongsToThisProcess(HWND window)
{
    if (window == nullptr) {
        return false;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    return processId == GetCurrentProcessId();
}

}

bool isGameForeground(HWND ownWindow)
{
    const HWND foreground = GetForegroundWindow();
    if (foreground == nullptr) {
        return false;
    }

    if (ownWindow != nullptr && (foreground == ownWindow || IsChild(ownWindow, foreground))) {
        return true;
    }

    if (belongsToThisProcess(foreground)) {
        return true;
    }

    const HWND core = FindWindowExA(foreground, nullptr, "Windows.UI.Core.CoreWindow", nullptr);
    return belongsToThisProcess(core);
}

}
