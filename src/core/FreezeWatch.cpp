#include "core/FreezeWatch.h"

#include "core/Logger.h"
#include "core/Paths.h"

#include <Windows.h>
#include <TlHelp32.h>
#include <psapi.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

namespace tsukuyomi::freezewatch {

namespace {

constexpr int kScopeSlots = 16;
struct ScopeSlot {
    std::atomic<DWORD> tid{0};
    std::atomic<unsigned long long> since{0};
    std::atomic<const char*> name{nullptr};
    std::atomic<bool> reported{false};
};
ScopeSlot g_scopes[kScopeSlots];
thread_local int t_slot = -1;
thread_local int t_depth = 0;

std::atomic<unsigned long long> g_lastPresent{0};
std::atomic<DWORD> g_presentTid{0};
std::atomic<bool> g_presentReported{false};
std::atomic<HWND> g_gameWindow{nullptr};

constexpr unsigned long long kPresentStallMs = 6000;
constexpr unsigned long long kScopeStallMs = 2000;
constexpr int kMaxDumps = 6;
std::atomic<int> g_dumps{0};

HANDLE g_thread = nullptr;
std::atomic<bool> g_stop{false};
DWORD g_selfTid = 0;

struct Module {
    std::uintptr_t base = 0;
    std::uintptr_t size = 0;
    char name[48] = {};
};
constexpr int kMaxModules = 320;
Module g_modules[kMaxModules];
int g_moduleCount = 0;
unsigned long long g_modulesAt = 0;

void refreshModules()
{
    HMODULE handles[kMaxModules];
    DWORD needed = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), handles, sizeof(handles), &needed)) {
        return;
    }
    const int count = static_cast<int>(needed / sizeof(HMODULE)) < kMaxModules
                          ? static_cast<int>(needed / sizeof(HMODULE))
                          : kMaxModules;
    int n = 0;
    for (int i = 0; i < count; ++i) {
        MODULEINFO info{};
        if (!K32GetModuleInformation(GetCurrentProcess(), handles[i], &info, sizeof(info))) {
            continue;
        }
        Module& m = g_modules[n];
        m.base = reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll);
        m.size = info.SizeOfImage;
        char path[MAX_PATH] = {};
        if (K32GetModuleBaseNameA(GetCurrentProcess(), handles[i], path, MAX_PATH) == 0) {
            std::snprintf(m.name, sizeof(m.name), "mod%p", info.lpBaseOfDll);
        } else {
            std::snprintf(m.name, sizeof(m.name), "%s", path);
        }
        ++n;
    }
    g_moduleCount = n;
    g_modulesAt = GetTickCount64();
}

const Module* moduleOf(std::uintptr_t at)
{
    for (int i = 0; i < g_moduleCount; ++i) {
        const Module& m = g_modules[i];
        if (at >= m.base && at < m.base + m.size) {
            return &m;
        }
    }
    return nullptr;
}

