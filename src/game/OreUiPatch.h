#pragma once

namespace tsukuyomi::oreui {

bool installEarlyFileHook();

bool buildPatchedBundle();

bool buildPatchedStartScreen();

void removeEarlyFileHook();

bool patchReady();

const char* ownGroupId(int index);
int ownGroupIdCount();

}
