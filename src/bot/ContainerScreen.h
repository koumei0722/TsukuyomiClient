#pragma once
#include <cstdint>
#include <optional>
#include <string>

namespace tsukuyomi::bot::containerscreen {

using ContainerAtFn = void*(__fastcall*)(void* player, std::uint8_t type, const void* position);
using InRangeFn = bool(__fastcall*)(void* model, float pickRange);
using WeakRefFn = void*(__fastcall*)(void* model, void* out);
using NameAtFn = void*(__fastcall*)(void* model, void* out, const void* position);
using CustomNameAtFn = bool(__fastcall*)(void* controller, const void* position);

bool initialize();
void* containerAtTarget();
void* inRangeTarget();
void* weakRefTarget();
void* nameAtTarget();
void* customNameAtTarget();
void setHooked(bool hooked);
bool available();
void* onContainerAt(ContainerAtFn original, void* player, std::uint8_t type, const void* position);
bool onInRange(InRangeFn original, void* model, float pickRange);
void* onWeakRef(WeakRefFn original, void* model, void* out);
void* onNameAt(NameAtFn original, void* model, void* out, const void* position);
bool onCustomNameAt(CustomNameAtFn original, void* controller, const void* position);

std::optional<std::string> open(const std::string& title, std::uint8_t window, std::int32_t x, std::int32_t y, std::int32_t z);
bool ready();
bool gone();
void finish();
void poll();

}