bool copyGuarded(void* dst, const void* src, std::size_t bytes)
{
    __try {
        std::memcpy(dst, src, bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool precededByCall(std::uintptr_t at)
{
    unsigned char b[8] = {};
    if (at < 8 || !copyGuarded(b, reinterpret_cast<const void*>(at - 8), sizeof(b))) {
        return false;
    }
    if (b[3] == 0xE8) {
        return true;
    }
    if (b[2] == 0xFF && b[3] == 0x15) {
        return true;
    }
    if (b[6] == 0xFF && (b[7] & 0xF8) == 0xD0) {
        return true;
    }
    if (b[5] == 0x41 && b[6] == 0xFF && (b[7] & 0xF8) == 0xD0) {
        return true;
    }
    if (b[5] == 0xFF && (b[6] & 0xF8) == 0x50) {
        return true;
    }
    if (b[4] == 0x41 && b[5] == 0xFF && (b[6] & 0xF8) == 0x50) {
        return true;
    }
    if (b[2] == 0xFF && (b[3] & 0xF8) == 0x90) {
        return true;
    }
    if (b[1] == 0x41 && b[2] == 0xFF && (b[3] & 0xF8) == 0x90) {
        return true;
    }
    if (b[6] == 0xFF && (b[7] & 0xF8) == 0x10) {
        return true;
    }
    return false;
}

constexpr int kMaxThreads = 384;
constexpr int kMaxReturns = 28;
constexpr std::size_t kStackBytes = 48 * 1024;
struct Shot {
    DWORD tid = 0;
    std::uintptr_t rip = 0;
    std::uintptr_t rsp = 0;
    int count = 0;
    std::uintptr_t ret[kMaxReturns] = {};
    bool ok = false;
};
Shot g_shots[kMaxThreads];
int g_shotCount = 0;
alignas(16) unsigned char g_stack[kStackBytes];
alignas(16) CONTEXT g_ctx;

void shootThread(DWORD tid, Shot& out)
{
    out = Shot{};
    out.tid = tid;
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                          FALSE, tid);
    if (h == nullptr) {
        return;
    }
    std::size_t copied = 0;
    if (SuspendThread(h) != static_cast<DWORD>(-1)) {
        std::memset(&g_ctx, 0, sizeof(g_ctx));
        g_ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (GetThreadContext(h, &g_ctx)) {
            out.rip = static_cast<std::uintptr_t>(g_ctx.Rip);
            out.rsp = static_cast<std::uintptr_t>(g_ctx.Rsp);
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<const void*>(out.rsp), &mbi, sizeof(mbi)) != 0) {
                const auto end = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
                std::size_t bytes = end > out.rsp ? end - out.rsp : 0;
                if (bytes > kStackBytes) {
                    bytes = kStackBytes;
                }
                if (bytes >= 8 && copyGuarded(g_stack, reinterpret_cast<const void*>(out.rsp), bytes)) {
                    copied = bytes;
                }
            }
            out.ok = true;
        }
        ResumeThread(h);
    }
    CloseHandle(h);
    for (std::size_t off = 0; off + 8 <= copied && out.count < kMaxReturns; off += 8) {
        std::uintptr_t value = 0;
        std::memcpy(&value, g_stack + off, sizeof(value));
        if (moduleOf(value) != nullptr && precededByCall(value)) {
            out.ret[out.count++] = value;
        }
    }
}

void shootAll()
{
    g_shotCount = 0;
    const DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return;
    }
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    for (BOOL ok = Thread32First(snap, &te); ok && g_shotCount < kMaxThreads;
         ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == g_selfTid) {
            continue;
        }
        shootThread(te.th32ThreadID, g_shots[g_shotCount++]);
    }
    CloseHandle(snap);
}

char g_line[1024];
HANDLE g_file = INVALID_HANDLE_VALUE;

void put(const char* text)
{
    if (g_file == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD wrote = 0;
    WriteFile(g_file, text, static_cast<DWORD>(std::strlen(text)), &wrote, nullptr);
}

int describe(std::uintptr_t at, char* out, std::size_t cap)
{
    if (const Module* m = moduleOf(at)) {
        return std::snprintf(out, cap, "%s+%#llx", m->name,
                             static_cast<unsigned long long>(at - m->base));
    }
    return std::snprintf(out, cap, "%#llx", static_cast<unsigned long long>(at));
}

bool isQuiet(const Shot& s)
{
    auto systemOnly = [](std::uintptr_t at) {
        const Module* m = moduleOf(at);
        if (m == nullptr) {
            return false;
        }
        return _stricmp(m->name, "ntdll.dll") == 0 || _stricmp(m->name, "KERNELBASE.dll") == 0
               || _stricmp(m->name, "KERNEL32.DLL") == 0 || _stricmp(m->name, "win32u.dll") == 0
               || _stricmp(m->name, "combase.dll") == 0 || _stricmp(m->name, "RPCRT4.dll") == 0
               || _stricmp(m->name, "ucrtbase.dll") == 0 || _stricmp(m->name, "msvcp140.dll") == 0;
    };
    if (!systemOnly(s.rip)) {
        return false;
    }
    for (int i = 0; i < s.count; ++i) {
        if (!systemOnly(s.ret[i])) {
            return false;
        }
    }
    return true;
}

void writeShot(const Shot& s, const char* tag)
{
    char where[128];
    describe(s.rip, where, sizeof(where));
    char name[96] = "";
    if (HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, s.tid); h != nullptr) {
        PWSTR desc = nullptr;
        if (SUCCEEDED(GetThreadDescription(h, &desc)) && desc != nullptr) {
            WideCharToMultiByte(CP_UTF8, 0, desc, -1, name, sizeof(name) - 1, nullptr, nullptr);
            LocalFree(desc);
        }
        CloseHandle(h);
    }
    std::snprintf(g_line, sizeof(g_line), "\r\n== thread %lu%s%s%s%s  rip %s\r\n",
                  static_cast<unsigned long>(s.tid), name[0] ? " \"" : "", name,
                  name[0] ? "\"" : "", tag, where);
    put(g_line);
    for (int i = 0; i < s.count; ++i) {
        char one[128];
        describe(s.ret[i], one, sizeof(one));
        std::snprintf(g_line, sizeof(g_line), "   [%2d] %s\r\n", i, one);
        put(g_line);
    }
}

