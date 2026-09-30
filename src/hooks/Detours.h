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

bool callGameModeContinueDestroyBlock(void* gameMode, const void* pos, std::uint8_t face,
                                      const void* playerPos, bool* out);
bool callGameModeDestroyBlock(void* gameMode, const void* pos, std::uint8_t face);
bool hasGameModeContinueDestroyBlock();
bool hasGameModeDestroyBlock();

int callUseItem(void* gameMode, void* itemStack, int extra);

int callUseItemTransaction(void* gameMode, void* itemStack, int extra);

void callNotifyInventoryOpen(void* client);

void* callUiDefLookup(void* self, const void* space, const void* name);

void* uiBagSetFunction();

bool setUiBagNumber(void* holder, const char* name, unsigned long long size, float value);

bool callSettingsGroupRegister(void* registry, const void* idView, void* provider);

void* gameClientInstance();

bool callOpenHowToPlayScreen();

bool popTopScreen();

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

bool hasNotifyInventoryOpen();

inline constexpr std::size_t kHitResultBytes = 0x85;
bool readHitResult(unsigned char out[kHitResultBytes]);
bool readLiquidHitResult(unsigned char out[kHitResultBytes]);

void requestGhostChunkBuild();

std::uint64_t requestChunkRebuilds(const std::vector<std::array<std::int32_t, 3>>& chunks);
void clearChunkRebuilds();
bool chunkHasGeometryNow(std::int32_t cx, std::int32_t cy, std::int32_t cz);
bool chunkLastBuildBoxTries(std::int32_t cx, std::int32_t cy, std::int32_t cz,
                            std::uint32_t& tries, std::uint64_t* startSeq = nullptr,
                            std::uint32_t* stacked = nullptr);
std::uint64_t nextBuildSeq();
void clearChunkBoxTries();

void armStorageHooks(void* subChunk, int baseX, int baseY, int baseZ);

void armStorageFromSubChunk(void* subChunk);

void armPendingStoragePreds();

void beginStorageArmBatch();
void endStorageArmBatch();

void dropStorageMark(int baseX, int baseY, int baseZ);

void clearStorageMarks();

}
