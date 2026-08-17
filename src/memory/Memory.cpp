#include "memory/Memory.h"

#include <cstdint>
#include <cstring>

#include <Windows.h>

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

}
