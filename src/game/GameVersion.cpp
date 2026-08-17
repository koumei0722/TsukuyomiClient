#include "game/GameVersion.h"

#include <Windows.h>

#include <string>
#include <vector>

#include "core/Logger.h"

#pragma comment(lib, "version.lib")

namespace tsukuyomi::gameVersion {

namespace {

std::wstring readRunningVersion()
{
    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) {
        return {};
    }
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path, &ignored);
    if (size == 0) {
        return {};
    }
    std::vector<unsigned char> block(size);
    if (GetFileVersionInfoW(path, 0, size, block.data()) == 0) {
        return {};
    }
    VS_FIXEDFILEINFO* info = nullptr;
    UINT length = 0;
    if (VerQueryValueW(block.data(), L"\\", reinterpret_cast<void**>(&info), &length) == 0
        || info == nullptr) {
        return {};
    }
    return std::to_wstring(HIWORD(info->dwFileVersionMS)) + L"."
           + std::to_wstring(LOWORD(info->dwFileVersionMS)) + L"."
           + std::to_wstring(HIWORD(info->dwFileVersionLS)) + L"."
           + std::to_wstring(LOWORD(info->dwFileVersionLS));
}

const std::wstring& cached()
{
    static const std::wstring value = readRunningVersion();
    return value;
}

}

const wchar_t* running()
{
    return cached().c_str();
}

void logOnce()
{
    static const bool once = [] {
        const std::wstring& version = cached();
        if (version.empty()) {
            log().warn(L"Could not read the game version");
            return true;
        }
        if (version == kSignatureSource) {
            log().info(L"Game version {} (same as the version the signatures were taken from)",
                       version);
        } else {

            log().info(L"Game version {} (signatures were taken from {})",
                       version, kSignatureSource);
        }
        return true;
    }();
    (void)once;
}

}
