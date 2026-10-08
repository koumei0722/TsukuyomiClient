#include "game/SystemInfo.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#include <intrin.h>
#include <Windows.h>
#include <dxgi.h>
#include <pdh.h>
#include <PdhMsg.h>
#include <psapi.h>

namespace tsukuyomi::sysinfo {
namespace {

bool copyText(const char* source, char* out, std::size_t cap)
{
    if (!source || !out || cap == 0) return false;
    const auto length = std::strlen(source);
    if (length >= cap) return false;
    std::memcpy(out, source, length + 1);
    return true;
}

bool brandText(char (&out)[49])
{
    int registers[4]{};
    __cpuid(registers, static_cast<int>(0x80000000u));
    if (static_cast<unsigned int>(registers[0]) < 0x80000004u) return false;
    for (unsigned int leaf = 0x80000002u; leaf <= 0x80000004u; ++leaf) {
        __cpuid(registers, static_cast<int>(leaf));
        std::memcpy(out + (leaf - 0x80000002u) * 16u, registers, 16);
    }
    out[48] = '\0';
    return true;
}

std::mutex g_pdhMutex;
bool g_pdhAttempted = false;
bool g_pdhReady = false;
PDH_HQUERY g_query = nullptr;
PDH_HCOUNTER g_counter = nullptr;
std::vector<std::byte> g_pdhBuffer(64 * 1024);

bool containsNoCase(const wchar_t* haystack, const wchar_t* needle)
{
    if (haystack == nullptr || needle == nullptr) return false;
    for (; *haystack != L'\0'; ++haystack) {
        const wchar_t* a = haystack;
        const wchar_t* b = needle;
        while (*b != L'\0' && *a != L'\0' && std::towlower(*a) == std::towlower(*b)) {
            ++a;
            ++b;
        }
        if (*b == L'\0') return true;
    }
    return *needle == L'\0';
}

struct AdapterData {
    char name[128]{};
    char vendor[32]{};
    char driver[32]{};
};
std::once_flag g_adapterOnce;
AdapterData g_adapterData{};
bool g_adapterAvailable = false;

bool readAdapter(AdapterData& out)
{
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory) return false;
    IDXGIAdapter1* adapter = nullptr;
    const HRESULT enumerate = factory->EnumAdapters1(0, &adapter);
    factory->Release();
    if (FAILED(enumerate) || !adapter) return false;
    DXGI_ADAPTER_DESC1 description{};
    const HRESULT describe = adapter->GetDesc1(&description);
    LARGE_INTEGER umd{};
    const bool haveDriver = SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd));
    adapter->Release();
    if (FAILED(describe)) return false;
    if (!WideCharToMultiByte(CP_UTF8, 0, description.Description, -1,
            out.name, static_cast<int>(sizeof(out.name)), nullptr, nullptr)) return false;
    const char* knownVendor = nullptr;
    switch (description.VendorId) {
    case 0x10DE: knownVendor = "NVIDIA Corporation"; break;
    case 0x1002: knownVendor = "Advanced Micro Devices, Inc."; break;
    case 0x8086: knownVendor = "Intel Corporation"; break;
    default: break;
    }
    if (knownVendor) {
        if (!copyText(knownVendor, out.vendor, sizeof(out.vendor))) return false;
    } else {
        const int written = std::snprintf(out.vendor, sizeof(out.vendor), "0x%04X", description.VendorId);
        if (written < 0 || static_cast<std::size_t>(written) >= sizeof(out.vendor)) return false;
    }
    out.driver[0] = '\0';
    if (haveDriver) {
        std::snprintf(out.driver, sizeof(out.driver), "%u.%u.%u.%u",
                      static_cast<unsigned>(HIWORD(umd.HighPart)), static_cast<unsigned>(LOWORD(umd.HighPart)),
                      static_cast<unsigned>(HIWORD(umd.LowPart)), static_cast<unsigned>(LOWORD(umd.LowPart)));
    }
    return true;
}

}

