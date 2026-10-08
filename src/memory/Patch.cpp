#include "memory/Patch.h"

#include "config/WriteSwitches.h"
#include "core/Logger.h"
#include "memory/Memory.h"
#include "core/Strings.h"

#include <Windows.h>
#include <TlHelp32.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

namespace tsukuyomi {

namespace {

constexpr std::size_t kMaxFrozen = 2048;
struct Frozen {
    HANDLE thread = nullptr;
    int priority = THREAD_PRIORITY_NORMAL;
    bool boosted = false;
    DWORD64 rip = 0;
};
Frozen g_frozen[kMaxFrozen]{};
CRITICAL_SECTION* frozenLock()
{
    static CRITICAL_SECTION lock;
    static const bool ready = [] {
        InitializeCriticalSection(&lock);
        return true;
    }();
    (void)ready;
    return &lock;
}

bool suspendOne(DWORD tid, std::size_t& frozen)
{
    const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION
                                         | THREAD_SET_LIMITED_INFORMATION,
                                     FALSE, tid);
    if (thread == nullptr) {
        return false;
    }
    if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
        CloseHandle(thread);
        return false;
    }
    Frozen& item = g_frozen[frozen++];
    item = Frozen{thread};
    const int priority = GetThreadPriority(thread);
    if ((priority == THREAD_PRIORITY_IDLE || priority == THREAD_PRIORITY_LOWEST || priority == THREAD_PRIORITY_BELOW_NORMAL)
        && SetThreadPriority(thread, THREAD_PRIORITY_TIME_CRITICAL)) {
        item.boosted = true;
        item.priority = priority;
    }
    return true;
}

using NtGetNextThreadFn = LONG(NTAPI*)(HANDLE, HANDLE, ACCESS_MASK, ULONG, ULONG, PHANDLE);
constexpr LONG kStatusNoMoreEntries = static_cast<LONG>(0x8000001AL);

void thawOthers(std::size_t frozen);

std::size_t freezeOthers()
{
    const DWORD self = GetCurrentThreadId();
    std::size_t frozen = 0;
    static const auto next = reinterpret_cast<NtGetNextThreadFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtGetNextThread")));
    if (next != nullptr) {
        HANDLE cursor = nullptr;
        bool complete = false;
        while (frozen < kMaxFrozen) {
            HANDLE following = nullptr;
            const LONG status = next(GetCurrentProcess(), cursor, THREAD_QUERY_LIMITED_INFORMATION, 0, 0, &following);
            if (cursor != nullptr) {
                CloseHandle(cursor);
                cursor = nullptr;
            }
            if (status != 0) {
                complete = status == kStatusNoMoreEntries;
                break;
            }
            cursor = following;
            const DWORD tid = GetThreadId(following);
            if (tid != 0 && tid != self) {
                suspendOne(tid, frozen);
            }
        }
        if (cursor != nullptr) {
            CloseHandle(cursor);
        }
        if (complete) {
            return frozen;
        }
        thawOthers(frozen);
        frozen = 0;
    }

    const DWORD pid = GetCurrentProcessId();
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return 0;
    }
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    for (BOOL ok = Thread32First(snap, &te); ok && frozen < kMaxFrozen; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) {
            continue;
        }
        suspendOne(te.th32ThreadID, frozen);
    }
    CloseHandle(snap);
    return frozen;
}

void thawOthers(std::size_t frozen)
{
    for (std::size_t i = 0; i < frozen; ++i) {
        Frozen& item = g_frozen[i];
        if (item.boosted && !SetThreadPriority(item.thread, item.priority)) {
            SetThreadPriority(item.thread, THREAD_PRIORITY_NORMAL);
        }
        ResumeThread(item.thread);
        CloseHandle(item.thread);
        item = Frozen{};
    }
}

void captureIps(std::size_t frozen)
{
    for (std::size_t i = 0; i < frozen; ++i) {
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL;
        g_frozen[i].rip = GetThreadContext(g_frozen[i].thread, &ctx) != 0 ? ctx.Rip : 0;
    }
}

bool anyInside(std::size_t frozen, std::uintptr_t begin, std::size_t size)
{
    for (std::size_t i = 0; i < frozen; ++i) {
        const DWORD64 rip = g_frozen[i].rip;
        if (rip > begin && rip < begin + size) {
            return true;
        }
    }
    return false;
}

struct Write {
    std::byte* address = nullptr;
    const std::byte* source = nullptr;
    std::size_t size = 0;
};

