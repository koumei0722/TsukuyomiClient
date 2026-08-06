#include "core/Strings.h"

#include <Windows.h>

#include <limits>

namespace tsukuyomi {

namespace {

bool fitsInInt(size_t size)
{
    return size <= static_cast<size_t>((std::numeric_limits<int>::max)());
}

}

std::string toUtf8(std::wstring_view text)
{
    if (text.empty() || !fitsInInt(text.size())) {
        return {};
    }

    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return {};
    }

    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring toUtf16(std::string_view text)
{
    if (text.empty() || !fitsInInt(text.size())) {
        return {};
    }

    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0);
    if (length <= 0) {
        return {};
    }

    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        result.data(), length);
    return result;
}

}
