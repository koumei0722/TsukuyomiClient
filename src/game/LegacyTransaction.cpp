#include "game/LegacyTransaction.h"

#include <Windows.h>

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

struct RawSource {
    std::int32_t type;
    std::uint8_t containerId;
    std::uint8_t pad[3];
    std::int32_t flags;
};

struct EmptyTransaction {
    static constexpr std::size_t kBucketCount = 8;
    static constexpr std::size_t kBucketSlots = kBucketCount * 2;
    static constexpr std::size_t kBucketMask = kBucketCount - 1;

    float maxLoadFactor;
    std::uint32_t pad;
    void* listHead;
    std::size_t listSize;
    void* bucketsFirst;
    void* bucketsLast;
    void* bucketsEnd;
    std::size_t mask;
    std::size_t bucketCount;
    void* balanceFirst;
    void* balanceLast;
    void* balanceEnd;

    void* sentinel[2];
    void* buckets[kBucketSlots];
};

static_assert(offsetof(EmptyTransaction, listHead) == 0x08);
static_assert(offsetof(EmptyTransaction, bucketsFirst) == 0x18);
static_assert(offsetof(EmptyTransaction, mask) == 0x30);
static_assert(offsetof(EmptyTransaction, bucketCount) == 0x38);
static_assert(offsetof(EmptyTransaction, balanceFirst) == 0x40);

void fillEmptyTransaction(EmptyTransaction& out)
{
    out = EmptyTransaction{};
    out.maxLoadFactor = 1.0f;
    out.sentinel[0] = out.sentinel;
    out.sentinel[1] = out.sentinel;
    out.listHead = out.sentinel;
    out.bucketsFirst = out.buckets;
    out.bucketsLast = out.buckets + EmptyTransaction::kBucketSlots;
    out.bucketsEnd = out.buckets + EmptyTransaction::kBucketSlots;
    out.mask = EmptyTransaction::kBucketMask;
    out.bucketCount = EmptyTransaction::kBucketCount;
}

struct SendPayload {
    void** (__fastcall* makeTransaction)(void**, std::uint32_t, const void*);
    void(__fastcall* makeAction)(void*, const RawSource*, std::int32_t, const void*, const void*);
    void(__fastcall* destroyAction)(void*);
    void(__fastcall* addAction)(void*, const void*);
    void(__fastcall* send)(void*, void**);

    void* player;

    EmptyTransaction empty;

    RawSource sourceA;
    RawSource sourceB;
    std::int32_t slotA;
    std::int32_t slotB;
    const void* fromA;
    const void* toA;
    const void* fromB;
    const void* toB;

    std::uint32_t transactionType;
    std::ptrdiff_t innerOffset;
    std::byte* action;
};

