#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace tsukuyomi::hooks {

void installAll();

void restoreMaterialBlend();

bool installLate();

bool inGhostBeDispatch();

float callGetDestroySpeed(void* rcx, void* rdx, void* r8, void* r9);
void callSetSelectedSlot(void* rcx, void* rdx, void* r8, void* r9);
bool callBuildBlock(void* gameMode, void* blockPos, unsigned char face, unsigned char extra,
                    bool simTick);

int callUseItem(void* gameMode, void* itemStack, int extra);

int callUseItemTransaction(void* gameMode, void* itemStack, int extra);

bool callSetGameMode(void* self, int mode, int extra);

void callNotifyInventoryOpen(void* client);

void* callUiDefLookup(void* self, const void* space, const void* name);

void* callUiBagFind(void* bag, const char* key);

bool setUiBagNumber(void* holder, const char* name, unsigned long long size, float value);

bool callKeyDisplayName(void* outString, int keyCode);

bool callSettingsGroupRegister(void* registry, const void* idView, void* provider);
bool callSettingsAddTab(void* list, unsigned tab, const void* screenView, const void* descView);
bool callSettingsInvokeAction(void* component, void* done);

void* gameClientInstance();

bool callOpenHowToPlayScreen();

void* callGameAllocate(size_t size);

void* callSettingsFindComponent(void* registry, const void* idView);

enum class InventoryOpenResult {
    Opened,
    NotReady,
    Faulted,
};

InventoryOpenResult callOpenInventoryScreen(void* client);

bool hasGetDestroySpeed();
bool hasSetSelectedSlot();
bool hasBuildBlock();
bool hasUseItem();
bool hasUseItemTransaction();
bool hasSetGameMode();
bool hasNotifyInventoryOpen();

const void* readWorldBlock(void* region, const int pos[3]);

void requestGhostChunkBuild();

void requestChunkRebuilds(const std::vector<std::array<std::int32_t, 3>>& chunks);
void clearChunkRebuilds();
bool chunkHasGeometryNow(std::int32_t cx, std::int32_t cy, std::int32_t cz);
bool chunkLastBuildBoxTries(std::int32_t cx, std::int32_t cy, std::int32_t cz,
                            std::uint32_t& tries, std::uint64_t* startSeq = nullptr,
                            std::uint32_t* stacked = nullptr);
std::size_t buildsOutOfOrderCount();
std::uint64_t nextBuildSeq();
void clearChunkBoxTries();
void chunkRebuildStats(std::size_t (&out)[9]);

void boxBucketStats(std::size_t& pushDiff, std::size_t& ok, std::size_t& skipped,
                    std::uint32_t& lastLayer);

bool boxCoverageStats(std::int32_t& minX, std::int32_t& maxX, std::int32_t& minZ,
                      std::int32_t& maxZ);

void askBuildStats(std::size_t (&out)[9]);

void overlayStats(std::size_t& stacked, std::size_t& missed);

void boxCullStats(std::size_t& getBlock, std::size_t& getExtra, std::size_t& lookup);

void boxShapeStats(std::size_t& verts);

void beCubeStats(std::size_t& cells, std::size_t& verts, std::size_t& miss);

void uvPaintStats(std::size_t& painted, std::size_t& missed);

void beCubeGates(std::size_t& realDrew, std::size_t& noBox, std::size_t& bothDrew);

bool modelBoxedForBlock(const void* block);

bool beModelsOn();

unsigned long long lastModelLearnedAt();

std::size_t chunkBuildRefusedCount();

void hotPathStats(std::size_t& preds, std::size_t& scans, std::size_t& getBlockGhost,
                  std::size_t& getBlockCalls);

void armStorageHooks(void* subChunk, int baseX, int baseY, int baseZ);

void armStorageFromSubChunk(void* subChunk);

void armPendingStoragePreds();

void dropStorageMark(int baseX, int baseY, int baseZ);

void clearStorageMarks();

void* materialInfoByName(std::string_view name);

}
