#include "memory/Memory.h"

#include <cstdint>
#include <cstring>

#include <Windows.h>

#include <Psapi.h>

namespace tsukuyomi::memory {

namespace {

bool checkAccess(const void* address, size_t size, DWORD allowedProtection)
{
    if (address == nullptr || size == 0) {
        return false;
    }

    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(address, &info, sizeof(info)) == 0) {
        return false;
    }

    if (info.State != MEM_COMMIT) {
        return false;
    }
    if ((info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
        return false;
    }
    if ((info.Protect & allowedProtection) == 0) {
        return false;
    }

    const auto start = reinterpret_cast<uintptr_t>(address);
    const auto regionEnd = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    return start + size <= regionEnd;
}

}

bool isReadable(const void* address, size_t size)
{
    constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY
                                | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE
                                | PAGE_EXECUTE_WRITECOPY;
    return checkAccess(address, size, kReadable);
}

bool isWritable(const void* address, size_t size)
{
    constexpr DWORD kWritable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE
                                | PAGE_EXECUTE_WRITECOPY;
    return checkAccess(address, size, kWritable);
}

bool isExecutable(const void* address, size_t size)
{
    constexpr DWORD kExecutable = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE
                                  | PAGE_EXECUTE_WRITECOPY;
    return checkAccess(address, size, kExecutable);
}

bool inGameModule(const void* address)
{
    if (address == nullptr) {
        return false;
    }
    static const struct Range {
        std::uintptr_t begin = 0;
        std::uintptr_t end = 0;
    } range = [] {
        Range out;
        const HMODULE module = GetModuleHandleW(nullptr);
        if (module == nullptr) {
            return out;
        }
        MODULEINFO info{};
        if (GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info)) == 0) {
            return out;
        }
        out.begin = reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll);
        out.end = out.begin + info.SizeOfImage;
        return out;
    }();
    if (range.begin == 0) {
        return false;
    }
    const auto at = reinterpret_cast<std::uintptr_t>(address);
    return at >= range.begin && at < range.end;
}

void* ripTarget(const std::byte* at, std::size_t dispOffset)
{
    if (at == nullptr || !isReadable(at + dispOffset, sizeof(std::int32_t))) {
        return nullptr;
    }
    std::int32_t displacement = 0;
    std::memcpy(&displacement, at + dispOffset, sizeof(displacement));
    const auto* const next = at + dispOffset + sizeof(displacement);
    return const_cast<std::byte*>(next) + displacement;
}

std::size_t functionSize(const void* address)
{
    if (address == nullptr || !inGameModule(address)) {
        return 0;
    }
    std::uintptr_t imageBase = 0;
    const PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(
        reinterpret_cast<DWORD64>(address), &imageBase, nullptr);
    if (entry == nullptr || imageBase == 0) {
        return 0;
    }
    if (entry->EndAddress <= entry->BeginAddress) {
        return 0;
    }
    return static_cast<std::size_t>(entry->EndAddress - entry->BeginAddress);
}

}
