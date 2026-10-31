#pragma once
#include <cstdint>
#include <optional>
#include <string>

namespace tsukuyomi::bot::friendworld {

bool initialize();
bool available();
void setGuarded(bool guarded);
using FindFn = void(__fastcall*)(void* finder, const std::string* id, void* callback);
void __fastcall onFind(FindFn original, void* finder, const std::string* id, void* callback);
void expectRejoin(std::uint64_t netherNetId, std::uint64_t untilMs);
void cancelRejoin();
enum class Lookup { Pending, Matched, Missing };
Lookup takeLookup();
std::optional<std::string> idFor(void* game, std::uint64_t netherNetId);
bool leaveThenJoin(void* game, const std::string& id);
}
