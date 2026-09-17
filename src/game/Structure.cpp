#include "game/Structure.h"

#include <algorithm>
#include <fstream>

#include "game/Nbt.h"

namespace tsukuyomi::structure {

namespace {

const std::string kEmptyName;

constexpr std::size_t kMaxVolume = 4 * 1024 * 1024;

bool readTriple(const nbt::Value* value, std::int32_t& x, std::int32_t& y, std::int32_t& z)
{
    if (value == nullptr || value->tag != nbt::Tag::List) {
        return false;
    }
    if (value->numbers.size() == 3 && value->listItem == nbt::Tag::Int) {
        x = static_cast<std::int32_t>(value->numbers[0]);
        y = static_cast<std::int32_t>(value->numbers[1]);
        z = static_cast<std::int32_t>(value->numbers[2]);
        return true;
    }
    if (value->list.size() != 3) {
        return false;
    }
    for (const auto& one : value->list) {
        if (!one || one->tag != nbt::Tag::Int) {
            return false;
        }
    }
    x = static_cast<std::int32_t>(value->list[0]->number);
    y = static_cast<std::int32_t>(value->list[1]->number);
    z = static_cast<std::int32_t>(value->list[2]->number);
    return true;
}

bool readIntLayer(const nbt::Value& layer, std::size_t volume,
                  std::vector<std::int32_t>& out)
{
    if (layer.tag != nbt::Tag::List) {
        return false;
    }
    if (!layer.numbers.empty()) {
        if (layer.numbers.size() != volume) {
            return false;
        }
        out.reserve(volume);
        for (const std::int64_t one : layer.numbers) {
            out.push_back(static_cast<std::int32_t>(one));
        }
        return true;
    }
    if (layer.list.size() != volume) {
        return false;
    }
    out.reserve(volume);
    for (const auto& one : layer.list) {
        if (!one || one->tag != nbt::Tag::Int) {
            return false;
        }
        out.push_back(static_cast<std::int32_t>(one->number));
    }
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

std::int32_t Structure::entryAt(std::int32_t x, std::int32_t y, std::int32_t z, int layer) const
{
    const std::size_t at = indexOf(x, y, z);
    if (at >= blocks.size()) {
        return -1;
    }
    if (layer == 1) {
        return at < blocks2.size() ? blocks2[at] : -1;
    }
    return blocks[at];
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

const std::vector<StateValue>& Structure::entityAt(std::int32_t x, std::int32_t y,
                                                   std::int32_t z) const
{
    static const std::vector<StateValue> none;
    const std::size_t at = indexOf(x, y, z);
    if (at >= blocks.size()) {
        return none;
    }
    const auto found = entityStates.find(at);
    return found != entityStates.end() ? found->second : none;
}

const std::string& Structure::nameAt(std::int32_t x, std::int32_t y, std::int32_t z) const
{
    const std::size_t at = indexOf(x, y, z);
    if (at >= blocks.size()) {
        return kEmptyName;
    }
    const std::int32_t which = blocks[at];
    if (which < 0 || static_cast<std::size_t>(which) >= palette.size()) {
        return kEmptyName;
    }
    return palette[static_cast<std::size_t>(which)].name;
}

LoadResult loadBytes(const char* bytes, std::size_t size)
{
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
    if (volume > kMaxVolume) {
        result.why = "structure is too large";
        return result;
    }

    readTriple(root.find("structure_world_origin"), result.value.originX, result.value.originY,
               result.value.originZ);

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

    if (preset != nullptr) {
        const nbt::Value* const posData =
            preset->find("block_position_data", nbt::Tag::Compound);
        if (posData != nullptr) {
            for (const auto& [key, node] : posData->compound) {
                if (!node || node->tag != nbt::Tag::Compound) {
                    continue;
                }
                const nbt::Value* const data =
                    node->find("block_entity_data", nbt::Tag::Compound);
                if (data == nullptr) {
                    continue;
                }
                std::size_t at = 0;
                bool ok = !key.empty();
                for (const char c : key) {
                    if (c < '0' || c > '9' || at > volume) {
                        ok = false;
                        break;
                    }
                    at = at * 10 + static_cast<std::size_t>(c - '0');
                }
                if (!ok || at >= volume) {
                    continue;
                }
                std::vector<StateValue> fields;
                for (const auto& [name, field] : data->compound) {
                    if (!field) {
                        continue;
                    }
                    StateValue one;
                    one.name = name;
                    one.tag = field->tag;
                    switch (field->tag) {
                    case nbt::Tag::Byte:
                    case nbt::Tag::Short:
                    case nbt::Tag::Int:
                    case nbt::Tag::Long:
                        one.number = field->number;
                        break;
                    case nbt::Tag::String:
                        one.text = field->text;
                        break;
                    default:
                        continue;
                    }
                    fields.push_back(std::move(one));
                }
                if (!fields.empty()) {
                    std::sort(fields.begin(), fields.end(),
                              [](const StateValue& a, const StateValue& b) {
                                  return a.name < b.name;
                              });
                    result.value.entityStates.emplace(at, std::move(fields));
                }
            }
        }
    }

    const nbt::Value* const layers = structureNode->find("block_indices", nbt::Tag::List);
    if (layers == nullptr || layers->list.empty()) {
        result.why = "no block indices";
        return result;
    }
    const nbt::Value* const first = layers->list[0].get();
    if (first == nullptr || first->tag != nbt::Tag::List) {
        result.why = "block indices layer is not a list";
        return result;
    }
    if (!readIntLayer(*first, volume, result.value.blocks)) {
        result.why = "block count does not match the size";
        return result;
    }

    if (layers->list.size() >= 2) {
        const nbt::Value* const second = layers->list[1].get();
        if (second != nullptr) {
            std::vector<std::int32_t> extra;
            if (readIntLayer(*second, volume, extra)) {
                result.value.blocks2 = std::move(extra);
            }
        }
    }

    return result;
}

LoadResult loadFile(const std::filesystem::path& path)
{
    LoadResult result;
    if (path.empty()) {
        result.why = "no path";
        return result;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        result.why = "could not open the file";
        return result;
    }
    std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (bytes.empty()) {
        result.why = "the file is empty";
        return result;
    }
    return loadBytes(bytes.data(), bytes.size());
}

}
