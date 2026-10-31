#pragma once

#include <cstddef>
#include <cstdint>

namespace tsukuyomi::blockwrite {

void noteRenderRegion(void* region);
void* renderRegion();
std::uint64_t regionGeneration();
bool regionIsAlive(void* region);

void* findChunk(void* region, int x, int z);
void* findSubChunk(void* region, int x, int y, int z);
bool lastFindWasMissingChunk();
bool lastFindWasOutsideWorld();
bool lastFindWasAboveTop();

void setKnownBlocks(const void* const* blocks, std::size_t count);
inline constexpr unsigned beIndexOf(int x, int y, int z)
{
    return static_cast<unsigned>(((x & 15) << 8) | ((z & 15) << 4) | (y & 15));
}
const void* readBlockOf(void* subChunk, unsigned layer, unsigned beIndex);
bool readExtraOf(void* subChunk, unsigned beIndex, const void*& out);
bool readWorldExtraAt(int x, int y, int z, const void*& out);

void noteSubChunkAt(int baseX, int baseY, int baseZ, void* subChunk);
void forgetSubChunkAt(int baseX, int baseY, int baseZ);
bool worldPosOfSubChunkWrite(void* subChunk, unsigned int index, int out[3]);
void forgetSubChunks();
void noteLiveSubChunk(void* subChunk);

void noteWorldChanged(bool leftAWorld);

}