std::size_t writeMany(const Write* writes, std::size_t count)
{
    if (count == 0) {
        return 0;
    }
    EnterCriticalSection(frozenLock());
    std::size_t frozen = 0;
    for (int attempt = 0;; ++attempt) {
        frozen = freezeOthers();
        captureIps(frozen);
        bool inside = false;
        for (std::size_t i = 0; i < count && !inside; ++i) {
            inside = anyInside(frozen, reinterpret_cast<std::uintptr_t>(writes[i].address), writes[i].size);
        }
        if (!inside) {
            break;
        }
        thawOthers(frozen);
        frozen = 0;
        if (attempt >= 100) {
            LeaveCriticalSection(frozenLock());
            static int warned = 0;
            if (warned++ < 3) {
                log().warn(L"Patch: a thread kept running inside the bytes to rewrite; the write was skipped");
            }
            return 0;
        }
        Sleep(1);
    }

    std::size_t written = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const Write& w = writes[i];
        DWORD oldProtect = 0;
        if (!VirtualProtect(w.address, w.size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            break;
        }
        std::memcpy(w.address, w.source, w.size);

        DWORD ignored = 0;
        VirtualProtect(w.address, w.size, oldProtect, &ignored);

        FlushInstructionCache(GetCurrentProcess(), w.address, w.size);
        ++written;
    }
    thawOthers(frozen);
    LeaveCriticalSection(frozenLock());
    return written;
}

bool writeAtomic(std::byte* address, const std::byte* source, std::size_t size)
{
    const auto at = reinterpret_cast<std::uintptr_t>(address);
    if (size == 0 || size > 8) {
        return false;
    }
    const std::uintptr_t line = at & ~static_cast<std::uintptr_t>(63);
    if (at + size > line + 64) {
        static int told = 0;
        if (told++ < 3) {
            log().warn(L"Patch: {} bytes at {:#x} cross a cache line; written with the other threads stopped", size, at);
        }
        return false;
    }
    std::uintptr_t begin = at & ~static_cast<std::uintptr_t>(7);
    if (at + size > begin + 8) {
        begin = std::max(line, at + size - 8);
    }
    const std::size_t offset = at - begin;

    EnterCriticalSection(frozenLock());
    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(begin), 8, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LeaveCriticalSection(frozenLock());
        return false;
    }
    auto* const word = reinterpret_cast<volatile LONG64*>(begin);
    LONG64 expected = *word;
    for (;;) {
        LONG64 desired = expected;
        std::memcpy(reinterpret_cast<std::byte*>(&desired) + offset, source, size);
        const LONG64 seen = InterlockedCompareExchange64(word, desired, expected);
        if (seen == expected) {
            break;
        }
        expected = seen;
    }
    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(begin), 8, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), address, size);
    LeaveCriticalSection(frozenLock());
    return true;
}

bool writeBytes(std::byte* address, const std::byte* source, size_t size)
{
    if (address == nullptr || size == 0) {
        return false;
    }
    const Write one{address, source, size};
    return writeMany(&one, 1) == 1;
}

}

Patch::Patch(void* address, std::vector<std::byte> patched, const char* name)
    : m_address(static_cast<std::byte*>(address))
    , m_patched(std::move(patched))
{
    if (name != nullptr) {
        writes::noteUnknown(name);
        if (!writes::allowed(std::string_view(name))) {
            m_address = nullptr;
            m_patched.clear();
            static std::vector<std::string> told;
            if (std::find(told.begin(), told.end(), name) == told.end()) {
                told.emplace_back(name);
                log().info(L"{} is turned off in hooks.json; not patched", toUtf16(name));
            }
        }
    }
}

Patch::~Patch()
{
    restore();
}

Patch::Patch(Patch&& other) noexcept
    : m_address(other.m_address)
    , m_original(std::move(other.m_original))
    , m_patched(std::move(other.m_patched))
    , m_applied(other.m_applied)
    , m_lockFree(other.m_lockFree)
{
    other.reset();
}

Patch& Patch::operator=(Patch&& other) noexcept
{
    if (this != &other) {
        restore();

        m_address = other.m_address;
        m_original = std::move(other.m_original);
        m_patched = std::move(other.m_patched);
        m_applied = other.m_applied;
        m_lockFree = other.m_lockFree;

        other.reset();
    }
    return *this;
}

void Patch::reset()
{
    m_address = nullptr;
    m_original.clear();
    m_patched.clear();
    m_applied = false;
    m_lockFree = false;
}

bool Patch::write(const std::byte* source)
{
    if (m_lockFree && writeAtomic(m_address, source, m_patched.size())) {
        return true;
    }
    return writeBytes(m_address, source, m_patched.size());
}

bool Patch::apply()
{
    if (!valid()) {
        return false;
    }
    if (m_applied) {
        return true;
    }

    if (m_original.empty()) {
        m_original.assign(m_address, m_address + m_patched.size());
    }

    if (!write(m_patched.data())) {
        return false;
    }

    m_applied = true;
    return true;
}

bool Patch::restore()
{
    if (!m_applied || m_original.empty()) {
        return false;
    }

    if (!write(m_original.data())) {
        return false;
    }

    m_applied = false;
    return true;
}