bool sendGuarded(SendPayload& p, const void** faultPc, const void** faultAddress)
{
    __try {
        void* transaction = nullptr;
        p.makeTransaction(&transaction, p.transactionType, &p.empty);
        if (transaction == nullptr) {
            return false;
        }

        void* const inner = static_cast<std::byte*>(transaction) + p.innerOffset;

        p.makeAction(p.action, &p.sourceA, p.slotA, p.fromA, p.toA);
        p.addAction(inner, p.action);
        p.destroyAction(p.action);

        p.makeAction(p.action, &p.sourceB, p.slotB, p.fromB, p.toB);
        p.addAction(inner, p.action);
        p.destroyAction(p.action);

        p.send(p.player, &transaction);
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

bool readPointer(const void* address, std::ptrdiff_t offset, void*& out)
{
    const auto* const at = static_cast<const std::byte*>(address) + offset;
    if (!memory::isReadable(at, sizeof(void*))) {
        return false;
    }
    out = *reinterpret_cast<void* const*>(at);
    return true;
}

}

LegacyTransaction& LegacyTransaction::instance()
{
    static LegacyTransaction object;
    return object;
}

void LegacyTransaction::onScansReady()
{
    if (m_scansReady) {
        return;
    }
    m_scansReady = true;

    const Scanner& scanner = Scanner::instance();
    m_makeTransaction = scanner.addressAs<MakeTransactionFn>(Target::MakeComplexTransaction);
    m_makeAction = scanner.addressAs<MakeActionFn>(Target::MakeInventoryAction);
    m_destroyAction = scanner.addressAs<DestroyActionFn>(Target::DestroyInventoryAction);
    m_addAction = scanner.addressAs<AddActionFn>(Target::AddInventoryAction);
    m_send = scanner.addressAs<SendFn>(Target::SendComplexTransaction);

    if (!available()) {
        log().warn(L"LegacyTransaction: the legacy inventory path is unavailable "
                   L"(create {}, build {}, release {}, append {}, send {})",
                   m_makeTransaction != nullptr, m_makeAction != nullptr,
                   m_destroyAction != nullptr, m_addAction != nullptr, m_send != nullptr);
    }
}

bool LegacyTransaction::available() const
{
    return m_makeTransaction != nullptr && m_makeAction != nullptr && m_destroyAction != nullptr
           && m_addAction != nullptr && m_send != nullptr;
}

bool LegacyTransaction::playerLooksUsable(const void* player) const
{
    if (player == nullptr || mainModule().contains(player)) {
        return false;
    }

    void* playerVtable = nullptr;
    if (!readPointer(player, 0, playerVtable) || !mainModule().contains(playerVtable)) {
        return false;
    }
    void* sendFn = nullptr;
    if (!readPointer(playerVtable, kSendVtableOffset, sendFn)
        || !mainModule().contains(sendFn)) {
        return false;
    }

    void* netManager = nullptr;
    if (!readPointer(player, kNetManagerOffset, netManager) || netManager == nullptr) {
        return false;
    }
    void* netManagerVtable = nullptr;
    if (!readPointer(netManager, 0, netManagerVtable)
        || !mainModule().contains(netManagerVtable)) {
        return false;
    }
    void* beginFn = nullptr;
    if (!readPointer(netManagerVtable, kBeginRequestVtableOffset, beginFn)
        || !mainModule().contains(beginFn)) {
        return false;
    }
    return true;
}

bool LegacyTransaction::swap(void* player, const SlotRef& a, const SlotRef& b)
{
    if (a.stack == nullptr || b.stack == nullptr) {
        return false;
    }
    return apply(player, Change{a.containerId, a.slot, a.stack, b.stack},
                 Change{b.containerId, b.slot, b.stack, a.stack});
}

bool LegacyTransaction::apply(void* player, const Change& a, const Change& b)
{
    if (player == nullptr || a.from == nullptr || a.to == nullptr || b.from == nullptr
        || b.to == nullptr) {
        return false;
    }
    if (a.slot < 0 || b.slot < 0) {
        return false;
    }
    if (a.containerId == b.containerId && a.slot == b.slot) {
        return false;
    }
    if ((a.containerId == kContainerOffhand && a.slot != kOffhandSlot)
        || (b.containerId == kContainerOffhand && b.slot != kOffhandSlot)) {
        return false;
    }
    if (!available()) {
        if (!m_warnedMissing) {
            m_warnedMissing = true;
            if (m_faulted) {
                log().warn(L"LegacyTransaction: the legacy inventory path was closed by an "
                           L"earlier fault, the item will be moved through the request path "
                           L"instead (the server refuses swords there)");
            } else {
                log().warn(L"LegacyTransaction: the helpers were not found, "
                           L"the legacy inventory path is skipped");
            }
        }
        return false;
    }

    if (!memory::isReadable(a.from, kItemStackSize) || !memory::isReadable(a.to, kItemStackSize)
        || !memory::isReadable(b.from, kItemStackSize)
        || !memory::isReadable(b.to, kItemStackSize)) {
        return false;
    }
    if (!playerLooksUsable(player)) {
        if (!m_warnedPlayer) {
            m_warnedPlayer = true;
            log().warn(L"LegacyTransaction: the player object does not look like the local one, "
                       L"the legacy path is skipped");
        }
        return false;
    }

    std::byte action[kActionSize];

    SendPayload payload{};
    payload.makeTransaction = reinterpret_cast<decltype(payload.makeTransaction)>(m_makeTransaction);
    payload.makeAction = reinterpret_cast<decltype(payload.makeAction)>(m_makeAction);
    payload.destroyAction = reinterpret_cast<decltype(payload.destroyAction)>(m_destroyAction);
    payload.addAction = reinterpret_cast<decltype(payload.addAction)>(m_addAction);
    payload.send = reinterpret_cast<decltype(payload.send)>(m_send);

    payload.player = player;
    payload.slotA = a.slot;
    payload.slotB = b.slot;
    payload.fromA = a.from;
    payload.toA = a.to;
    payload.fromB = b.from;
    payload.toB = b.to;

    payload.sourceA.type = kSourceContainer;
    payload.sourceA.containerId = a.containerId;
    payload.sourceA.flags = 0;

    payload.sourceB.type = kSourceContainer;
    payload.sourceB.containerId = b.containerId;
    payload.sourceB.flags = 0;

    payload.transactionType = kNormalTransaction;
    payload.innerOffset = kInnerTransactionOffset;
    payload.action = action;

    fillEmptyTransaction(payload.empty);

    const void* faultPc = nullptr;
    const void* faultAddress = nullptr;
    const bool sent = sendGuarded(payload, &faultPc, &faultAddress);
    if (faultPc != nullptr) {
        log().error(L"LegacyTransaction: faulted at {} while touching {}, "
                    L"the legacy path is disabled",
                    faultPc, faultAddress);
        m_makeTransaction = nullptr;
        m_faulted = true;
        return false;
    }
    return sent;
}

}
