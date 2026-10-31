#pragma once
#include <cstdint>
#include <optional>
#include <string>

namespace tsukuyomi::bot::gameidentity {

bool initialize();
void* ctorTarget();
using CtorFn = void*(__fastcall*)(void* self, void* key, void* token);
void* onConstruct(CtorFn original, void* self, void* key, void* token);
bool held();
std::optional<std::string> assertWith(const std::string& fingerprints);
std::optional<std::string> token();
void armTokenSwap(std::string token);
void disarmTokenSwap();
void release();

}
