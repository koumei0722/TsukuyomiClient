#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tsukuyomi::hooks {

void installAll();

void freezeHookGroups();

bool installLate();

float callGetDestroySpeed(void* rcx, void* rdx, void* r8, void* r9);
void callSetSelectedSlot(void* rcx, void* rdx, void* r8, void* r9);
bool callAttackCore(void* gameMode, void* target, bool direct, const void* hitPos);
bool callSendComplexTx(void* player, void** transaction);
bool attackHooksInstalled();
bool callBuildBlock(void* gameMode, void* blockPos, unsigned char face, unsigned char extra,
                    bool simTick);
bool callOriginalBuildBlock(void* gameMode, std::int32_t blockPos[3], unsigned char face,
                            const float click[3], unsigned char extra, bool simTick);
bool callBuildBlockAimedWithoutSwing(void* gameMode, std::int32_t blockPos[3], unsigned char face,
                                     const float click[3], unsigned char extra, bool simTick);

struct DryWrite {
    void* subChunk = nullptr;
    unsigned layer = 0, index = 0;
    const void* before = nullptr;
    const void* block = nullptr;
};
class DryRun {
public:
    explicit DryRun(bool apply);
    ~DryRun();
    DryRun(const DryRun&) = delete;
    DryRun& operator=(const DryRun&) = delete;
    bool ok() const { return m_ok; }
    bool click(void* gameMode, std::int32_t blockPos[3], unsigned char face, const float click[3], bool& used);
    const std::vector<DryWrite>& writes() const { return m_writes; }
    unsigned dropped() const { return m_dropped; }
private:
    friend void noteDryWrite(void*, unsigned, unsigned, const void*);
    friend void noteDryPacket();
    std::vector<DryWrite> m_writes;
    unsigned m_dropped = 0;
    bool m_apply = false, m_ok = false;
};
bool dryRunActive();
void noteDryWrite(void* subChunk, unsigned layer, unsigned index, const void* block);
void noteDryPacket();

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
bool callClientRefreshScreenSize(void* client);

bool callOpenHowToPlayScreen();

bool popTopScreen();

bool offstackScreenHeld();
bool tickOffstackScreen();
void releaseOffstackScreen(const wchar_t* why);

void* callGameAllocate(size_t size);
bool callGameFree(void* data);

bool translate(void* out, const char* key);
bool translateWith(void* out, const void* key, const void* params);

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
void* hitResultAddress();
bool readLiquidHitResult(unsigned char out[kHitResultBytes]);
bool repickHitResult();

}
