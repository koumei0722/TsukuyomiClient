#include "game/AvailableCommandsPatch.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/Logger.h"
#include "core/Paths.h"
#include "core/Strings.h"
#include "game/BlockRegistry.h"
#include "game/ChatCommandComplete.h"
#include "memory/Memory.h"

namespace tsukuyomi::availablecommands {
namespace {

constexpr std::size_t kPacketSize = 0xf0;
constexpr std::size_t kValues = 0x30;
constexpr std::size_t kEnums = 0x60;
constexpr std::size_t kCommands = 0xa8;
constexpr std::size_t kStringSize = 0x20;
constexpr std::size_t kEnumSize = 0x38;
constexpr std::size_t kCommandSize = 0x80;
constexpr std::size_t kOverloadSize = 0x20;
constexpr std::size_t kParamSize = 0x28;
constexpr std::uint32_t kEnumType = 0x300000;
constexpr std::uint16_t kNotCheatFlag = 0x80;
constexpr std::uint8_t kParamAutocompleteExpansion = 0x1;
static_assert(sizeof(std::string) == kStringSize);

struct RawVector { const std::byte* begin; const std::byte* end; const std::byte* capacity; };
static_assert(sizeof(RawVector) == 0x18);
std::mutex g_fileMutex;
std::atomic_bool g_missingLogged{false};
std::atomic_bool g_namesLogged{false};
std::atomic_bool g_faultLogged{false};
std::atomic_bool g_badPacketLogged{false};
std::atomic_bool g_cacheWriteLogged{false};
std::atomic_bool g_disablePatch{false};
std::atomic_bool g_flagsLogged{false};
std::atomic_bool g_registrySourceLogged{false};
std::atomic_bool g_packetSourceLogged{false};
std::atomic_bool g_cacheSourceLogged{false};

template<class T> T readAt(const std::byte* base, std::size_t offset)
{
    T result;
    std::memcpy(&result, base + offset, sizeof(result));
    return result;
}

template<class T> void writeAt(std::byte* base, std::size_t offset, const T& value)
{
    std::memcpy(base + offset, &value, sizeof(value));
}

bool getVector(const std::byte* base, std::size_t offset, std::size_t stride,
               RawVector& out, std::size_t& count)
{
    out = readAt<RawVector>(base, offset);
    const auto begin = reinterpret_cast<std::uintptr_t>(out.begin);
    const auto end = reinterpret_cast<std::uintptr_t>(out.end);
    const auto capacity = reinterpret_cast<std::uintptr_t>(out.capacity);
    if (begin == 0 && end == 0 && capacity == 0) { count = 0; return true; }
    if (begin == 0 || end < begin || capacity < end || (end - begin) % stride != 0) return false;
    count = (end - begin) / stride;
    return count <= (std::numeric_limits<std::size_t>::max)() / stride
        && (count == 0 || memory::isReadable(out.begin, count * stride));
}

bool getString(const std::byte* data, std::string& out)
{
    if (!memory::isReadable(data, kStringSize)) return false;
    const auto length = readAt<std::size_t>(data, 0x10);
    const auto capacity = readAt<std::size_t>(data, 0x18);
    if (length > capacity || length == (std::numeric_limits<std::size_t>::max)()) return false;
    const char* chars = capacity >= 16 ? readAt<const char*>(data, 0) : reinterpret_cast<const char*>(data);
    if (!memory::isReadable(chars, length + 1)) return false;
    out.assign(chars, length);
    return true;
}

struct OwnedBytes {
    std::vector<std::unique_ptr<std::byte[]>> blocks;
    std::byte* make(std::size_t size)
    {
        auto block = std::make_unique<std::byte[]>(size == 0 ? 1 : size);
        auto* result = block.get();
        std::memset(result, 0, size);
        blocks.push_back(std::move(block));
        return result;
    }
};

void putString(OwnedBytes& owned, std::byte* at, const std::string& value)
{
    const std::size_t length = value.size();
    const std::size_t capacity = length < 16 ? 15 : length;
    if (length < 16) std::memcpy(at, value.c_str(), length + 1);
    else {
        auto* chars = owned.make(length + 1);
        std::memcpy(chars, value.c_str(), length + 1);
        writeAt(at, 0, chars);
    }
    writeAt(at, 0x10, length);
    writeAt(at, 0x18, capacity);
}

void putVector(std::byte* at, const std::byte* begin, std::size_t size)
{
    const RawVector vec{begin, begin == nullptr ? nullptr : begin + size,
                        begin == nullptr ? nullptr : begin + size};
    std::memcpy(at, &vec, sizeof(vec));
}

std::byte* appendVector(OwnedBytes& owned, std::byte* packet, std::size_t offset,
                        const RawVector& source, std::size_t count, std::size_t added,
                        std::size_t stride)
{
    if (added > ((std::numeric_limits<std::size_t>::max)() / stride) - count)
        throw std::bad_alloc();
    const std::size_t size = (count + added) * stride;
    auto* result = owned.make(size);
    if (count != 0) std::memcpy(result, source.begin, count * stride);
    putVector(packet + offset, result, size);
    return result + count * stride;
}

std::filesystem::path itemFile()
{
    const auto& dir = paths::dataDir();
    return dir.empty() ? std::filesystem::path{} : dir / "ChatCommand" / "items.txt";
}

std::vector<std::string> readCache()
{
    std::lock_guard lock(g_fileMutex);
    std::vector<std::string> lines;
    const auto file = itemFile();
    if (file.empty()) return lines;
    std::ifstream input(file, std::ios::binary);
    for (std::string line; std::getline(input, line); ) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(std::move(line));
    }
    return lines;
}

void writeCache(const std::vector<std::string>& lines)
{
    std::lock_guard lock(g_fileMutex);
    const auto file = itemFile();
    if (file.empty()) return;
    std::vector<std::string> old;
    {
        std::ifstream input(file, std::ios::binary);
        for (std::string line; std::getline(input, line); ) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) old.push_back(std::move(line));
        }
    }
    if (old == lines) return;
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    for (const auto& line : lines) output << line << '\n';
    if ((!output || ec) && !g_cacheWriteLogged.exchange(true))
        log().warn(L"ChatCommand: item completion cache could not be written");
}