bool Patch::setAll(std::span<Patch* const> patches, bool enabled)
{
    std::vector<Patch*> todo;
    std::vector<Write> writes;
    todo.reserve(patches.size());
    writes.reserve(patches.size());
    for (Patch* const patch : patches) {
        if (patch == nullptr || !patch->valid() || patch->m_applied == enabled) {
            continue;
        }
        if (enabled) {
            if (patch->m_original.empty()) {
                patch->m_original.assign(patch->m_address, patch->m_address + patch->m_patched.size());
            }
            if (patch->m_lockFree && writeAtomic(patch->m_address, patch->m_patched.data(), patch->m_patched.size())) {
                patch->m_applied = true;
                continue;
            }
            writes.push_back({patch->m_address, patch->m_patched.data(), patch->m_patched.size()});
        } else {
            if (patch->m_original.empty()) {
                continue;
            }
            if (patch->m_lockFree && writeAtomic(patch->m_address, patch->m_original.data(), patch->m_original.size())) {
                patch->m_applied = false;
                continue;
            }
            writes.push_back({patch->m_address, patch->m_original.data(), patch->m_original.size()});
        }
        todo.push_back(patch);
    }
    const std::size_t written = writeMany(writes.data(), writes.size());
    for (std::size_t i = 0; i < written; ++i) {
        todo[i]->m_applied = enabled;
    }
    return written == writes.size();
}

std::size_t movssStoreLength(const void* address)
{
    if (address == nullptr || !memory::isReadable(address, 15)) {
        return 0;
    }
    const auto* const b = static_cast<const unsigned char*>(address);
    std::size_t i = 0;
    if (b[i++] != 0xF3) {
        return 0;
    }
    if (b[i] >= 0x40 && b[i] <= 0x4F) {
        ++i;
    }
    if (b[i] != 0x0F || b[i + 1] != 0x11) {
        return 0;
    }
    i += 2;
    const unsigned modrm = b[i++];
    const unsigned mod = modrm >> 6;
    const unsigned rm = modrm & 7;
    if (mod == 3) {
        return 0;
    }
    if (rm == 4) {
        const unsigned base = b[i++] & 7;
        if (mod == 0 && base == 5) {
            i += 4;
        }
    } else if (mod == 0 && rm == 5) {
        i += 4;
    }
    if (mod == 1) {
        i += 1;
    } else if (mod == 2) {
        i += 4;
    }
    return i;
}

Patch makeStubPatch(void* function, std::span<const std::byte> stub, const char* name)
{
    auto* const head = static_cast<std::byte*>(function);
    const std::size_t span = 2 + 127 + stub.size() + 16;
    if (head == nullptr || stub.empty() || stub.size() > 32 || !memory::isReadable(head, span)) {
        return Patch();
    }
    std::byte* place = nullptr;
    std::byte* const limit = head + span;
    for (std::byte* run = head + 2; run < limit && place == nullptr;) {
        if (*run != std::byte{0xCC}) {
            ++run;
            continue;
        }
        std::byte* runEnd = run;
        while (runEnd < limit && *runEnd == std::byte{0xCC}) {
            ++runEnd;
        }
        std::byte* const candidate = runEnd - stub.size();
        if ((reinterpret_cast<std::uintptr_t>(runEnd) & 15) == 0
            && static_cast<std::size_t>(runEnd - run) >= stub.size() + 4
            && candidate - (head + 2) <= 127) {
            place = candidate;
        }
        run = runEnd;
    }
    if (place == nullptr) {
        return Patch();
    }
    const auto rel = static_cast<std::uint8_t>(place - (head + 2));
    Patch patch(head, {std::byte{0xEB}, static_cast<std::byte>(rel)}, name);
    if (!patch.valid()) {
        return patch;
    }
    EnterCriticalSection(frozenLock());
    DWORD oldProtect = 0;
    bool written = false;
    if (VirtualProtect(place, stub.size(), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        std::memcpy(place, stub.data(), stub.size());
        DWORD ignored = 0;
        VirtualProtect(place, stub.size(), oldProtect, &ignored);
        FlushInstructionCache(GetCurrentProcess(), place, stub.size());
        written = true;
    }
    LeaveCriticalSection(frozenLock());
    if (!written) {
        return Patch();
    }
    patch.m_lockFree = true;
    return patch;
}

Patch makeAtomicPatch(void* address, std::vector<std::byte> bytes, const char* name)
{
    if (address == nullptr || bytes.empty() || bytes.size() > 8) {
        return Patch();
    }
    Patch patch(address, std::move(bytes), name);
    patch.m_lockFree = patch.valid();
    return patch;
}

Patch makeSkipPatch(void* address, size_t size, const char* name)
{
    if (address == nullptr || size < 2 || size > 129) {
        return Patch();
    }
    return makeAtomicPatch(address, {std::byte{0xEB}, static_cast<std::byte>(size - 2)}, name);
}

}
