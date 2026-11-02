#include "game/Structure.h"

#include <algorithm>
#include <limits>
#include <new>
#include <Windows.h>

#include "game/Inflate.h"
#include "game/Litematic.h"
#include "game/Nbt.h"

namespace tsukuyomi::structure {

namespace {

bool readTriple(const nbt::Value* value, std::int32_t& x, std::int32_t& y, std::int32_t& z)
{
    if (!value || value->tag != nbt::Tag::List || value->listItem != nbt::Tag::Int
        || value->ints.size() != 3) return false;
    x = value->ints[0]; y = value->ints[1]; z = value->ints[2];
    return true;
}

bool readIntLayer(nbt::Value& layer, std::size_t volume, std::size_t paletteSize, std::vector<std::uint16_t>& out)
{
    if (layer.tag != nbt::Tag::IntArray
        && (layer.tag != nbt::Tag::List || layer.listItem != nbt::Tag::Int)) return false;
    if (layer.ints.size() != volume) return false;
    out.resize(volume);
    for (std::size_t i = 0; i < volume; ++i) {
        const auto entry = layer.ints[i];
        out[i] = entry < 0 ? 0 : static_cast<std::size_t>(entry) >= paletteSize
            ? 0xFFFFu : static_cast<std::uint16_t>(entry + 1);
    }
    std::vector<std::int32_t>().swap(layer.ints);
    return true;
}

}

std::string StateValue::normalized() const
{
    if (tag == nbt::Tag::String) {
        return text;
    }
    return std::to_string(number);
}

std::string PaletteEntry::key() const
{
    std::string out = name;
    if (states.empty()) {
        return out;
    }
    out += '[';
    for (std::size_t i = 0; i < states.size(); ++i) {
        if (i != 0) {
            out += ',';
        }
        out += states[i].name;
        out += '=';
        out += states[i].normalized();
    }
    out += ']';
    return out;
}

std::size_t Structure::indexOf(std::int32_t x, std::int32_t y, std::int32_t z) const
{
    if (x < 0 || y < 0 || z < 0 || x >= sizeX || y >= sizeY || z >= sizeZ) {
        return blocks.size();
    }
    const std::size_t at = (static_cast<std::size_t>(x) * static_cast<std::size_t>(sizeY)
                            + static_cast<std::size_t>(y))
                               * static_cast<std::size_t>(sizeZ)
                           + static_cast<std::size_t>(z);
    return at < blocks.size() ? at : blocks.size();
}

LoadResult loadBytes(const char* bytes, std::size_t size)
try {
    if (inflate::isGzip(bytes, size)) return litematic::loadBytes(bytes, size);

    LoadResult result;

    const nbt::Result parsed = nbt::read(bytes, size);
    if (!parsed.root) {
        result.why = parsed.why != nullptr ? parsed.why : "could not read the NBT";
        return result;
    }
    const nbt::Value& root = *parsed.root;

    if (!readTriple(root.find("size"), result.value.sizeX, result.value.sizeY,
                    result.value.sizeZ)) {
        result.why = "no usable size";
        return result;
    }
    if (result.value.sizeX <= 0 || result.value.sizeY <= 0 || result.value.sizeZ <= 0) {
        result.why = "size is not positive";
        return result;
    }
    const std::size_t volume = result.value.volume();

    const nbt::Value* const structureNode = root.find("structure", nbt::Tag::Compound);
    if (structureNode == nullptr) {
        result.why = "no structure node";
        return result;
    }
    const nbt::Value* const palette = structureNode->find("palette", nbt::Tag::Compound);
    const nbt::Value* const preset =
        palette != nullptr ? palette->find("default", nbt::Tag::Compound) : nullptr;
    const nbt::Value* const entries =
        preset != nullptr ? preset->find("block_palette", nbt::Tag::List) : nullptr;
    if (entries == nullptr) {
        result.why = "no block palette";
        return result;
    }
    if (entries->list.size() > 65534) {
        result.why = "palette too large";
        return result;
    }
    result.value.palette.reserve(entries->list.size());
    for (const auto& one : entries->list) {
        PaletteEntry entry;
        if (one && one->tag == nbt::Tag::Compound) {
            if (const nbt::Value* const name = one->find("name", nbt::Tag::String)) {
                entry.name = name->text;
            }
            if (const nbt::Value* const version = one->find("version", nbt::Tag::Int)) {
                entry.version = static_cast<std::int32_t>(version->number);
            }
            if (const nbt::Value* const states = one->find("states", nbt::Tag::Compound)) {
                entry.states.reserve(states->compound.size());
                for (const auto& [key, value] : states->compound) {
                    if (!value) {
                        continue;
                    }
                    StateValue state;
                    state.name = key;
                    state.tag = value->tag;
                    state.number = value->number;
                    state.text = value->text;
                    entry.states.push_back(std::move(state));
                }
                std::sort(entry.states.begin(), entry.states.end(),
                          [](const StateValue& a, const StateValue& b) { return a.name < b.name; });
            }
        }
        result.value.palette.push_back(std::move(entry));
    }

    const nbt::Value* const layers = structureNode->find("block_indices", nbt::Tag::List);
    if (layers == nullptr || layers->list.empty()) {
        result.why = "no block indices";
        return result;
    }
    nbt::Value* const first = layers->list[0].get();
    if (first == nullptr || (first->tag != nbt::Tag::List && first->tag != nbt::Tag::IntArray)) {
        result.why = "block indices layer is not a list";
        return result;
    }
    if (!readIntLayer(*first, volume, result.value.palette.size(), result.value.blocks)) {
        result.why = "block count does not match the size";
        return result;
    }

    if (layers->list.size() >= 2) {
        nbt::Value* const second = layers->list[1].get();
        if (second != nullptr) {
            std::vector<std::uint16_t> extra;
            if (readIntLayer(*second, volume, result.value.palette.size(), extra)) {
                const bool used = std::any_of(extra.begin(), extra.end(),
                                              [](const std::uint16_t one) { return one != 0; });
                if (used) {
                    result.value.blocks2 = std::move(extra);
                }
            }
        }
    }

    return result;
}

catch (const std::bad_alloc&) {
    LoadResult result;
    result.why = "out of memory";
    return result;
}

LoadResult loadFile(const std::filesystem::path& path)
{
    LoadResult result;
    if (path.empty()) { result.why = "no path"; return result; }
    struct Mapping {
        HANDLE file = INVALID_HANDLE_VALUE;
        HANDLE mapping = nullptr;
        const char* view = nullptr;
        ~Mapping() {
            if (view) UnmapViewOfFile(view);
            if (mapping) CloseHandle(mapping);
            if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        }
    } mapped;
    mapped.file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (mapped.file == INVALID_HANDLE_VALUE) {
        result.why = "could not open the file"; return result;
    }
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(mapped.file, &length) || length.QuadPart < 0) {
        result.why = "could not size the file"; return result;
    }
    if (length.QuadPart == 0) { result.why = "the file is empty"; return result; }
    const auto bytes = static_cast<std::uint64_t>(length.QuadPart);
    constexpr std::uint64_t margin = 512ull * 1024 * 1024;
    const auto max = (std::numeric_limits<std::uint64_t>::max)();
    const auto extra = bytes / 2 + bytes % 2;
    result.requiredMemory = bytes > max - extra - margin ? max : bytes + extra + margin;
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status)) {
        result.why = "could not query free memory"; return result;
    }
    result.availableMemory = status.ullAvailPhys;
    if (result.availableMemory < result.requiredMemory) {
        result.why = "not enough free memory"; return result;
    }
    mapped.mapping = CreateFileMappingW(mapped.file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapped.mapping) { result.why = "could not create the file mapping"; return result; }
    mapped.view = static_cast<const char*>(MapViewOfFile(mapped.mapping, FILE_MAP_READ, 0, 0, 0));
    if (!mapped.view) { result.why = "could not map the file"; return result; }
    auto loaded = loadBytes(mapped.view, static_cast<std::size_t>(bytes));
    if (loaded.requiredMemory < result.requiredMemory) {
        loaded.requiredMemory = result.requiredMemory;
        loaded.availableMemory = result.availableMemory;
    }
    return loaded;
}

}
