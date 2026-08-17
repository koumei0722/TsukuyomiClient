#include "input/Keys.h"

#include <Windows.h>

#include <algorithm>
#include <format>

namespace tsukuyomi::keys {

namespace {

bool isDown(int virtualKey)
{
    return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
}

}

int normalize(int virtualKey)
{
    switch (virtualKey) {
    case VK_LCONTROL:
    case VK_RCONTROL:
        return VK_CONTROL;
    case VK_LSHIFT:
    case VK_RSHIFT:
        return VK_SHIFT;
    case VK_LMENU:
    case VK_RMENU:
        return VK_MENU;
    default:
        return virtualKey;
    }
}

bool isModifier(int virtualKey)
{
    const int key = normalize(virtualKey);
    return key == VK_CONTROL || key == VK_SHIFT || key == VK_MENU;
}

std::wstring name(int virtualKey)
{
    switch (normalize(virtualKey)) {
    case VK_CONTROL:   return L"CTRL";
    case VK_SHIFT:     return L"SHIFT";
    case VK_MENU:      return L"ALT";
    case VK_SPACE:     return L"SPACE";
    case VK_RETURN:    return L"ENTER";
    case VK_ESCAPE:    return L"ESC";
    case VK_TAB:       return L"TAB";
    case VK_BACK:      return L"BACKSPACE";
    case VK_CAPITAL:   return L"CAPSLOCK";
    case VK_UP:        return L"↑";
    case VK_DOWN:      return L"↓";
    case VK_LEFT:      return L"←";
    case VK_RIGHT:     return L"→";
    case VK_INSERT:    return L"INSERT";
    case VK_DELETE:    return L"DELETE";
    case VK_HOME:      return L"HOME";
    case VK_END:       return L"END";
    case VK_PRIOR:     return L"PAGEUP";
    case VK_NEXT:      return L"PAGEDOWN";
    case VK_LBUTTON:   return L"MOUSE1";
    case VK_RBUTTON:   return L"MOUSE2";
    case VK_MBUTTON:   return L"MOUSE3";
    case VK_XBUTTON1:  return L"MOUSE4";
    case VK_XBUTTON2:  return L"MOUSE5";
    case VK_OEM_MINUS: return L"-";
    case VK_OEM_PLUS:  return L"+";
    case VK_OEM_COMMA: return L",";
    case VK_OEM_PERIOD:return L".";

    case VK_PAUSE:     return L"PAUSE";
    case VK_SNAPSHOT:  return L"PRINTSCREEN";
    case VK_SCROLL:    return L"SCROLLLOCK";
    case VK_NUMLOCK:   return L"NUMLOCK";
    case VK_APPS:      return L"MENU";
    case VK_LWIN:      return L"WIN";
    case VK_RWIN:      return L"WIN";
    case VK_MULTIPLY:  return L"NUM*";
    case VK_ADD:       return L"NUM+";
    case VK_SUBTRACT:  return L"NUM-";
    case VK_DECIMAL:   return L"NUM.";
    case VK_DIVIDE:    return L"NUM/";
    case VK_OEM_1:     return L";";
    case VK_OEM_2:     return L"/";
    case VK_OEM_3:     return L"@";
    case VK_OEM_4:     return L"[";
    case VK_OEM_5:     return L"\\";
    case VK_OEM_6:     return L"]";
    case VK_OEM_7:     return L"^";
    case VK_OEM_102:   return L"_";
    default:
        break;
    }

    const int key = normalize(virtualKey);

    if ((key >= '0' && key <= '9') || (key >= 'A' && key <= 'Z')) {
        return std::wstring(1, static_cast<wchar_t>(key));
    }
    if (key >= VK_F1 && key <= VK_F24) {
        return std::format(L"F{}", key - VK_F1 + 1);
    }
    if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9) {
        return std::format(L"NUM{}", key - VK_NUMPAD0);
    }

    return std::format(L"0x{:02X}", key);
}

std::wstring comboName(std::span<const int> combo)
{
    if (combo.empty()) {
        return L"unassigned";
    }

    std::wstring text;
    for (const int key : combo) {
        if (!text.empty()) {
            text += L" + ";
        }
        text += name(key);
    }
    return text;
}

bool isComboDown(std::span<const int> combo)
{
    if (combo.empty()) {
        return false;
    }
    return std::all_of(combo.begin(), combo.end(),
                       [](int key) { return isDown(key); });
}

}
