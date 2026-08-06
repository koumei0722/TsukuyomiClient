#pragma once

namespace tsukuyomi::hooks {

void installAll();

float callGetDestroySpeed(void* rcx, void* rdx, void* r8, void* r9);
void callSetSelectedSlot(void* rcx, void* rdx, void* r8, void* r9);
bool callBuildBlock(void* gameMode, void* blockPos, unsigned char face, unsigned char extra);

int callUseItem(void* gameMode, void* itemStack);

bool callSetGameMode(void* self, int mode, int extra);

void callNotifyInventoryOpen(void* client);

bool hasGetDestroySpeed();
bool hasSetSelectedSlot();
bool hasBuildBlock();
bool hasUseItem();
bool hasSetGameMode();
bool hasNotifyInventoryOpen();

}
