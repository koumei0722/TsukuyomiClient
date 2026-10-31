#pragma once
#include <cstdint>
#include <optional>

namespace tsukuyomi::bot {
struct LocalPlayerIds { std::uint64_t runtimeId = 0; std::int64_t uniqueId = 0; };
std::optional<LocalPlayerIds> readLocalPlayerIds();
std::optional<std::uint64_t> readActorRuntimeId(const void* actor);
}