bool itemNames(const RawVector& values, std::size_t valueCount,
               const std::byte* enumData, std::vector<std::string>& out)
{
    RawVector refs{};
    std::size_t count = 0;
    if (!getVector(enumData, 0x20, sizeof(std::uint32_t), refs, count)) return false;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto index = readAt<std::uint32_t>(refs.begin, i * sizeof(std::uint32_t));
        if (index >= valueCount) return false;
        std::string name;
        if (!getString(values.begin + index * kStringSize, name)) return false;
        out.push_back(std::move(name));
    }
    return true;
}

bool makePacket(const void* source, OwnedBytes& owned, std::byte* packet,
                std::vector<std::string>& foundItems)
{
    if (source == nullptr || !memory::isReadable(source, kPacketSize)) return false;
    std::memcpy(packet, source, kPacketSize);
    RawVector values{}, enums{}, commands{};
    std::size_t valueCount = 0, enumCount = 0, commandCount = 0;
    if (!getVector(packet, kValues, kStringSize, values, valueCount)
        || !getVector(packet, kEnums, kEnumSize, enums, enumCount)
        || !getVector(packet, kCommands, kCommandSize, commands, commandCount)
        || enumCount > UINT32_MAX - 5) return false;

    std::optional<std::uint32_t> itemIndex;
    std::vector<std::string> nearNames;
    for (std::size_t i = 0; i < enumCount; ++i) {
        std::string name;
        if (!getString(enums.begin + i * kEnumSize, name)) return false;
        if (name == "Item") { itemIndex = static_cast<std::uint32_t>(i); break; }
        std::string lower = name;
        for (char& ch : lower) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
        if (lower.find("item") != std::string::npos) nearNames.push_back(std::move(name));
    }
    if (!itemIndex && !g_namesLogged.exchange(true)) {
        for (std::size_t i = 0; i < nearNames.size() && i < 32; ++i)
            log().info(L"ChatCommand: Item-like enum: {}", toUtf16(nearNames[i]));
    }

    std::vector<std::string> itemValues;
    if (itemIndex) {
        std::vector<std::string> packetItems;
        if (itemNames(values, valueCount, enums.begin + *itemIndex * kEnumSize, packetItems))
            foundItems = chatcommand::normalizeItemCandidates(packetItems);
        if (!g_packetSourceLogged.exchange(true))
            log().info(L"ChatCommand: item completion uses the Item enum of the packet ({} names)", foundItems.size());
    } else {
        foundItems = chatcommand::normalizeItemCandidates(blocks::registeredItemNames());
        if (foundItems.size() >= 100) {
            if (!g_registrySourceLogged.exchange(true))
                log().info(L"ChatCommand: item completion from ItemRegistry ({} names)", foundItems.size());
        } else {
            foundItems = chatcommand::normalizeItemCandidates(readCache());
            if (!foundItems.empty() && !g_cacheSourceLogged.exchange(true))
                log().info(L"ChatCommand: item completion from cache ({} names)", foundItems.size());
        }
        itemValues = chatcommand::itemEnumValues(foundItems);
    }
    const auto spec = chatcommand::buildTkCommandSpec(static_cast<std::uint32_t>(enumCount), itemIndex,
                                                       itemValues);
    if (foundItems.empty() && !g_missingLogged.exchange(true))
        log().warn(L"ChatCommand: item completion is unavailable; enter a cheats-enabled world once to cache it");
    std::size_t addedValues = 0;
    for (const auto& one : spec.enums) {
        if (one.values.size() > UINT32_MAX - addedValues) return false;
        addedValues += one.values.size();
    }
    if (valueCount > UINT32_MAX - addedValues) return false;
    auto* newValues = appendVector(owned, packet, kValues, values, valueCount, addedValues, kStringSize);
    auto* newEnums = appendVector(owned, packet, kEnums, enums, enumCount, spec.enums.size(), kEnumSize);
    std::uint32_t nextValue = static_cast<std::uint32_t>(valueCount);
    for (std::size_t i = 0; i < spec.enums.size(); ++i) {
        const auto& definition = spec.enums[i];
        auto* entry = newEnums + i * kEnumSize;
        putString(owned, entry, definition.name);
        auto* indices = owned.make(definition.values.size() * sizeof(std::uint32_t));
        for (std::size_t j = 0; j < definition.values.size(); ++j) {
            writeAt(indices, j * sizeof(std::uint32_t), nextValue++);
            putString(owned, newValues, definition.values[j]);
            newValues += kStringSize;
        }
        putVector(entry + 0x20, indices, definition.values.size() * sizeof(std::uint32_t));
    }

    if (!g_flagsLogged.exchange(true))
        log().info(L"ChatCommand: tk is added as a non-cheat command (flags {:#x})", kNotCheatFlag);

    auto* command = appendVector(owned, packet, kCommands, commands, commandCount, 1, kCommandSize);
    putString(owned, command, "tk");
    putString(owned, command + 0x20, "Tsukuyomi");
    writeAt<std::uint16_t>(command, 0x40, kNotCheatFlag);
    writeAt<std::uint8_t>(command, 0x42, 0);
    writeAt<std::int32_t>(command, 0x78, -1);
    auto* overloads = owned.make(spec.overloads.size() * kOverloadSize);
    putVector(command + 0x48, overloads, spec.overloads.size() * kOverloadSize);
    for (std::size_t i = 0; i < spec.overloads.size(); ++i) {
        const auto& definition = spec.overloads[i];
        auto* params = owned.make(definition.size() * kParamSize);
        putVector(overloads + i * kOverloadSize, params, definition.size() * kParamSize);
        for (std::size_t j = 0; j < definition.size(); ++j) {
            auto* param = params + j * kParamSize;
            putString(owned, param, definition[j].name);
            writeAt<std::uint32_t>(param, 0x20, kEnumType | definition[j].enumIndex);
            writeAt<std::uint8_t>(param, 0x24, definition[j].optional ? 1 : 0);
            writeAt<std::uint8_t>(param, 0x25, kParamAutocompleteExpansion);
        }
    }
    return true;
}