void dump(const char* reason, DWORD focusTid)
{
    if (g_dumps.fetch_add(1, std::memory_order_relaxed) >= kMaxDumps) {
        return;
    }
    shootAll();

    SYSTEMTIME t{};
    GetLocalTime(&t);
    wchar_t leaf[64];
    swprintf_s(leaf, L"freeze-%04u%02u%02u-%02u%02u%02u.txt", t.wYear, t.wMonth, t.wDay,
               t.wHour, t.wMinute, t.wSecond);
    const std::wstring path = (paths::dataDir() / leaf).wstring();
    g_file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
    std::snprintf(g_line, sizeof(g_line),
                  "Tsukuyomi freeze report %04u-%02u-%02u %02u:%02u:%02u\r\nreason: %s\r\n"
                  "present thread %lu / last present %llu ms ago / threads %d / modules %d\r\n",
                  t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, reason,
                  static_cast<unsigned long>(g_presentTid.load()),
                  static_cast<unsigned long long>(GetTickCount64() - g_lastPresent.load()),
                  g_shotCount, g_moduleCount);
    put(g_line);
    const unsigned long long now = GetTickCount64();
    for (const ScopeSlot& slot : g_scopes) {
        const DWORD tid = slot.tid.load(std::memory_order_acquire);
        if (tid == 0) {
            continue;
        }
        const char* n = slot.name.load(std::memory_order_acquire);
        std::snprintf(g_line, sizeof(g_line), "scope \"%s\" on thread %lu for %llu ms\r\n",
                      n != nullptr ? n : "?", static_cast<unsigned long>(tid),
                      static_cast<unsigned long long>(now - slot.since.load()));
        put(g_line);
    }
    int quiet = 0;
    for (int pass = 0; pass < 3; ++pass) {
        for (int i = 0; i < g_shotCount; ++i) {
            const Shot& s = g_shots[i];
            const bool focus = (s.tid == focusTid && focusTid != 0);
            const bool present = (s.tid == g_presentTid.load());
            if (pass == 0 && focus) {
                writeShot(s, " [stalled scope]");
            } else if (pass == 1 && present && !focus) {
                writeShot(s, " [present]");
            } else if (pass == 2 && !focus && !present) {
                if (isQuiet(s)) {
                    ++quiet;
                } else {
                    writeShot(s, "");
                }
            }
        }
    }
    std::snprintf(g_line, sizeof(g_line), "\r\n(%d thread(s) only waiting in the system were folded)\r\n",
                  quiet);
    put(g_line);
    if (g_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
    log().warn(L"FreezeWatch: the game looked stuck ({}); the thread stacks were written to {}",
               std::wstring(reason, reason + std::strlen(reason)), path);
}

bool ourWindowInFront(HWND fg)
{
    DWORD fgPid = 0;
    if (fg != nullptr) {
        GetWindowThreadProcessId(fg, &fgPid);
    }
    return fgPid == GetCurrentProcessId() && !IsIconic(fg);
}

bool isOurGhost(HWND fg)
{
    HWND const game = g_gameWindow.load(std::memory_order_acquire);
    if (fg == nullptr || game == nullptr || !IsWindow(game)) {
        return false;
    }
    wchar_t cls[16] = {};
    if (GetClassNameW(fg, cls, 16) == 0 || std::wcscmp(cls, L"Ghost") != 0) {
        return false;
    }
    RECT ghost{};
    RECT ours{};
    if (!GetWindowRect(fg, &ghost) || !GetWindowRect(game, &ours)) {
        return false;
    }
    const POINT center{(ghost.left + ghost.right) / 2, (ghost.top + ghost.bottom) / 2};
    return PtInRect(&ours, center) != FALSE;
}

DWORD WINAPI watchLoop(void*)
{
    g_selfTid = GetCurrentThreadId();
    SetThreadDescription(GetCurrentThread(), L"Tsukuyomi FreezeWatch");
    refreshModules();
    char reason[256];
    while (!g_stop.load(std::memory_order_acquire)) {
        Sleep(250);
        const unsigned long long now = GetTickCount64();

        for (ScopeSlot& slot : g_scopes) {
            const DWORD tid = slot.tid.load(std::memory_order_acquire);
            if (tid == 0) {
                continue;
            }
            const unsigned long long since = slot.since.load(std::memory_order_acquire);
            if (since == 0 || since > now || now - since < kScopeStallMs
                || slot.reported.exchange(true, std::memory_order_acq_rel)) {
                continue;
            }
            const char* n = slot.name.load(std::memory_order_acquire);
            std::snprintf(reason, sizeof(reason), "thread %lu has been inside \"%s\" for %llu ms",
                          static_cast<unsigned long>(tid), n != nullptr ? n : "?",
                          static_cast<unsigned long long>(now - since));
            dump(reason, tid);
        }

        const unsigned long long last = g_lastPresent.load(std::memory_order_acquire);
        if (last == 0) {
            continue;
        }
        if (last > now || now - last < kPresentStallMs) {
            g_presentReported.store(false, std::memory_order_relaxed);
            if (HWND const fg = GetForegroundWindow(); ourWindowInFront(fg)) {
                g_gameWindow.store(fg, std::memory_order_release);
            }
            if (now - g_modulesAt > 30000) {
                refreshModules();
            }
            continue;
        }
        if (g_presentReported.load(std::memory_order_relaxed)) {
            continue;
        }
        HWND const fg = GetForegroundWindow();
        const bool inFront = ourWindowInFront(fg);
        if (!inFront && !isOurGhost(fg)) {
            continue;
        }
        g_presentReported.store(true, std::memory_order_relaxed);
        std::snprintf(reason, sizeof(reason), "no frame was presented for %llu ms (in front: %s)",
                      static_cast<unsigned long long>(now - last),
                      inFront ? "the game window" : "the \"not responding\" ghost of the game window");
        dump(reason, 0);
    }
    return 0;
}

}

