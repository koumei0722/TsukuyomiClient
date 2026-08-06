#include "game/ItemStackOps.h"

#include <Windows.h>

#include <cstring>

#include "core/Logger.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"

namespace tsukuyomi {

namespace {

int faultFilter(EXCEPTION_POINTERS* info, const void** faultPc, const void** faultAddress)
{
    const unsigned long code = info->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_IN_PAGE_ERROR) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    *faultPc = info->ExceptionRecord->ExceptionAddress;
    *faultAddress = info->ExceptionRecord->NumberParameters >= 2
                        ? reinterpret_cast<const void*>(
                              info->ExceptionRecord->ExceptionInformation[1])
                        : nullptr;
    return EXCEPTION_EXECUTE_HANDLER;
}

struct SwapPayload {
    void (__fastcall* copyCtor)(void*, const void*);
    void (__fastcall* assign)(void*, const void*);
    void (__fastcall* dtor)(void*);
    void (__fastcall* netIdAssign)(std::int64_t, void**, const void*);

    std::byte* a;
    std::byte* b;

    std::ptrdiff_t netIdOffset;
    std::ptrdiff_t netTagOffset;
};

bool swapGuarded(SwapPayload& p, std::byte* temp, const void** faultPc,
                 const void** faultAddress)
{
    __try {

        p.copyCtor(temp, p.a);

        *reinterpret_cast<void**>(temp) = *reinterpret_cast<void* const*>(p.a);

        std::memset(temp + p.netIdOffset, 0, sizeof(std::int32_t));
        temp[p.netTagOffset] = std::byte{0};

        void* field = temp + p.netIdOffset;
        p.netIdAssign(static_cast<std::int64_t>(static_cast<std::uint8_t>(p.a[p.netTagOffset]))
                          + 1,
                      &field, p.a + p.netIdOffset);

        p.assign(p.a, p.b);
        field = p.a + p.netIdOffset;
        p.netIdAssign(static_cast<std::int64_t>(static_cast<std::uint8_t>(p.b[p.netTagOffset]))
                          + 1,
                      &field, p.b + p.netIdOffset);

        p.assign(p.b, temp);
        field = p.b + p.netIdOffset;
        p.netIdAssign(static_cast<std::int64_t>(static_cast<std::uint8_t>(temp[p.netTagOffset]))
                          + 1,
                      &field, temp + p.netIdOffset);

        *reinterpret_cast<void**>(temp) = *reinterpret_cast<void* const*>(p.a);
        p.dtor(temp);
        return true;
    } __except (faultFilter(GetExceptionInformation(), faultPc, faultAddress)) {
        return false;
    }
}

struct StashPayload {
    void (__fastcall* copyCtor)(void*, const void*);
    void (__fastcall* assign)(void*, const void*);
    void (__fastcall* dtor)(void*);
    void (__fastcall* netIdAssign)(std::int64_t, void**, const void*);

    std::byte* stash;
    std::byte* other;

    std::ptrdiff_t netIdOffset;
    std::ptrdiff_t netTagOffset;
};

bool stashGuarded(StashPayload& p, const void** faultPc, const void** faultAddress)
{
    __try {
        p.copyCtor(p.stash, p.other);
        *reinterpret_cast<void**>(p.stash) = *reinterpret_cast<void* const*>(p.other);

        std::memset(p.stash + p.netIdOffset, 0, sizeof(std::int32_t));
        p.stash[p.netTagOffset] = std::byte{0};

        void* field = p.stash + p.netIdOffset;
        p.netIdAssign(
            static_cast<std::int64_t>(static_cast<std::uint8_t>(p.other[p.netTagOffset])) + 1,
            &field, p.other + p.netIdOffset);
        return true;
    } __except (faultFilter(GetExceptionInformation(), faultPc, faultAddress)) {
        return false;
    }
}

bool assignGuarded(StashPayload& p, const void** faultPc, const void** faultAddress)
{
    __try {
        p.assign(p.other, p.stash);
        void* field = p.other + p.netIdOffset;
        p.netIdAssign(
            static_cast<std::int64_t>(static_cast<std::uint8_t>(p.stash[p.netTagOffset])) + 1,
            &field, p.stash + p.netIdOffset);
        return true;
    } __except (faultFilter(GetExceptionInformation(), faultPc, faultAddress)) {
        return false;
    }
}

bool restoreGuarded(StashPayload& p, const void** faultPc, const void** faultAddress)
{
    __try {
        p.assign(p.other, p.stash);
        void* field = p.other + p.netIdOffset;
        p.netIdAssign(
            static_cast<std::int64_t>(static_cast<std::uint8_t>(p.stash[p.netTagOffset])) + 1,
            &field, p.stash + p.netIdOffset);
        p.dtor(p.stash);
        return true;
    } __except (faultFilter(GetExceptionInformation(), faultPc, faultAddress)) {
        return false;
    }
}

bool discardGuarded(void (__fastcall* dtor)(void*), std::byte* stash, const void** faultPc,
                    const void** faultAddress)
{
    __try {
        dtor(stash);
        return true;
    } __except (faultFilter(GetExceptionInformation(), faultPc, faultAddress)) {
        return false;
    }
}

bool notifyGuarded(void* entry, void* container, int slot, const void** faultPc,
                   const void** faultAddress)
{
    __try {
        reinterpret_cast<void(__fastcall*)(void*, int)>(entry)(container, slot);
        return true;
    } __except (faultFilter(GetExceptionInformation(), faultPc, faultAddress)) {
        return false;
    }
}

struct ModuleRange {
    const std::byte* base = nullptr;
    std::size_t size = 0;

