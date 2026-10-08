#pragma once

#include <string>

namespace tsukuyomi::notice {

void failOnce(const char* key, const std::wstring& logText, const std::string& chatText);

void pump();

}
