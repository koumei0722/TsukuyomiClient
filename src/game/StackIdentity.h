#pragma once
#include <cstdint>

namespace tsukuyomi::inventoryresync {
struct StackIdentity {
    std::uintptr_t item=0;
    std::uint16_t aux=0;
    std::uint64_t nbt=0;
    bool known=false;
};
inline bool sameItem(const StackIdentity& a, const StackIdentity& b)
{
    return a.known && b.known && a.item!=0 && a.item==b.item && a.aux==b.aux && a.nbt==b.nbt;
}
}