    bool contains(const void* address) const
    {
        const auto* const value = static_cast<const std::byte*>(address);
        return base != nullptr && value >= base && value < base + size;
    }
};

const ModuleRange& mainModule()
{
    static const ModuleRange range = [] {
        ModuleRange result;
        const auto* const base = reinterpret_cast<const std::byte*>(GetModuleHandleW(nullptr));
        if (base == nullptr) {
            return result;
        }
        const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
            return result;
        }
        const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) {
            return result;
        }
        result.base = base;
        result.size = nt->OptionalHeader.SizeOfImage;
        return result;
    }();
    return range;
}

}

ItemStackOps& ItemStackOps::instance()
{
    static ItemStackOps object;
    return object;
}

void ItemStackOps::onScansReady()
{
    if (m_scansReady) {
        return;
    }
    m_scansReady = true;

    const Scanner& scanner = Scanner::instance();
    m_copyCtor = scanner.addressAs<CopyCtorFn>(Target::ItemStackCopyCtor);
    m_assign = scanner.addressAs<AssignFn>(Target::ItemStackAssign);
    m_dtor = scanner.addressAs<DtorFn>(Target::ItemStackDtor);
    m_netIdAssign = scanner.addressAs<NetIdAssignFn>(Target::ItemStackNetIdAssign);

    if (!available()) {
        log().warn(L"ItemStackOps: swapping stacks across containers is unavailable "
                   L"(copy {}, assign {}, destroy {}, net id {})",
                   m_copyCtor != nullptr, m_assign != nullptr, m_dtor != nullptr,
                   m_netIdAssign != nullptr);
    }
}

bool ItemStackOps::available() const
{
    return m_copyCtor != nullptr && m_assign != nullptr && m_dtor != nullptr
           && m_netIdAssign != nullptr;
}

bool ItemStackOps::swap(void* a, void* b)
{
    if (a == nullptr || b == nullptr || a == b) {
        return false;
    }
    if (!available()) {
        if (!m_warnedMissing) {
            m_warnedMissing = true;
            log().warn(L"ItemStackOps: the stack swap helpers were not found, "
                       L"the client side cannot be kept in sync");
        }
        return false;
    }

    auto* const first = static_cast<std::byte*>(a);
    auto* const second = static_cast<std::byte*>(b);
    if (!memory::isReadable(first, kStackSize) || !memory::isReadable(second, kStackSize)) {
        return false;
    }

    void* const vtable = *reinterpret_cast<void* const*>(first);
    if (vtable == nullptr || vtable != *reinterpret_cast<void* const*>(second)
        || !mainModule().contains(vtable)) {
        return false;
    }

    const auto tagA = static_cast<std::uint8_t>(first[kNetTagOffset]);
    const auto tagB = static_cast<std::uint8_t>(second[kNetTagOffset]);
    if (tagA != kSupportedTag || tagB != kSupportedTag) {
        if (!m_warnedTag) {
            m_warnedTag = true;
            log().warn(L"ItemStackOps: net id tags {} and {} are not the plain kind, "
                       L"the swap is skipped",
                       tagA, tagB);
        }
        return false;
    }

    SwapPayload payload{};
    payload.copyCtor = m_copyCtor;
    payload.assign = m_assign;
    payload.dtor = m_dtor;
    payload.netIdAssign = m_netIdAssign;
    payload.a = first;
    payload.b = second;
    payload.netIdOffset = kNetIdOffset;
    payload.netTagOffset = kNetTagOffset;

    std::byte temp[kStackSize]{};

    const void* faultPc = nullptr;
    const void* faultAddress = nullptr;
    if (!swapGuarded(payload, temp, &faultPc, &faultAddress)) {
        const ModuleRange& module = mainModule();
        const auto rva = module.contains(faultPc)
                             ? static_cast<std::size_t>(
                                   static_cast<const std::byte*>(faultPc) - module.base)
                             : 0;
        log().error(L"ItemStackOps: swapping two stacks faulted "
                    L"(pc rva {:#x}, touched {:#x})",
                    rva, reinterpret_cast<std::uintptr_t>(faultAddress));
        return false;
    }
    return true;
}

