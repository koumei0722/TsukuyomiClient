#include "game/Nbt.h"

#include <cstring>

namespace tsukuyomi::nbt {

const Value* Value::find(const std::string& key) const
{
    if (tag != Tag::Compound) {
        return nullptr;
    }
    const auto at = compound.find(key);
    return (at == compound.end() || !at->second) ? nullptr : at->second.get();
}

const Value* Value::find(const std::string& key, Tag want) const
{
    const Value* const got = find(key);
    return (got != nullptr && got->tag == want) ? got : nullptr;
}

namespace {

class Reader {
public:
    Reader(const char* bytes, std::size_t size) : m_bytes(bytes), m_size(size) {}

    bool ok() const { return m_why == nullptr; }
    const char* why() const { return m_why; }

    void fail(const char* reason)
    {
        if (m_why == nullptr) {
            m_why = reason;
        }
    }

    std::size_t left() const { return m_size - m_at; }

    template <typename T>
    T fixed()
    {
        T value{};
        if (!ok()) {
            return value;
        }
        if (left() < sizeof(T)) {
            fail("truncated");
            return value;
        }
        std::memcpy(&value, m_bytes + m_at, sizeof(T));
        m_at += sizeof(T);
        return value;
    }

    std::string text()
    {
        const auto length = static_cast<std::uint16_t>(fixed<std::uint16_t>());
        if (!ok()) {
            return {};
        }
        if (left() < length) {
            fail("truncated string");
            return {};
        }
        std::string out(m_bytes + m_at, length);
        m_at += length;
        return out;
    }

    bool canHold(std::int32_t count, std::size_t leastPerItem)
    {
        if (!ok()) {
            return false;
        }
        if (count < 0) {
            fail("negative count");
            return false;
        }
        if (leastPerItem != 0
            && static_cast<std::size_t>(count) > left() / leastPerItem) {
            fail("count larger than the file");
            return false;
        }
        return true;
    }

private:
    const char* m_bytes = nullptr;
    std::size_t m_size = 0;
    std::size_t m_at = 0;
    const char* m_why = nullptr;
};

constexpr int kMaxDepth = 64;

ValuePtr readPayload(Reader& reader, Tag tag, int depth);

std::size_t leastSizeOf(Tag tag)
{
    switch (tag) {
    case Tag::Byte: return 1;
    case Tag::Short: return 2;
    case Tag::Int: return 4;
    case Tag::Long: return 8;
    case Tag::Float: return 4;
    case Tag::Double: return 8;
    case Tag::String: return 2;
    case Tag::ByteArray: return 4;
    case Tag::IntArray: return 4;
    case Tag::LongArray: return 4;
    case Tag::List: return 5;
    case Tag::Compound: return 1;
    default: return 1;
    }
}

void readNumbers(Reader& reader, Value& out, std::size_t itemSize)
{
    const std::int32_t count = reader.fixed<std::int32_t>();
    if (!reader.canHold(count, itemSize)) {
        return;
    }
    out.numbers.reserve(static_cast<std::size_t>(count));
    for (std::int32_t i = 0; i < count && reader.ok(); ++i) {
        switch (itemSize) {
        case 1: out.numbers.push_back(reader.fixed<std::int8_t>()); break;
        case 4: out.numbers.push_back(reader.fixed<std::int32_t>()); break;
        default: out.numbers.push_back(reader.fixed<std::int64_t>()); break;
        }
    }
}

ValuePtr readPayload(Reader& reader, Tag tag, int depth)
{
    if (!reader.ok()) {
        return nullptr;
    }
    if (depth > kMaxDepth) {
        reader.fail("nested too deep");
        return nullptr;
    }

    auto value = std::make_shared<Value>();
    value->tag = tag;

    switch (tag) {
    case Tag::Byte: value->number = reader.fixed<std::int8_t>(); break;
    case Tag::Short: value->number = reader.fixed<std::int16_t>(); break;
    case Tag::Int: value->number = reader.fixed<std::int32_t>(); break;
    case Tag::Long: value->number = reader.fixed<std::int64_t>(); break;
    case Tag::Float: value->real = reader.fixed<float>(); break;
    case Tag::Double: value->real = reader.fixed<double>(); break;
    case Tag::String: value->text = reader.text(); break;
    case Tag::ByteArray: readNumbers(reader, *value, 1); break;
    case Tag::IntArray: readNumbers(reader, *value, 4); break;
    case Tag::LongArray: readNumbers(reader, *value, 8); break;

    case Tag::List: {
        const auto itemTag = static_cast<Tag>(reader.fixed<std::uint8_t>());
        const std::int32_t count = reader.fixed<std::int32_t>();
        if (!reader.canHold(count, leastSizeOf(itemTag))) {
            return nullptr;
        }
        if (count == 0) {
            break;
        }
        if (itemTag == Tag::End) {
            reader.fail("list of End with items");
            return nullptr;
        }
        value->listItem = itemTag;
        if (itemTag == Tag::Byte || itemTag == Tag::Short || itemTag == Tag::Int
            || itemTag == Tag::Long) {
            value->numbers.reserve(static_cast<std::size_t>(count));
            for (std::int32_t i = 0; i < count && reader.ok(); ++i) {
                switch (itemTag) {
                case Tag::Byte:
                    value->numbers.push_back(reader.fixed<std::int8_t>());
                    break;
                case Tag::Short:
                    value->numbers.push_back(reader.fixed<std::int16_t>());
                    break;
                case Tag::Int:
                    value->numbers.push_back(reader.fixed<std::int32_t>());
                    break;
                default:
                    value->numbers.push_back(reader.fixed<std::int64_t>());
                    break;
                }
            }
            break;
        }
        value->list.reserve(static_cast<std::size_t>(count));
        for (std::int32_t i = 0; i < count && reader.ok(); ++i) {
            ValuePtr item = readPayload(reader, itemTag, depth + 1);
            if (!item) {
                return nullptr;
            }
            value->list.push_back(std::move(item));
        }
        break;
    }

    case Tag::Compound:
        while (reader.ok()) {
            const auto itemTag = static_cast<Tag>(reader.fixed<std::uint8_t>());
            if (!reader.ok()) {
                return nullptr;
            }
            if (itemTag == Tag::End) {
                break;
            }
            std::string name = reader.text();
            ValuePtr item = readPayload(reader, itemTag, depth + 1);
            if (!item) {
                return nullptr;
            }
            value->compound[std::move(name)] = std::move(item);
        }
        break;

    default:
        reader.fail("unknown tag");
        return nullptr;
    }

    return reader.ok() ? value : nullptr;
}

}

Result read(const char* bytes, std::size_t size)
{
    Result result;
    if (bytes == nullptr || size == 0) {
        result.why = "empty";
        return result;
    }

    Reader reader(bytes, size);
    const auto tag = static_cast<Tag>(reader.fixed<std::uint8_t>());
    if (!reader.ok()) {
        result.why = reader.why();
        return result;
    }
    if (tag != Tag::Compound) {
        result.why = "root is not a compound (compressed file?)";
        return result;
    }

    result.rootName = reader.text();
    result.root = readPayload(reader, tag, 0);
    if (!result.root) {
        result.why = reader.why() != nullptr ? reader.why() : "malformed";
    }
    return result;
}

}