void start()
{
    if (g_thread != nullptr) {
        return;
    }
    g_stop.store(false, std::memory_order_release);
    g_thread = CreateThread(nullptr, 0, &watchLoop, nullptr, 0, nullptr);
    if (g_thread == nullptr) {
        log().warn(L"FreezeWatch: could not start the watcher thread ({})", GetLastError());
    }
}

void stop()
{
    if (g_thread == nullptr) {
        return;
    }
    g_stop.store(true, std::memory_order_release);
    if (WaitForSingleObject(g_thread, 5000) != WAIT_OBJECT_0) {
        log().warn(L"FreezeWatch: still waiting for the watcher thread to stop");
        WaitForSingleObject(g_thread, INFINITE);
    }
    CloseHandle(g_thread);
    g_thread = nullptr;
}

void notePresent()
{
    g_lastPresent.store(GetTickCount64(), std::memory_order_release);
    g_presentTid.store(GetCurrentThreadId(), std::memory_order_relaxed);
}

Scope::Scope(const char* name)
{
    if (t_slot >= 0) {
        ++t_depth;
        return;
    }
    const DWORD self = GetCurrentThreadId();
    for (int i = 0; i < kScopeSlots; ++i) {
        DWORD expected = 0;
        if (g_scopes[i].tid.compare_exchange_strong(expected, self, std::memory_order_acq_rel)) {
            g_scopes[i].name.store(name, std::memory_order_relaxed);
            g_scopes[i].reported.store(false, std::memory_order_relaxed);
            g_scopes[i].since.store(GetTickCount64(), std::memory_order_release);
            m_slot = i;
            t_slot = i;
            t_depth = 0;
            return;
        }
    }
}

Scope::~Scope()
{
    if (m_slot < 0) {
        if (t_slot >= 0 && t_depth > 0) {
            --t_depth;
        }
        return;
    }
    g_scopes[m_slot].since.store(0, std::memory_order_release);
    g_scopes[m_slot].tid.store(0, std::memory_order_release);
    t_slot = -1;
    t_depth = 0;
}

}
