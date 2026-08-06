#pragma once

#include <string>
#include <string_view>

namespace tsukuyomi {

std::string toUtf8(std::wstring_view text);
std::wstring toUtf16(std::string_view text);

}
