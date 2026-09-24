#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace tsukuyomi::hooks {

void installAll();

void refreshHookGroups();

void freezeHookGroups();

bool installLate();

float callGetDestroySpeed(void* rcx, void* rdx, void* r8, void* r9);
void callSetSelectedSlot(void* rcx, void* rdx, void* r8, void* r9);
bool callAttackCore(void* gameMode, void* target, bool direct, const void* hitPos);
bool callSendComplexTx(void* player, void** transaction);
bool attackHooksInstalled();
bool callBuildBlock(void* gameMode, void* blockPos, unsigned char face, unsigned char extra,
                    bool simTick);

int callUseItem(void* gameMode, void* itemStack, int extra);

int callUseItemTransaction(void* gameMode, void* itemStack, int extra);

bool callSetGameMode(void* self, int mode, int extra);

void callNotifyInventoryOpen(void* client);

void* callUiDefLookup(void* self, const void* space, const void* name);

void* callUiBagFind(void* bag, const char* key);

void* uiBagSetFunction();

bool setUiBagNumber(void* holder, const char* name, unsigned long long size, float value);

bool callKeyDisplayName(void* outString, int keyCode);

bool callSettingsGroupRegister(void* registry, const void* idView, void* provider);
bool callSettingsInvokeAction(void* component, void* done);

void* gameClientInstance();

bool callOpenHowToPlayScreen();

bool popTopScreen();

bool offstackScreenAvailable();
bool offstackScreenHeld();
bool tickOffstackScreen();
void releaseOffstackScreen(const wchar_t* why);

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

std::uint64_t requestChunkRebuilds(const std::vector<std::array<std::int32_t, 3>>& chunks);
void clearChunkRebuilds();
bool chunkHasGeometryNow(std::int32_t cx, std::int32_t cy, std::int32_t cz);
bool chunkLastBuildBoxTries(std::int32_t cx, std::int32_t cy, std::int32_t cz,
                            std::uint32_t& tries, std::uint64_t* startSeq = nullptr,
                            std::uint32_t* stacked = nullptr);
std::uint64_t nextBuildSeq();
void clearChunkBoxTries();
void chunkRebuildStats(std::size_t (&out)[9]);

void askBuildStats(std::size_t (&out)[9]);

void overlayStats(std::size_t& stacked, std::size_t& missed);

void hotPathStats(std::size_t& preds, std::size_t& scans, std::size_t& getBlockGhost,
                  std::size_t& getBlockCalls);

void armStorageHooks(void* subChunk, int baseX, int baseY, int baseZ);

void armStorageFromSubChunk(void* subChunk);

void armPendingStoragePreds();

void beginStorageArmBatch();
void endStorageArmBatch();

void dropStorageMark(int baseX, int baseY, int baseZ);

void clearStorageMarks();

}