int faultFilter(unsigned long code)
{
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR
        ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

bool callOriginal(LoadPacketFn original, void* registry, const void* packet)
{
    __try { original(registry, packet); return true; }
    __except (faultFilter(GetExceptionCode())) { return false; }
}

}

void loadWithTk(LoadPacketFn original, void* registry, const void* packet)
{
    if (original == nullptr) return;
    if (g_disablePatch.load()) { callOriginal(original, registry, packet); return; }
    alignas(16) std::array<std::byte, 0x100> copy{};
    OwnedBytes owned;
    std::vector<std::string> foundItems;
    bool ready = false;
    try { ready = makePacket(packet, owned, copy.data(), foundItems); }
    catch (const std::exception&) { ready = false; }
    if (!ready) {
        if (!g_badPacketLogged.exchange(true)) log().warn(L"ChatCommand: completion packet could not be copied");
        callOriginal(original, registry, packet);
        return;
    }
    if (!callOriginal(original, registry, copy.data())) {
        g_disablePatch.store(true);
        if (!g_faultLogged.exchange(true)) log().error(L"ChatCommand: augmented command packet faulted; using the original packet");
        callOriginal(original, registry, packet);
        return;
    }
    if (!foundItems.empty()) {
        try { writeCache(foundItems); }
        catch (const std::exception&) {
            if (!g_cacheWriteLogged.exchange(true))
                log().warn(L"ChatCommand: item completion cache could not be written");
        }
    }
}

}
