#pragma once

#include <cstddef>
#include <cstdint>

namespace tsukuyomi::blockwrite {

struct BlockPos {
    int x = 0;
    int y = 0;
    int z = 0;
};

void noteRegion(void* region, unsigned int mode, unsigned int updateFlags, void* actor);

void noteRenderRegion(void* region);

void* renderRegion();

std::uint64_t regionGeneration();

bool regionIsAlive(void* region);

bool lastFindWasMissingChunk();
bool lastFindWasOutsideWorld();

void* findSubChunk(void* region, int x, int y, int z);

void* region();

bool placeAt(const BlockPos& at, const void* block);

void setKnownBlocks(const void* const* blocks, std::size_t count);

void noteWritePos(const int pos[3]);

const void* readWorldAt(int x, int y, int z);

bool readWorldExtraAt(int x, int y, int z, const void*& out);

std::size_t knownSubChunkCount();

void noteSubChunkAt(int baseX, int baseY, int baseZ, void* subChunk);

void forgetSubChunkAt(int baseX, int baseY, int baseZ);

bool worldPosOfSubChunkWrite(void* subChunk, unsigned int index, int out[3]);

void forgetSubChunks();

void noteWorldChanged(bool leftAWorld);

void beginSelfWrite();
void endSelfWrite();

bool selfWriting();

const void* onSubChunkWrite(void* subChunk, unsigned int layer, unsigned int index,
                            const void* block);

}
