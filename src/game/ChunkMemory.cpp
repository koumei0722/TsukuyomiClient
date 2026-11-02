#include "game/ChunkMemory.h"
#include "game/ChunkResend.h"
#include "memory/Memory.h"
#include <Windows.h>
#include <cstring>
#include <algorithm>

namespace tsukuyomi::chunkmem {
namespace {
LONG accessFilter(DWORD code)
{
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR
        ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}
bool readPointer(const void* at, void*& out)
{
    __try { out = *static_cast<void* const*>(at); return true; }
    __except (accessFilter(GetExceptionCode())) { return false; }
}
bool readByte(const void* at, std::uint8_t& out)
{
    __try { out = *static_cast<const std::uint8_t*>(at); return true; }
    __except (accessFilter(GetExceptionCode())) { return false; }
}
bool looksLikeGameObject(const void* p)
{
    void* table = nullptr;
    return memory::plausiblePointer(p) && readPointer(p, table) && memory::inGameModule(table);
}
}
using StorageGetFn = const void*(__fastcall*)(void*, unsigned int);
void* chunkSource(void* region)
{
    void* source = nullptr;
    return region && readPointer(static_cast<std::uint8_t*>(region) + 0x28, source) ? source : nullptr;
}
namespace {
bool readGrid(void* region, ChunkRef* out, int& count)
{
    count = 0;
    __try {
        const auto* source = static_cast<const std::uint8_t*>(chunkSource(region));
        if (!source) return false;
        const int ox = *reinterpret_cast<const int*>(source + kChunkSourceOriginX);
        const int oz = *reinterpret_cast<const int*>(source + kChunkSourceOriginZ);
        const int width = *reinterpret_cast<const int*>(source + kChunkSourceWidth);
        const auto begin = *reinterpret_cast<const std::uintptr_t*>(source + 0x170);
        const auto end = *reinterpret_cast<const std::uintptr_t*>(source + 0x178);
        if (width <= 0 || width > 128 || end <= begin || end - begin != static_cast<std::uintptr_t>(width * width) * 16) return false;
        for (int i = 0; i < width * width; ++i) {
            auto* chunk = *reinterpret_cast<std::uint8_t* const*>(begin + static_cast<std::uintptr_t>(i) * 16);
            if (!chunk) continue;
            const auto* pos = reinterpret_cast<const int*>(chunk + kLevelChunkPosition);
            const auto dx = static_cast<std::int64_t>(pos[0]) - ox, dz = static_cast<std::int64_t>(pos[1]) - oz;
            if (dx >= 0 && dz >= 0 && dx < width && dz < width && (dz * width + dx == i || dx * width + dz == i))
                out[count++] = {pos[0], pos[1], chunk};
        }
        return true;
    } __except (accessFilter(GetExceptionCode())) { return false; }
}
}
bool listChunks(void* region, std::vector<ChunkRef>& out)
{
    out.resize(128 * 128);
    int count = 0;
    const bool ok = readGrid(region, out.data(), count);
    out.resize(ok ? count : 0);
    return ok;
}
bool shapeOf(void* chunk, ChunkShape& out)
{
    __try {
        auto* p = static_cast<std::uint8_t*>(chunk);
        if (!p) return false;
        out.minY = *reinterpret_cast<const int*>(p + kLevelChunkMinY);
        out.maxY = *reinterpret_cast<const int*>(p + kLevelChunkMaxY);
        out.table = *reinterpret_cast<std::uint8_t**>(p + kLevelChunkSubChunks);
        const auto tableEnd = *reinterpret_cast<std::uintptr_t*>(p + 0x1d0);
        const auto begin = reinterpret_cast<std::uintptr_t>(out.table);
        if (out.minY > out.maxY || out.minY < -1024 || out.maxY > 4095 || tableEnd < begin
            || (tableEnd - begin) % kSubChunkBytes || (tableEnd - begin) / kSubChunkBytes > 64) return false;
        out.subCount = static_cast<int>((tableEnd - begin) / kSubChunkBytes);
        out.biomeBegin = *reinterpret_cast<void**>(p + kLevelChunkBiomesBegin);
        const auto biomeEnd = *reinterpret_cast<std::uintptr_t*>(p + kLevelChunkBiomesEnd);
        const auto biomeBegin = reinterpret_cast<std::uintptr_t>(out.biomeBegin);
        if (biomeEnd < biomeBegin || (biomeEnd - biomeBegin) % 8 || (biomeEnd - biomeBegin) / 8 > 64) return false;
        out.biomeCount = static_cast<int>((biomeEnd - biomeBegin) / 8);
        return true;
    } __except (accessFilter(GetExceptionCode())) { return false; }
}
bool viewSub(std::uint8_t* sub, SubView& out)
{
    __try {
        out.sky = *reinterpret_cast<const std::uint8_t**>(sub + kSubChunkSkyLight);
        out.block = *reinterpret_cast<const std::uint8_t**>(sub + kSubChunkBlockLight);
        out.storage[0] = *reinterpret_cast<void**>(sub + kSubChunkStorage);
        out.storage[1] = *reinterpret_cast<void**>(sub + kSubChunkStorage + 8);
        const auto offset = chunkresend::subChunkStatusOffset();
        out.status = offset > 0 ? *reinterpret_cast<const std::int32_t*>(sub + offset) : -1;
        return true;
    } __except (accessFilter(GetExceptionCode())) { return false; }
}
bool readStorage(void* storage, const void* out[4096])
{
    __try {
        if (!storage) return false;
        const auto get = reinterpret_cast<StorageGetFn>((*static_cast<void***>(storage))[3]);
        if (!memory::inGameModule(reinterpret_cast<const void*>(get))) return false;
        for (unsigned i = 0; i < 4096; ++i) { out[i] = get(storage, i); if (!out[i]) return false; }
        return true;
    } __except (accessFilter(GetExceptionCode())) { return false; }
}
bool copyLight(const std::uint8_t* array, std::uint8_t out[2048])
{
    __try { if (!array) return false; std::memcpy(out, array, 2048); return true; }
    __except (accessFilter(GetExceptionCode())) { return false; }
}
namespace {
void* biomeStorage(void* chunk, int section)
{
    const auto* p = static_cast<const std::uint8_t*>(chunk);
    void* begin = nullptr; void* end = nullptr;
    if (!memory::plausiblePointer(chunk) || !readPointer(p + kLevelChunkBiomesBegin, begin)
        || !readPointer(p + kLevelChunkBiomesEnd, end) || !memory::plausiblePointer(begin) || end <= begin) return nullptr;
    const auto count = (static_cast<std::uint8_t*>(end) - static_cast<std::uint8_t*>(begin)) / 8;
    if (count <= 0 || count > 64 || section < 0) return nullptr;
    section = std::min(section, static_cast<int>(count) - 1);
    void* storage = nullptr;
    for (; section >= 0; --section) {
        if (readPointer(static_cast<std::uint8_t*>(begin) + section * 8, storage) && storage != nullptr) break;
    }
    return storage && looksLikeGameObject(storage) ? storage : nullptr;
}
}
const void* biomeAt(void* chunk, int x, int y, int z)
{
    int minY = 0;
    if (!memory::plausiblePointer(chunk) || !memory::copyGuarded(static_cast<std::uint8_t*>(chunk) + kLevelChunkMinY, &minY, sizeof(minY))) return nullptr;
    void* storage = biomeStorage(chunk, (y >> 4) - (minY >> 4));
    return storage ? storageGet(storage, static_cast<unsigned>(((x & 15) << 8) | ((z & 15) << 4) | (y & 15))) : nullptr;
}
bool readBiomes(void* chunk, int section, const void* out[64])
{
    void* storage = biomeStorage(chunk, section);
    if (!storage) return false;
    for (unsigned y = 0; y < 4; ++y) for (unsigned z = 0; z < 4; ++z) for (unsigned x = 0; x < 4; ++x) {
        out[(y << 4) | (z << 2) | x] = storageGet(storage, (x << 10) | (z << 6) | (y << 2));
        if (!out[(y << 4) | (z << 2) | x]) return false;
    }
    return true;
}
bool biomeName(const void* biome, char* out, std::size_t cap)
{
    return looksLikeGameObject(biome) && readGameString(static_cast<const std::uint8_t*>(biome) + kBiomeName, out, cap) && std::strchr(out, ':');
}
std::int32_t readDayTime(void* region)
{
    __try {
        const auto* level = *reinterpret_cast<const std::uint8_t* const*>(static_cast<std::uint8_t*>(region) + 0x20);
        if (!looksLikeGameObject(level)) return 6000;
        const auto* data = *reinterpret_cast<const std::uint8_t* const*>(level + 0x90);
        if (!memory::plausiblePointer(data)) return 6000;
        const auto time = *reinterpret_cast<const std::int32_t*>(data + 0x340);
        return time >= 0 ? time : 6000;
    } __except (accessFilter(GetExceptionCode())) { return 6000; }
}
bool readLightNibble(const void* subChunk, std::ptrdiff_t at, int index, int& out)
{
    void* array = nullptr;
    if (!readPointer(static_cast<const std::uint8_t*>(subChunk) + at, array)) {
        return false;
    }
    if (array == nullptr) {
        out = 0;
        return true;
    }
    std::uint8_t packed = 0;
    if (!memory::plausiblePointer(array)
        || !readByte(static_cast<std::uint8_t*>(array) + index / 2, packed)) {
        return false;
    }
    out = (index & 1) != 0 ? (packed >> 4) : (packed & 0x0F);
    return true;
}

bool chunkPositionIs(const std::uint8_t* chunk, int cx, int cz)
{
    const auto* const pos = reinterpret_cast<const std::int32_t*>(chunk + kLevelChunkPosition);
    return pos[0] == cx && pos[1] == cz;
}

void* gridChunkAt(void* region, int x, int z)
{
    __try {
        const auto* const source = *reinterpret_cast<const std::uint8_t* const*>(static_cast<std::uint8_t*>(region) + 0x28);
        if (source == nullptr) {
            return nullptr;
        }
        const std::int32_t originX = *reinterpret_cast<const std::int32_t*>(source + kChunkSourceOriginX);
        const std::int32_t originZ = *reinterpret_cast<const std::int32_t*>(source + kChunkSourceOriginZ);
        const std::int32_t width = *reinterpret_cast<const std::int32_t*>(source + kChunkSourceWidth);
        const auto begin = *reinterpret_cast<const std::uintptr_t*>(source + 0x170);
        const auto end = *reinterpret_cast<const std::uintptr_t*>(source + 0x178);
        if (width <= 0 || width > 128 || end <= begin || (end - begin) / 16 != static_cast<std::uintptr_t>(width) * width) {
            return nullptr;
        }
        const int cx = x >> 4;
        const int cz = z >> 4;
        const int dx = cx - originX;
        const int dz = cz - originZ;
        if (dx < 0 || dz < 0 || dx >= width || dz >= width) {
            return nullptr;
        }
        for (const int index : {dz * width + dx, dx * width + dz}) {
            const auto* const chunk = *reinterpret_cast<const std::uint8_t* const*>(begin + static_cast<std::uintptr_t>(index) * 16);
            if (chunk != nullptr && chunkPositionIs(chunk, cx, cz)) {
                return const_cast<std::uint8_t*>(chunk);
            }
        }
        return nullptr;
    } __except (accessFilter(GetExceptionCode())) {
        return nullptr;
    }
}

bool readGameString(const void* at, char* out, std::size_t cap)
{
    __try {
        const auto* const bytes = static_cast<const std::uint8_t*>(at);
        const std::uint64_t size = *reinterpret_cast<const std::uint64_t*>(bytes + 0x10);
        const std::uint64_t capacity = *reinterpret_cast<const std::uint64_t*>(bytes + 0x18);
        if (size == 0 || size >= cap || capacity < size || capacity > 0x100000) {
            return false;
        }
        const char* const text =
            capacity <= 15 ? reinterpret_cast<const char*>(bytes) : *reinterpret_cast<const char* const*>(bytes);
        for (std::uint64_t i = 0; i < size; ++i) {
            const auto c = static_cast<unsigned char>(text[i]);
            if (c < 0x20 || c >= 0x7f) {
                return false;
            }
            out[i] = static_cast<char>(c);
        }
        out[size] = '\0';
        return true;
    } __except (accessFilter(GetExceptionCode())) {
        return false;
    }
}

const void* storageGet(void* storage, unsigned int index)
{
    __try {
        void** const vtable = *reinterpret_cast<void***>(storage);
        const auto get = reinterpret_cast<StorageGetFn>(vtable[3]);
        if (!memory::inGameModule(reinterpret_cast<const void*>(get))) {
            return nullptr;
        }
        return get(storage, index);
    } __except (accessFilter(GetExceptionCode())) {
        return nullptr;
    }
}

void* subChunkInChunk(std::uint8_t* chunk, int y, bool& aboveTop)
{
    aboveTop = false;
    __try {
        const std::int32_t minY = *reinterpret_cast<const std::int32_t*>(chunk + kLevelChunkMinY);
        const std::int32_t maxY = *reinterpret_cast<const std::int32_t*>(chunk + kLevelChunkMaxY);
        if (y < minY || y > maxY) {
            return nullptr;
        }
        auto* const table = *reinterpret_cast<std::uint8_t**>(chunk + 0x1c8);
        auto* const tableEnd = *reinterpret_cast<std::uint8_t**>(chunk + 0x1d0);
        const std::ptrdiff_t index = (y >> 4) - (minY >> 4);
        if (table == nullptr || tableEnd < table || index < 0) {
            return nullptr;
        }
        if (index >= (tableEnd - table) / 0x68) {
            aboveTop = true;
            return nullptr;
        }
        return table + index * 0x68;
    } __except (accessFilter(GetExceptionCode())) {
        return nullptr;
    }
}
}