bool cpuName(char* out, std::size_t cap)
{
    char brand[49]{};
    if (!brandText(brand)) return false;
    std::string_view text(brand);
    const auto first = text.find_first_not_of(' ');
    if (first == std::string_view::npos) return false;
    const auto last = text.find_last_not_of(' ');
    text = text.substr(first, last - first + 1);
    if (!out || text.size() >= cap) return false;
    std::memcpy(out, text.data(), text.size());
    out[text.size()] = '\0';
    return true;
}

bool cpuThreads(int& out)
{
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    if (system.dwNumberOfProcessors == 0 ||
        system.dwNumberOfProcessors > static_cast<DWORD>(std::numeric_limits<int>::max())) return false;
    out = static_cast<int>(system.dwNumberOfProcessors);
    return true;
}

bool memoryUsage(Memory& out)
{
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            static_cast<DWORD>(sizeof(counters)))) return false;
    MEMORYSTATUSEX physical{};
    physical.dwLength = sizeof(physical);
    if (!GlobalMemoryStatusEx(&physical) || physical.ullTotalPhys == 0) return false;
    out = {static_cast<std::uint64_t>(counters.PrivateUsage),
        static_cast<std::uint64_t>(counters.WorkingSetSize),
        static_cast<std::uint64_t>(physical.ullTotalPhys)};
    return true;
}

namespace {
BOOL CALLBACK pickOwnWindow(HWND window, LPARAM param)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid != GetCurrentProcessId() || IsWindowVisible(window) == FALSE
        || GetWindow(window, GW_OWNER) != nullptr) {
        return TRUE;
    }
    *reinterpret_cast<HWND*>(param) = window;
    return FALSE;
}
}

void* mainWindow()
{
    static std::atomic<HWND> cached{nullptr};
    const HWND window = cached.load(std::memory_order_relaxed);
    if (window != nullptr && IsWindow(window) != FALSE) {
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (pid == GetCurrentProcessId() && IsWindowVisible(window) != FALSE
            && GetWindow(window, GW_OWNER) == nullptr) {
            return window;
        }
    }
    HWND found = nullptr;
    EnumWindows(&pickOwnWindow, reinterpret_cast<LPARAM>(&found));
    cached.store(found, std::memory_order_relaxed);
    return found;
}

namespace {

bool preciseRefreshRate(const wchar_t* gdiName, double& hz)
{
    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS
        || pathCount == 0 || pathCount > 64 || modeCount > 256) {
        return false;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr)
        != ERROR_SUCCESS) {
        return false;
    }
    for (UINT32 i = 0; i < pathCount; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = paths[i].sourceInfo.adapterId;
        source.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) {
            continue;
        }
        if (gdiName != nullptr && std::wcscmp(source.viewGdiDeviceName, gdiName) != 0) {
            continue;
        }
        const DISPLAYCONFIG_RATIONAL& rate = paths[i].targetInfo.refreshRate;
        if (rate.Denominator == 0 || rate.Numerator == 0) {
            continue;
        }
        hz = static_cast<double>(rate.Numerator) / static_cast<double>(rate.Denominator);
        return true;
    }
    return false;
}

std::mutex g_refreshMutex;
std::wstring g_refreshDevice;
double g_refreshRate = 0.0;
unsigned long long g_refreshTick = 0;

}

