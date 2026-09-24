#include "input/LowLevelHook.h"

namespace tsukuyomi::input {

LowLevelHook installLowLevelHook(int idHook, HOOKPROC proc)
{
    LowLevelHook result;

    result.hook = SetWindowsHookExW(idHook, proc, nullptr, 0);
    if (result.hook != nullptr) {
        return result;
    }
    result.errorWithoutModule = GetLastError();

    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(proc), &module)) {
        result.errorWithModule = GetLastError();
        return result;
    }
    result.hook = SetWindowsHookExW(idHook, proc, module, 0);
    if (result.hook != nullptr) {
        result.withModule = true;
    } else {
        result.errorWithModule = GetLastError();
    }
    return result;
}

}
