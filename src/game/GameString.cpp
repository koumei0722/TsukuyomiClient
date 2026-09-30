#include "game/GameString.h"

#include "memory/Memory.h"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace tsukuyomi::gamestring {

namespace {

using AllocFn = void*(__fastcall*)(void* allocator, std::size_t size);
void** g_allocatorAt = nullptr;
DeleteFn g_gameDelete = nullptr;

bool readGuarded(const void* at, void* out, std::size_t size)
{
    __try {
        std::memcpy(out, at, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* allocGameGuarded(std::size_t size)
{
    __try {
        void* const allocator = *g_allocatorAt;
        if (allocator == nullptr) {
            return nullptr;
        }
        void** const vt = *static_cast<void***>(allocator);
        return reinterpret_cast<AllocFn>(vt[1])(allocator, size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool deleteGameGuarded(void* ptr, std::size_t size)
{
    __try {
        g_gameDelete(ptr, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}

void configure(void** allocatorAt, DeleteFn gameDelete)
{
    g_allocatorAt = allocatorAt;
    g_gameDelete = gameDelete;
}

bool available()
{
    return g_allocatorAt != nullptr && g_gameDelete != nullptr;
}

bool read(void* str, std::string& text)
{
    if (str == nullptr) return false;
    auto* const s = static_cast<std::byte*>(str);
    std::uint64_t size = 0;
    std::uint64_t cap = 0;
    if (!readGuarded(s + 0x10, &size, 8) || !readGuarded(s + 0x18, &cap, 8)
        || size > cap || cap > 0x100000) return false;
    const char* data = reinterpret_cast<const char*>(s);
    if (cap >= 16 && !readGuarded(s, &data, 8)) return false;
    if (data == nullptr) return false;
    text.resize(static_cast<std::size_t>(size));
    return size == 0 || readGuarded(data, text.data(), static_cast<std::size_t>(size));
}

bool assign(void* str, std::string_view text)
{
    if (str == nullptr || !available()) return false;
    auto* const s = static_cast<std::byte*>(str);
    std::uint64_t size = 0;
    std::uint64_t cap = 0;
    if (!readGuarded(s + 0x10, &size, 8) || !readGuarded(s + 0x18, &cap, 8) || size > cap || cap > 0x100000) {
        return false;
    }
    const std::size_t n = text.size();
    if (n <= cap) {
        char* data = reinterpret_cast<char*>(s);
        if (cap >= 16 && !readGuarded(s, &data, 8)) {
            return false;
        }
        if (data == nullptr || !memory::isWritable(data, n + 1)) {
            return false;
        }
        std::memcpy(data, text.data(), n);
        data[n] = 0;
        const std::uint64_t newSize = n;
        std::memcpy(s + 0x10, &newSize, 8);
        return true;
    }
    const std::uint64_t newCap = std::max<std::uint64_t>(static_cast<std::uint64_t>(n) | 0xF, 0x16);
    if (newCap + 1 >= 0x1000) {
        return false;
    }
    char* const fresh = static_cast<char*>(allocGameGuarded(newCap + 1));
    if (fresh == nullptr) {
        return false;
    }
    std::memcpy(fresh, text.data(), n);
    fresh[n] = 0;
    if (cap >= 16) {
        void* old = nullptr;
        if (readGuarded(s, &old, 8) && old != nullptr) {
            if (cap + 1 < 0x1000) {
                deleteGameGuarded(old, cap + 1);
            } else {
                void* real = nullptr;
                if (readGuarded(static_cast<std::byte*>(old) - 8, &real, 8) && real != nullptr) {
                    deleteGameGuarded(real, cap + 1 + 0x27);
                }
            }
        }
    }
    void* const freshPtr = fresh;
    const std::uint64_t newSize = n;
    std::memcpy(s, &freshPtr, 8);
    std::memcpy(s + 0x10, &newSize, 8);
    std::memcpy(s + 0x18, &newCap, 8);
    return true;
}

}
