#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace tsukuyomi::gamestring {

using DeleteFn = void(__fastcall*)(void* ptr, std::size_t size);

void configure(void** allocatorAt, DeleteFn gameDelete);
bool available();
bool read(void* str, std::string& text);
bool assign(void* str, std::string_view text);
bool release(void* str);
bool releaseVector(void* vec);

}
