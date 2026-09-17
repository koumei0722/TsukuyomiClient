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

bool placeAtRegion(void* region, const BlockPos& at, const void* block);

struct StorageSpot {
    void* subChunk = nullptr;
    unsigned int index = 0;
};

bool placeGhostActor(void* region, const BlockPos& at, const void* block, const void* air,
                     StorageSpot* spot);

bool removeGhostActor(void* region, const BlockPos& at, const void* block, const void* air);

bool nudgeRegion(void* region, const BlockPos& at, const void* block, const void* air,
                 void** outSubChunk, StorageSpot* hold);

void clearSpot(const StorageSpot& spot, const void* air);

inline constexpr std::size_t kFindWhyCount = 12;
void findSubChunkStats(std::size_t out[kFindWhyCount]);

bool lastFindWasMissingChunk();
bool lastFindWasOutsideWorld();

void* findSubChunk(void* region, int x, int y, int z);

void* region();

bool placeAt(const BlockPos& at, const void* block, bool* wroteGhost = nullptr);

void setKnownBlocks(const void* const* blocks, std::size_t count);

void noteWritePos(const int pos[3]);

const void* readWorldAt(int x, int y, int z);

bool readWorldExtraAt(int x, int y, int z, const void*& out);

std::size_t knownSubChunkCount();

void* subChunkAt(int baseX, int baseY, int baseZ);

void noteSubChunkAt(int baseX, int baseY, int baseZ, void* subChunk);

void forgetSubChunkAt(int baseX, int baseY, int baseZ);

void storageVtableStats(std::size_t& known, std::size_t& rejected);

bool worldPosOfSubChunkWrite(void* subChunk, unsigned int index, int out[3]);

void noteSubChunkAlt(int baseX, int baseY, int baseZ, void* subChunk);

void forgetSubChunks();

void noteWorldChanged();

void beginSelfWrite();
void endSelfWrite();

void beginRenderWrite();
void endRenderWrite();
bool renderWriting();
bool selfWriting();

void beginPlacement(const void* air);

void endPlacement();

std::size_t restoreAll();

std::size_t placedCount();

const void* onSubChunkWrite(void* subChunk, unsigned int layer, unsigned int index,
                            const void* block);

}
