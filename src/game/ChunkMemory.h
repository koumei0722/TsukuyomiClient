#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace tsukuyomi::chunkmem {
inline constexpr std::ptrdiff_t kChunkSourceOriginX = 0x140, kChunkSourceOriginZ = 0x148, kChunkSourceWidth = 0x158;
inline constexpr std::ptrdiff_t kLevelChunkPosition = 0x78, kLevelChunkMinY = 0x64, kLevelChunkMaxY = 0x70;
inline constexpr std::ptrdiff_t kSubChunkSkyLight = 0x08, kSubChunkBlockLight = 0x10;
inline constexpr std::ptrdiff_t kLevelChunkBiomesBegin = 0x3e0, kLevelChunkBiomesEnd = 0x3e8, kBiomeName = 0x198;
inline constexpr std::ptrdiff_t kLevelChunkSubChunks = 0x1c8, kSubChunkBytes = 0x68, kSubChunkStorage = 0x20;
struct ChunkRef { int cx, cz; void* chunk; };
struct ChunkShape { int minY, maxY; std::uint8_t* table; int subCount; void* biomeBegin; int biomeCount; };
struct SubView { void* storage[2]; const std::uint8_t* sky; const std::uint8_t* block; std::int32_t status; };
void* gridChunkAt(void* region, int x, int z);
void* subChunkInChunk(std::uint8_t* chunk, int y, bool& aboveTop);
const void* storageGet(void* storage, unsigned int index);
bool readLightNibble(const void* subChunk, std::ptrdiff_t at, int index, int& out);
bool readGameString(const void* at, char* out, std::size_t cap);
void* chunkSource(void* region);
bool listChunks(void* region, std::vector<ChunkRef>& out);
bool shapeOf(void* chunk, ChunkShape& out);
bool viewSub(std::uint8_t* sub, SubView& out);
bool readStorage(void* storage, const void* out[4096]);
bool copyLight(const std::uint8_t* array, std::uint8_t out[2048]);
const void* biomeAt(void* chunk, int x, int y, int z);
bool readBiomes(void* chunk, int section, const void* out[64]);
bool biomeName(const void* biome, char* out, std::size_t cap);
std::int32_t readDayTime(void* region);
}