bool ItemStackOps::notifySlotChanged(void* container, int slot)
{

    if (container == nullptr || slot < 0) {
        return false;
    }
    if (!memory::isReadable(container, sizeof(void*))) {
        return false;
    }

    auto* const vtable = *reinterpret_cast<std::byte* const*>(container);
    if (vtable == nullptr || !mainModule().contains(vtable)
        || !memory::isReadable(vtable, kContentChangedVtableOffset + sizeof(void*))) {
        return false;
    }

    void* const entry =
        *reinterpret_cast<void* const*>(vtable + kContentChangedVtableOffset);
    if (entry == nullptr || !mainModule().contains(entry)) {
        return false;
    }

    const void* faultPc = nullptr;
    const void* faultAddress = nullptr;
    if (!notifyGuarded(entry, container, slot, &faultPc, &faultAddress)) {
        const ModuleRange& module = mainModule();
        const auto rva = module.contains(faultPc)
                             ? static_cast<std::size_t>(
                                   static_cast<const std::byte*>(faultPc) - module.base)
                             : 0;
        log().error(L"ItemStackOps: telling the container that slot {} changed faulted "
                    L"(pc rva {:#x}, touched {:#x})",
                    slot, rva, reinterpret_cast<std::uintptr_t>(faultAddress));
        return false;
    }
    return true;
}

bool ItemStackOps::setNetIdValue(void* stack, std::int32_t value)
{
    if (stack == nullptr) {
        return false;
    }
    auto* const bytes = static_cast<std::byte*>(stack);
    if (!memory::isReadable(bytes, kStackSize)) {
        return false;
    }

    void* const vtable = *reinterpret_cast<void* const*>(bytes);
    if (vtable == nullptr || !mainModule().contains(vtable)) {
        return false;
    }

    if (static_cast<std::uint8_t>(bytes[kNetTagOffset]) != kSupportedTag) {
        return false;
    }

    std::memcpy(bytes + kNetIdOffset, &value, sizeof(value));
    return true;
}

bool ItemStackOps::cloneTo(void* dst, const void* src)
{
    if (!available() || dst == nullptr || src == nullptr) {
        return false;
    }

    auto* const target = static_cast<std::byte*>(dst);
    auto* const source = const_cast<std::byte*>(static_cast<const std::byte*>(src));
    if (!memory::isWritable(target, kStackSize) || !memory::isReadable(source, kStackSize)) {
        return false;
    }
    void* const vtable = *reinterpret_cast<void* const*>(source);
    if (vtable == nullptr || !mainModule().contains(vtable)) {
        return false;
    }
    if (static_cast<std::uint8_t>(source[kNetTagOffset]) != kSupportedTag) {
        return false;
    }

    StashPayload payload{};
    payload.copyCtor = m_copyCtor;
    payload.assign = m_assign;
    payload.dtor = m_dtor;
    payload.netIdAssign = m_netIdAssign;
    payload.stash = target;
    payload.other = source;
    payload.netIdOffset = kNetIdOffset;
    payload.netTagOffset = kNetTagOffset;

    const void* faultPc = nullptr;
    const void* faultAddress = nullptr;
    if (!stashGuarded(payload, &faultPc, &faultAddress)) {
        const ModuleRange& module = mainModule();
        const auto rva = module.contains(faultPc)
                             ? static_cast<std::size_t>(
                                   static_cast<const std::byte*>(faultPc) - module.base)
                             : 0;
        log().error(L"ItemStackOps: keeping a copy of a stack faulted "
                    L"(pc rva {:#x}, touched {:#x})",
                    rva, reinterpret_cast<std::uintptr_t>(faultAddress));
        return false;
    }
    return true;
}

