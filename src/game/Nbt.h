#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace tsukuyomi::nbt {

enum class Tag : std::uint8_t {
    End = 0,
    Byte = 1,
    Short = 2,
    Int = 3,
    Long = 4,
    Float = 5,
    Double = 6,
    ByteArray = 7,
    String = 8,
    List = 9,
    Compound = 10,
    IntArray = 11,
    LongArray = 12,
};

struct Value;
using ValuePtr = std::shared_ptr<Value>;

struct Value {
    Tag tag = Tag::End;

    Tag listItem = Tag::End;

    std::int64_t number = 0;
    double real = 0.0;
    std::string text;
    std::vector<std::int64_t> numbers;
    std::vector<ValuePtr> list;
    std::map<std::string, ValuePtr> compound;

    const Value* find(const std::string& key) const;

    const Value* find(const std::string& key, Tag want) const;
};

struct Result {
    ValuePtr root;
    std::string rootName;
    const char* why = nullptr;
};

Result read(const char* bytes, std::size_t size);

}