bool displayInfo(void* hwnd, Display& out)
{
    Display result{};
    if (hwnd) {
        RECT rect{};
        if (!GetClientRect(static_cast<HWND>(hwnd), &rect)) return false;
        result.width = rect.right - rect.left;
        result.height = rect.bottom - rect.top;
    } else {
        result.width = GetSystemMetrics(SM_CXSCREEN);
        result.height = GetSystemMetrics(SM_CYSCREEN);
    }
    if (result.width <= 0 || result.height <= 0) return false;
    MONITORINFOEXW monitor{};
    monitor.cbSize = sizeof(monitor);
    const wchar_t* displayName = nullptr;
    if (hwnd) {
        const HMONITOR handle = MonitorFromWindow(static_cast<HWND>(hwnd), MONITOR_DEFAULTTONEAREST);
        if (!handle || !GetMonitorInfoW(handle, &monitor)) return false;
        displayName = monitor.szDevice;
    }
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsW(displayName, ENUM_CURRENT_SETTINGS, &mode)) return false;
    result.refreshHz = static_cast<int>(mode.dmDisplayFrequency);
    {
        std::lock_guard lock(g_refreshMutex);
        const std::wstring device = displayName != nullptr ? displayName : L"";
        const unsigned long long now = GetTickCount64();
        if (device != g_refreshDevice || g_refreshTick == 0 || now - g_refreshTick >= 5000) {
            g_refreshDevice = device;
            g_refreshTick = now;
            double hz = 0.0;
            g_refreshRate = preciseRefreshRate(displayName, hz) ? hz : 0.0;
        }
        result.refreshRate = g_refreshRate;
    }

    std::call_once(g_adapterOnce, [] { g_adapterAvailable = readAdapter(g_adapterData); });
    if (!g_adapterAvailable) return false;
    std::memcpy(result.adapter, g_adapterData.name, sizeof(result.adapter));
    std::memcpy(result.vendor, g_adapterData.vendor, sizeof(result.vendor));
    std::memcpy(result.driver, g_adapterData.driver, sizeof(result.driver));
    out = result;
    return true;
}

void closeGpuUtilization()
{
    std::lock_guard lock(g_pdhMutex);
    if (g_query != nullptr) {
        PdhCloseQuery(g_query);
        g_query = nullptr;
    }
    g_counter = nullptr;
    g_pdhReady = false;
    g_pdhAttempted = false;
}

bool gpuUtilization(int& percent)
{
    std::lock_guard lock(g_pdhMutex);
    if (!g_pdhAttempted) {
        g_pdhAttempted = true;
        if (PdhOpenQueryW(nullptr, 0, &g_query) != ERROR_SUCCESS) return false;
        if (PdhAddEnglishCounterW(g_query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &g_counter) != ERROR_SUCCESS ||
            PdhCollectQueryData(g_query) != ERROR_SUCCESS) {
            PdhCloseQuery(g_query);
            g_query = nullptr;
            return false;
        }
        g_pdhReady = true;
        return false;
    }
    if (!g_pdhReady) return false;
    if (PdhCollectQueryData(g_query) != ERROR_SUCCESS) return false;
    DWORD bytes = static_cast<DWORD>(g_pdhBuffer.size());
    DWORD count = 0;
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(g_pdhBuffer.data());
    PDH_STATUS status = PdhGetFormattedCounterArrayW(g_counter, PDH_FMT_DOUBLE, &bytes, &count, items);
    if (status == PDH_MORE_DATA) {
        constexpr DWORD kMaxBytes = 16u * 1024u * 1024u;
        if (bytes == 0 || bytes > kMaxBytes) return false;
        g_pdhBuffer.resize(bytes);
        items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(g_pdhBuffer.data());
        count = 0;
        status = PdhGetFormattedCounterArrayW(g_counter, PDH_FMT_DOUBLE, &bytes, &count, items);
    }
    if (status != ERROR_SUCCESS) return false;
    wchar_t pidTag[32]{};
    if (swprintf_s(pidTag, L"pid_%lu_", GetCurrentProcessId()) < 0) return false;
    double total = 0.0;
    bool found = false;
    for (DWORD index = 0; index < count; ++index) {
        const auto* name = items[index].szName;
        if (!name || !containsNoCase(name, pidTag) || !containsNoCase(name, L"_engtype_3d")) continue;
        if (items[index].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA &&
            items[index].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
        const double value = items[index].FmtValue.doubleValue;
        if (!std::isfinite(value) || value < 0.0) continue;
        total += value;
        found = true;
    }
    if (!found) return false;
    percent = static_cast<int>(std::clamp(total, 0.0, 100.0) + 0.5);
    return true;
}

}
