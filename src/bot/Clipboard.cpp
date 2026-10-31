#include "Clipboard.h"

#include <windows.h>

namespace tsukuyomi::bot::clipboard {

bool putText(const std::string& utf8)
{
    const int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.c_str(), -1, nullptr, 0);
    if (chars <= 0) {
        return false;
    }
    const std::size_t bytes = static_cast<std::size_t>(chars) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory == nullptr) {
        return false;
    }
    wchar_t* const buffer = static_cast<wchar_t*>(GlobalLock(memory));
    if (buffer == nullptr) {
        GlobalFree(memory);
        return false;
    }
    const bool converted = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.c_str(), -1,
                                               buffer, chars) == chars;
    GlobalUnlock(memory);
    if (!converted) {
        GlobalFree(memory);
        return false;
    }
    if (!OpenClipboard(nullptr)) {
        GlobalFree(memory);
        return false;
    }
    const bool success = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
    CloseClipboard();
    if (!success) {
        GlobalFree(memory);
    }
    return success;
}

}
