#include "input/Keys.h"

#include <Windows.h>

#include <algorithm>
#include <cwctype>
#include <format>

namespace tsukuyomi::keys {

namespace {

bool isDown(int virtualKey)
{
    return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
}

}

std::vector<std::string> commandNames()
{
    std::vector<std::string> result;
    for (char c = 'a'; c <= 'z'; ++c) result.emplace_back(1, c);
    for (char c = '0'; c <= '9'; ++c) result.emplace_back(1, c);
    for (int i = 1; i <= 24; ++i) result.push_back("f" + std::to_string(i));
    for (int i = 0; i <= 9; ++i) result.push_back("num" + std::to_string(i));
    for (const char* value : {"ctrl", "shift", "alt", "space", "enter", "esc", "tab", "backspace",
         "capslock", "up", "down", "left", "right", "insert", "delete", "home", "end", "pageup",
         "pagedown", "mouse1", "mouse2", "mouse3", "mouse4", "mouse5", "pause", "printscreen",
         "scrolllock", "numlock", "menu", "win", "minus", "plus"}) result.emplace_back(value);
    return result;
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

namespace {

std::wstring upperTrimmed(const std::wstring& text)
{
    size_t a = 0;
    size_t b = text.size();
    while (a < b && iswspace(text[a])) {
        ++a;
    }
    while (b > a && iswspace(text[b - 1])) {
        --b;
    }
    std::wstring out;
    out.reserve(b - a);
    for (size_t i = a; i < b; ++i) {
        out.push_back(static_cast<wchar_t>(towupper(text[i])));
    }
    return out;
}

int keyFromToken(const std::wstring& t)
{
    static const std::pair<const wchar_t*, int> kNames[] = {
        {L"CTRL", VK_CONTROL},      {L"CONTROL", VK_CONTROL},  {L"LCTRL", VK_CONTROL},
        {L"RCTRL", VK_CONTROL},     {L"SHIFT", VK_SHIFT},      {L"LSHIFT", VK_SHIFT},
        {L"RSHIFT", VK_SHIFT},      {L"ALT", VK_MENU},         {L"LALT", VK_MENU},
        {L"RALT", VK_MENU},         {L"SPACE", VK_SPACE},      {L"ENTER", VK_RETURN},
        {L"RETURN", VK_RETURN},     {L"ESC", VK_ESCAPE},       {L"ESCAPE", VK_ESCAPE},
        {L"TAB", VK_TAB},           {L"BACKSPACE", VK_BACK},   {L"CAPSLOCK", VK_CAPITAL},
        {L"UP", VK_UP},             {L"DOWN", VK_DOWN},        {L"LEFT", VK_LEFT},
        {L"RIGHT", VK_RIGHT},       {L"\u2191", VK_UP},        {L"\u2193", VK_DOWN},
        {L"\u2190", VK_LEFT},       {L"\u2192", VK_RIGHT},     {L"INSERT", VK_INSERT},
        {L"DELETE", VK_DELETE},     {L"HOME", VK_HOME},        {L"END", VK_END},
        {L"PAGEUP", VK_PRIOR},      {L"PAGEDOWN", VK_NEXT},    {L"MOUSE1", VK_LBUTTON},
        {L"MOUSE2", VK_RBUTTON},    {L"MOUSE3", VK_MBUTTON},   {L"MOUSE4", VK_XBUTTON1},
        {L"MOUSE5", VK_XBUTTON2},   {L"LMB", VK_LBUTTON},      {L"RMB", VK_RBUTTON},
        {L"MMB", VK_MBUTTON},       {L"BUTTON_1", VK_LBUTTON}, {L"BUTTON_2", VK_RBUTTON},
        {L"BUTTON_3", VK_MBUTTON},  {L"BUTTON_4", VK_XBUTTON1}, {L"BUTTON_5", VK_XBUTTON2},
        {L"-", VK_OEM_MINUS},       {L"MINUS", VK_OEM_MINUS},  {L"PLUS", VK_OEM_PLUS},
        {L",", VK_OEM_COMMA},       {L".", VK_OEM_PERIOD},     {L"PAUSE", VK_PAUSE},
        {L"PRINTSCREEN", VK_SNAPSHOT}, {L"SCROLLLOCK", VK_SCROLL}, {L"NUMLOCK", VK_NUMLOCK},
        {L"MENU", VK_APPS},         {L"WIN", VK_LWIN},         {L"NUM*", VK_MULTIPLY},
        {L"NUM+", VK_ADD},          {L"NUM-", VK_SUBTRACT},    {L"NUM.", VK_DECIMAL},
        {L"NUM/", VK_DIVIDE},       {L";", VK_OEM_1},          {L"/", VK_OEM_2},
        {L"@", VK_OEM_3},           {L"[", VK_OEM_4},          {L"\\", VK_OEM_5},
        {L"]", VK_OEM_6},           {L"^", VK_OEM_7},          {L"_", VK_OEM_102},
    };
    for (const auto& [name, vk] : kNames) {
        if (t == name) {
            return vk;
        }
    }
    if (t.size() == 1 && ((t[0] >= L'0' && t[0] <= L'9') || (t[0] >= L'A' && t[0] <= L'Z'))) {
        return static_cast<int>(t[0]);
    }
    auto number = [&t](size_t from, int& out) {
        if (from >= t.size() || t.size() - from > 2) {
            return false;
        }
        int n = 0;
        for (size_t i = from; i < t.size(); ++i) {
            if (t[i] < L'0' || t[i] > L'9') {
                return false;
            }
            n = n * 10 + (t[i] - L'0');
        }
        out = n;
        return true;
    };
    int n = 0;
    if (t.size() >= 2 && t[0] == L'F' && number(1, n) && n >= 1 && n <= 24) {
        return VK_F1 + n - 1;
    }
    if (t.rfind(L"NUM", 0) == 0 && number(3, n) && n <= 9) {
        return VK_NUMPAD0 + n;
    }
    if (t.rfind(L"0X", 0) == 0 && t.size() > 2 && t.size() <= 4) {
        int v = 0;
        for (size_t i = 2; i < t.size(); ++i) {
            const wchar_t c = t[i];
            const int d = (c >= L'0' && c <= L'9') ? c - L'0' : (c >= L'A' && c <= L'F') ? c - L'A' + 10 : -1;
            if (d < 0) {
                return 0;
            }
            v = v * 16 + d;
        }
        return (v > 0 && v < 0xFF) ? v : 0;
    }
    return 0;
}

}

bool parseCombo(const std::wstring& text, std::vector<int>& out)
{
    const std::wstring all = upperTrimmed(text);
    if (all.empty() || all == L"UNASSIGNED" || all == L"NONE") {
        out.clear();
        return true;
    }
    std::vector<int> combo;
    size_t start = 0;
    while (start <= all.size()) {
        size_t plus = all.find(L'+', start);
        if (plus != std::wstring::npos && plus >= 3 && all.compare(plus - 3, 3, L"NUM") == 0
            && (plus == 3 || all[plus - 4] == L' ' || all[plus - 4] == L'+')) {
            plus = all.find(L'+', plus + 1);
        }
        const std::wstring token = upperTrimmed(all.substr(start, plus == std::wstring::npos ? std::wstring::npos : plus - start));
        if (token.empty()) {
            return false;
        }
        const int vk = keyFromToken(token);
        if (vk == 0) {
            return false;
        }
        if (std::find(combo.begin(), combo.end(), vk) == combo.end()) {
            combo.push_back(vk);
        }
        if (plus == std::wstring::npos) {
            break;
        }
        start = plus + 1;
    }
    out = std::move(combo);
    return true;
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
