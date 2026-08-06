#pragma once

#include "memory/Signatures.h"

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

namespace tsukuyomi {

struct ScanHit {
    std::byte* address = nullptr;

    size_t count = 0;
};

ScanHit scanRange(std::span<std::byte> range, std::string_view signature);

ScanHit scanMainModule(std::string_view signature, std::string_view section = ".text");

class Scanner {
public:
    static Scanner& instance();

    size_t scanAll();

    std::byte* address(Target target) const;
    bool found(Target target) const;

    template <class T>
    T addressAs(Target target) const
    {
        return reinterpret_cast<T>(address(target));
    }

private:
    Scanner() = default;

    std::array<std::byte*, static_cast<size_t>(Target::Count)> m_addresses{};
};

}
