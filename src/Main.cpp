#include "core/Client.h"
#include "game/OreUiPatch.h"

#include <Windows.h>

namespace {

#pragma warning(push)
#pragma warning(disable : 4702)
DWORD WINAPI bootstrap(LPVOID parameter)
{
    HMODULE self = static_cast<HMODULE>(parameter);
    tsukuyomi::Client::instance().run(self);

    tsukuyomi::oreui::removeEarlyFileHook();

    FreeLibraryAndExitThread(self, 0);
    return 0;
}
#pragma warning(pop)

}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);

        tsukuyomi::oreui::installEarlyFileHook();
        tsukuyomi::oreui::buildPatchedBundle();

        tsukuyomi::oreui::buildPatchedStartScreen();

        const HANDLE thread = CreateThread(nullptr, 0, &bootstrap, module, 0, nullptr);
        if (thread != nullptr) {
            CloseHandle(thread);
        }
    }
    return TRUE;
}
