#include "game/JsonValueMemory.h"

#include "memory/Memory.h"

#include <cstring>
#include <vector>

namespace tsukuyomi::jsonmem {
namespace {
constexpr std::size_t kHolder = 0x00, kType = 0x08;
constexpr std::size_t kMapHead = 0x00, kMapSize = 0x08;
constexpr std::size_t kVectorBegin = 0x00, kVectorEnd = 0x08;
constexpr std::size_t kLeft = 0x00, kParent = 0x08, kRight = 0x10, kIsNil = 0x19;
constexpr std::size_t kKey = 0x20, kNodeValue = 0x28;
constexpr std::uint32_t kMaxEntries = 4096;
constexpr std::uint32_t kMaxText = 256;

template<class T> bool read(const void* base, std::size_t offset, T& out)
{
    return base != nullptr && memory::copyGuarded(static_cast<const char*>(base) + offset, &out, sizeof(out));
}

bool readText(const char* text, std::string& out)
{
    out.clear();
    if (!memory::plausiblePointer(text)) return false;
    for (std::uint32_t i = 0; i < kMaxText; ++i) {
        char c = 0;
        if (!memory::copyGuarded(text + i, &c, 1)) return false;
        if (c == '\0') return true;
        if (static_cast<unsigned char>(c) < 0x20) return false;
        out.push_back(c);
    }
    return false;
}

bool objectNodes(Value object, std::vector<const void*>& out)
{
    out.clear();
    Type type{};
    if (!typeOf(object, type) || type != Type::Object) return false;
    const void* map = nullptr;
    const void* head = nullptr;
    std::uint64_t size = 0;
    if (!read(object, kHolder, map) || map == nullptr || !read(map, kMapHead, head) || !read(map, kMapSize, size)
        || head == nullptr || size > kMaxEntries) return false;
    std::uint8_t nil = 0;
    const void* node = nullptr;
    if (!read(head, kIsNil, nil) || nil != 1 || !read(head, kParent, node)) return false;
    std::vector<const void*> stack;
    while ((node != head || !stack.empty()) && out.size() <= size) {
        while (node != head) {
            if (stack.size() > 64 || !read(node, kIsNil, nil) || nil != 0) return false;
            stack.push_back(node);
            if (!read(node, kLeft, node)) return false;
        }
        if (stack.empty()) break;
        node = stack.back();
        stack.pop_back();
        out.push_back(node);
        if (!read(node, kRight, node)) return false;
    }
    return out.size() == size;
}

bool arrayElements(Value array, std::vector<Value>& out)
{
    out.clear();
    Type type{};
    if (!typeOf(array, type) || type != Type::Array) return false;
    const void* vector = nullptr;
    const char* begin = nullptr;
    const char* end = nullptr;
    if (!read(array, kHolder, vector) || vector == nullptr || !read(vector, kVectorBegin, begin)
        || !read(vector, kVectorEnd, end) || end < begin || (end - begin) % 8 != 0
        || static_cast<std::size_t>(end - begin) / 8 > kMaxEntries) return false;
    const std::size_t n = static_cast<std::size_t>(end - begin) / 8;
    out.resize(n);
    return n == 0 || memory::copyGuarded(begin, out.data(), n * 8);
}

bool keyOf(const void* node, std::string& key)
{
    const char* text = nullptr;
    return read(node, kKey, text) && readText(text, key);
}
}

bool typeOf(Value value, Type& out)
{
    std::uint8_t type = 0;
    if (!read(value, kType, type) || type > static_cast<std::uint8_t>(Type::Object)) return false;
    out = static_cast<Type>(type);
    return true;
}

Value member(Value object, const char* name)
{
    std::vector<const void*> list;
    if (name == nullptr || !objectNodes(object, list)) return nullptr;
    std::string key;
    for (const void* node : list) {
        if (keyOf(node, key) && key == name) return static_cast<const char*>(node) + kNodeValue;
    }
    return nullptr;
}

Value element(Value array, std::uint32_t wanted)
{
    std::vector<Value> list;
    if (!arrayElements(array, list) || wanted >= list.size()) return nullptr;
    return list[wanted];
}

bool count(Value container, std::uint32_t& out)
{
    Type type{};
    if (!typeOf(container, type)) return false;
    if (type == Type::Array) {
        std::vector<Value> list;
        if (!arrayElements(container, list)) return false;
        out = static_cast<std::uint32_t>(list.size());
        return true;
    }
    std::vector<const void*> list;
    if (!objectNodes(container, list)) return false;
    out = static_cast<std::uint32_t>(list.size());
    return true;
}

bool asNumber(Value value, double& out)
{
    Type type{};
    if (!typeOf(value, type)) return false;
    switch (type) {
    case Type::Int: { std::int64_t v = 0; if (!read(value, kHolder, v)) return false; out = static_cast<double>(v); return true; }
    case Type::UInt: { std::uint64_t v = 0; if (!read(value, kHolder, v)) return false; out = static_cast<double>(v); return true; }
    case Type::Real: return read(value, kHolder, out);
    default: return false;
    }
}

}