void ItemStackOps::destroyClone(void* dst)
{
    if (dst == nullptr || m_dtor == nullptr) {
        return;
    }
    const void* faultPc = nullptr;
    const void* faultAddress = nullptr;
    if (!discardGuarded(m_dtor, static_cast<std::byte*>(dst), &faultPc, &faultAddress)) {
        const ModuleRange& module = mainModule();
        const auto rva = module.contains(faultPc)
                             ? static_cast<std::size_t>(
                                   static_cast<const std::byte*>(faultPc) - module.base)
                             : 0;
        log().error(L"ItemStackOps: dropping a copied stack faulted "
                    L"(pc rva {:#x}, touched {:#x})",
                    rva, reinterpret_cast<std::uintptr_t>(faultAddress));
    }
}

bool ItemStackOps::assignFrom(void* dst, const void* src)
{
    if (!available() || dst == nullptr || src == nullptr || dst == src) {
        return false;
    }

    auto* const target = static_cast<std::byte*>(dst);
    auto* const source = const_cast<std::byte*>(static_cast<const std::byte*>(src));
    if (!memory::isWritable(target, kStackSize) || !memory::isReadable(source, kStackSize)) {
        return false;
    }

    void* const vtable = *reinterpret_cast<void* const*>(target);
    if (vtable == nullptr || vtable != *reinterpret_cast<void* const*>(source)
        || !mainModule().contains(vtable)) {
        return false;
    }
    if (static_cast<std::uint8_t>(source[kNetTagOffset]) != kSupportedTag
        || static_cast<std::uint8_t>(target[kNetTagOffset]) != kSupportedTag) {
        return false;
    }

    StashPayload payload{};
    payload.copyCtor = m_copyCtor;
    payload.assign = m_assign;
    payload.dtor = m_dtor;
    payload.netIdAssign = m_netIdAssign;
    payload.stash = source;
    payload.other = target;
    payload.netIdOffset = kNetIdOffset;
    payload.netTagOffset = kNetTagOffset;

    const void* faultPc = nullptr;
    const void* faultAddress = nullptr;
    if (!assignGuarded(payload, &faultPc, &faultAddress)) {
        const ModuleRange& module = mainModule();
        const auto rva = module.contains(faultPc)
                             ? static_cast<std::size_t>(
                                   static_cast<const std::byte*>(faultPc) - module.base)
                             : 0;
        log().error(L"ItemStackOps: copying a stack over another faulted "
                    L"(pc rva {:#x}, touched {:#x})",
                    rva, reinterpret_cast<std::uintptr_t>(faultAddress));
        return false;
    }
    return true;
}

bool ItemStackOps::setCount(void* stack, std::uint8_t count)
{
    if (stack == nullptr) {
        return false;
    }
    auto* const target = static_cast<std::byte*>(stack);
    if (!memory::isWritable(target, kStackSize)) {
        return false;
    }
    void* const vtable = *reinterpret_cast<void* const*>(target);
    if (vtable == nullptr || !mainModule().contains(vtable)) {
        return false;
    }
    target[kCountOffset] = static_cast<std::byte>(count);
    return true;
}

bool ItemStackOps::stash(const void* src)
{

    discard();
    if (!cloneTo(m_stash, src)) {
        return false;
    }
    m_hasStash = true;
    return true;
}

bool ItemStackOps::restore(void* dst)
{
    if (!m_hasStash || !available() || dst == nullptr) {
        return false;
    }

    auto* const target = static_cast<std::byte*>(dst);
    if (!memory::isReadable(target, kStackSize)) {
        return false;
    }

    void* const vtable = *reinterpret_cast<void* const*>(target);
    if (vtable == nullptr || vtable != *reinterpret_cast<void* const*>(m_stash)) {
        return false;
    }

    StashPayload payload{};
    payload.copyCtor = m_copyCtor;
    payload.assign = m_assign;
    payload.dtor = m_dtor;
    payload.netIdAssign = m_netIdAssign;
    payload.stash = m_stash;
    payload.other = target;
    payload.netIdOffset = kNetIdOffset;
    payload.netTagOffset = kNetTagOffset;

    const void* faultPc = nullptr;
    const void* faultAddress = nullptr;
    const bool ok = restoreGuarded(payload, &faultPc, &faultAddress);

    m_hasStash = false;
    if (!ok) {
        const ModuleRange& module = mainModule();
        const auto rva = module.contains(faultPc)
                             ? static_cast<std::size_t>(
                                   static_cast<const std::byte*>(faultPc) - module.base)
                             : 0;
        log().error(L"ItemStackOps: putting a kept stack back faulted "
                    L"(pc rva {:#x}, touched {:#x})",
                    rva, reinterpret_cast<std::uintptr_t>(faultAddress));
        return false;
    }
    return true;
}

void ItemStackOps::discard()
{
    if (!m_hasStash) {
        return;
    }
    m_hasStash = false;
    destroyClone(m_stash);
}

}
