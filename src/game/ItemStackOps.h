#pragma once

#include <cstddef>
#include <cstdint>

namespace tsukuyomi {

class ItemStackOps {
public:
    static ItemStackOps& instance();

    void onScansReady();

    bool available() const;

    bool swap(void* a, void* b);

    bool notifySlotChanged(void* container, int slot);

    bool setNetIdValue(void* stack, std::int32_t value);

    static constexpr std::size_t kStackBytes = 0x98;

    bool cloneTo(void* dst, const void* src);

    void destroyClone(void* dst);

    bool assignFrom(void* dst, const void* src);

    bool setCount(void* stack, std::uint8_t count);

    bool stash(const void* src);

    bool restore(void* dst);

    void discard();

    bool hasStash() const { return m_hasStash; }

private:
    ItemStackOps() = default;

    using CopyCtorFn = void(__fastcall*)(void* dst, const void* src);
    using AssignFn = void(__fastcall*)(void* dst, const void* src);
    using DtorFn = void(__fastcall*)(void* self);

    using NetIdAssignFn = void(__fastcall*)(std::int64_t tagPlusOne, void** dstField,
                                            const void* srcField);

    static constexpr std::size_t kStackSize = kStackBytes;
    static constexpr std::ptrdiff_t kCountOffset = 0x22;
    static constexpr std::ptrdiff_t kNetIdOffset = 0x80;
    static constexpr std::ptrdiff_t kNetTagOffset = 0x90;

    static constexpr std::ptrdiff_t kContentChangedVtableOffset = 0x110;

    static constexpr std::uint8_t kSupportedTag = 0;

    CopyCtorFn m_copyCtor = nullptr;
    AssignFn m_assign = nullptr;
    DtorFn m_dtor = nullptr;
    NetIdAssignFn m_netIdAssign = nullptr;

    std::byte m_stash[kStackSize]{};
    bool m_hasStash = false;

    bool m_scansReady = false;
    bool m_warnedMissing = false;
    bool m_warnedTag = false;
};

}
