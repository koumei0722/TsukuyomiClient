#pragma once

namespace tsukuyomi::hooks {

void installAll();

float callGetDestroySpeed(void* rcx, void* rdx, void* r8, void* r9);
void callSetSelectedSlot(void* rcx, void* rdx, void* r8, void* r9);
bool callBuildBlock(void* gameMode, void* blockPos, unsigned char face, unsigned char extra);

int callUseItem(void* gameMode, void* itemStack);

int callUseItemTransaction(void* gameMode, void* itemStack);

bool callSetGameMode(void* self, int mode, int extra);

void callNotifyInventoryOpen(void* client);

void* callUiDefLookup(void* self, const void* space, const void* name);

void* callUiBagFind(void* bag, const char* key);

bool callKeyDisplayName(void* outString, int keyCode);

bool callSettingsGroupRegister(void* registry, const void* idView, void* provider);

void* callGameAllocate(size_t size);

void* callSettingsFindComponent(void* registry, const void* idView);

bool hasGetDestroySpeed();
bool hasSetSelectedSlot();
bool hasBuildBlock();
bool hasUseItem();
bool hasUseItemTransaction();
bool hasSetGameMode();
bool hasNotifyInventoryOpen();

}
