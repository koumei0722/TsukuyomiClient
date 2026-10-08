#pragma once

namespace tsukuyomi::oreui {

bool installEarlyFileHook();

bool buildPatchedBundle();

bool buildPatchedStartScreen();

void removeEarlyFileHook();

bool patchReady();

const wchar_t* patchFailure();

const char* ownGroupId(int index);
int ownGroupIdCount();

}
