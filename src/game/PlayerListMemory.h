#pragma once

#include <cstdint>

namespace tsukuyomi::playerlist::mem {
inline constexpr int kMaxPlayerEntries = 4096, kMaxScoreEntries = 65536;
struct RawPlayer { std::int64_t actorId; char name[97]; std::uint32_t nameLength; const void* skin; };
int readPlayerMap(const void* map, RawPlayer* out, int cap);
bool findListObjective(const void* scoreboard, const void** outObjective);
bool readScores(const void* scoreboard, const void* objective, const std::int64_t* ids, int count,
                bool* has, int* score);

inline constexpr int kMaxFaceSide = 32;
bool readFace(const void* skin, std::uint8_t* out, int maxSide, int& side);
bool readAttribute(const std::uint8_t* attributes, std::uint32_t id, float& current, float& maximum);
}
