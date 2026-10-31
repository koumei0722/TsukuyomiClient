#pragma once

#include <cstdint>
#include <string>

namespace tsukuyomi::jsonmem {

enum class Type : std::uint8_t { Null = 0, Int = 1, UInt = 2, Real = 3, String = 4, Bool = 5, Array = 6, Object = 7 };

using Value = const void*;

bool typeOf(Value value, Type& out);
Value member(Value object, const char* name);
Value element(Value array, std::uint32_t index);
bool count(Value container, std::uint32_t& out);
bool entry(Value object, std::uint32_t index, std::string& key, Value& value);
bool asNumber(Value value, double& out);
}
