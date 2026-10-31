#include "GameString.h"
#include "hooks/Detours.h"
#include "memory/Memory.h"
#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <limits>

namespace tsukuyomi::bot::gamestring {
namespace {
int memoryException(unsigned code)
{
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}
bool readGuarded(const void* string, char* out, std::size_t capacity, std::size_t* length)
{
    __try {
        const auto* p = static_cast<const std::uint8_t*>(string);
        const auto size = *reinterpret_cast<const std::size_t*>(p + 0x10);
        const auto reserved = *reinterpret_cast<const std::size_t*>(p + 0x18);
        if (size > reserved || size > capacity) return false;
        const auto* source = reserved >= 16 ? *reinterpret_cast<const char* const*>(p) : reinterpret_cast<const char*>(p);
        std::memcpy(out, source, size);
        *length = size;
        return true;
    } __except (memoryException(GetExceptionCode())) { return false; }
}
}

void* allocate(std::size_t size)
{
    constexpr std::size_t extra = 31 + sizeof(void*);
    if (!size || size > (std::numeric_limits<std::size_t>::max)() - extra) return nullptr;
    const bool aligned = size >= 4096;
    void* const raw = hooks::callGameAllocate(size + (aligned ? extra : 0));
    if (!raw) return nullptr;
    if (!aligned) return raw;
    auto* const data = reinterpret_cast<std::uint8_t*>((reinterpret_cast<std::uintptr_t>(raw) + extra) & ~std::uintptr_t{31});
    std::memcpy(data - sizeof(void*), &raw, sizeof raw);
    return data;
}
bool free(void* data, std::size_t size)
{
    if (!data) return true;
    void* raw = data;
    if (size >= 4096) {
        if (!memory::copyGuarded(static_cast<std::uint8_t*>(data) - sizeof(void*), &raw, sizeof raw)) return false;
        const auto gap = reinterpret_cast<std::uintptr_t>(data) - reinterpret_cast<std::uintptr_t>(raw);
        if (gap < sizeof(void*) || gap > 31 + sizeof(void*)) return false;
    }
    return hooks::callGameFree(raw);
}
void initialize(void* string)
{
    std::memset(string, 0, 0x20);
    const std::size_t capacity = 15;
    std::memcpy(static_cast<std::uint8_t*>(string) + 0x18, &capacity, sizeof capacity);
}

std::optional<std::string> read(const void* string)
{
    std::size_t length = 0;
    if (!memory::copyGuarded(static_cast<const std::uint8_t*>(string) + 0x10, &length, sizeof length) || length > (1u << 20)) return std::nullopt;
    std::string out(length, '\0');
    if (!readGuarded(string, out.data(), out.size(), &length)) return std::nullopt;
    out.resize(length);
    return out;
}

bool assign(void* string, std::string_view bytes)
{
    std::size_t reserved = 0;
    if (!memory::isWritable(string, 0x20)
        || !memory::copyGuarded(static_cast<std::uint8_t*>(string) + 0x18, &reserved, sizeof reserved)
        || reserved < 15) return false;
    if (bytes.size() <= reserved) {
        void* data = string;
        if (reserved >= 16 && !memory::copyGuarded(string, &data, sizeof data)) return false;
        if (bytes.size() == (std::numeric_limits<std::size_t>::max)()
            || !memory::isWritable(data, bytes.size() + 1)) return false;
        if (!bytes.empty()) std::memmove(data, bytes.data(), bytes.size());
        static_cast<char*>(data)[bytes.size()] = '\0';
        const auto length = bytes.size();
        std::memcpy(static_cast<std::uint8_t*>(string) + 0x10, &length, sizeof length);
        return true;
    }
    if (bytes.size() == (std::numeric_limits<std::size_t>::max)()) return false;
    const auto size = bytes.size() + 1;
    auto* const data = static_cast<char*>(allocate(size));
    if (!data) return false;
    std::memcpy(data, bytes.data(), bytes.size());
    data[bytes.size()] = '\0';
    if (!destroy(string)) { free(data, size); return false; }
    const std::size_t fields[2] = {bytes.size(), bytes.size()};
    std::memcpy(string, &data, sizeof data);
    std::memcpy(static_cast<std::uint8_t*>(string) + 0x10, fields, sizeof fields);
    return true;
}

bool destroy(void* string)
{
    std::size_t reserved = 0;
    void* data = nullptr;
    if (!memory::isWritable(string, 0x20)
        || !memory::copyGuarded(static_cast<std::uint8_t*>(string) + 0x18, &reserved, sizeof reserved)) return false;
    if (reserved == (std::numeric_limits<std::size_t>::max)()) return false;
    if (reserved >= 16) {
        if (!memory::copyGuarded(string, &data, sizeof data) || !data) return false;
        if (!free(data, reserved + 1)) return false;
    }
    initialize(string);
    return true;
}

}
