#pragma once

#include <Windows.h>

namespace tsukuyomi::input {

struct LowLevelHook {
    HHOOK hook = nullptr;

    bool withModule = false;

    DWORD errorWithoutModule = 0;
    DWORD errorWithModule = 0;
};

LowLevelHook installLowLevelHook(int idHook, HOOKPROC proc);

}
