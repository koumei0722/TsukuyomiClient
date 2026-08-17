#include "game/InventoryActionBridge.h"

#include "core/Logger.h"
#include "game/InventoryScreen.h"
#include "game/ItemStackOps.h"
#include "game/ItemStackRequest.h"
#include "game/LegacyTransaction.h"
#include "memory/Memory.h"
#include "modules/HandRestock.h"
#include "modules/OffhandSwap.h"

#include <Windows.h>

#include <cstring>
#include <format>
#include <string>

namespace tsukuyomi {

namespace {

const wchar_t* containerName(std::uint64_t container)
{
    switch (container) {
    case 7:
        return L"chest";
    case 12:
        return L"whole inventory";
    case 28:
        return L"hotbar";
    case 29:
        return L"inventory";
    case 34:
        return L"hand";
    case 59:
        return L"cursor";
    default:
        return nullptr;
    }
}

const wchar_t* kindName(std::uint8_t kind)
{
    switch (kind) {
    case 0:
        return L"Take";
    case 1:
        return L"Place";
    case 2:
        return L"Swap";
    default:
        return nullptr;
    }
}

bool bridgeActive()
{
    if (!OffhandSwap::instance().legacyBridgeEnabled()) {
        return false;
    }
    return OffhandSwap::instance().enabled() || HandRestock::instance().enabled();
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

int filterAccessViolation(unsigned long code)
{
    return (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR)
               ? EXCEPTION_EXECUTE_HANDLER
               : EXCEPTION_CONTINUE_SEARCH;
}

std::byte* callGetItemGuarded(void* fn, void* self, const void* query)
{
    using GetItemFn = std::byte*(__fastcall*)(void*, const void*);
    __try {
        return reinterpret_cast<GetItemFn>(fn)(self, query);
    } __except (filterAccessViolation(GetExceptionCode())) {
        return nullptr;
    }
}

}

InventoryActionBridge& InventoryActionBridge::instance()
{
    static InventoryActionBridge bridge;
    return bridge;
}

void InventoryActionBridge::onSetSelectedSlot(void* holder)
{
    if (holder == nullptr) {
        return;
    }
    void* const previous = m_holder.exchange(holder, std::memory_order_acq_rel);
    if (previous != nullptr && previous != holder) {
        m_holderAlt.store(previous, std::memory_order_release);
    }
}

bool InventoryActionBridge::readSlot(const void* address, SlotView& out)
{
    if (!memory::isReadable(address, sizeof(std::uint64_t) * 5)) {
        return false;
    }
    const auto* const base = static_cast<const std::byte*>(address);
    std::memcpy(&out.container, base + kSlotContainerOffset, sizeof(out.container));
    std::memcpy(&out.slot, base + kSlotIndexOffset, sizeof(out.slot));
    std::memcpy(&out.netValue, base + kSlotNetValueOffset, sizeof(out.netValue));
    std::memcpy(&out.netAlt, base + kSlotNetAltOffset, sizeof(out.netAlt));
    std::memcpy(&out.netTag, base + kSlotNetTagOffset, sizeof(out.netTag));
    return true;
}

bool InventoryActionBridge::readRequest(void* const* clientHolder, std::int32_t& requestId,
                                        std::uint64_t& index)
{
    if (clientHolder == nullptr || !memory::isReadable(clientHolder, sizeof(void*))) {
        return false;
    }

    const auto* const client = static_cast<const std::byte*>(*clientHolder);
    if (client == nullptr || !memory::isReadable(client + kPendingOffset, sizeof(void*))) {
        return false;
    }

    const std::byte* request = nullptr;
    std::memcpy(&request, client + kPendingOffset, sizeof(request));
    if (request == nullptr || !memory::isReadable(request, kRequestHeadSize)) {
        return false;
    }

    std::memcpy(&requestId, request + kRequestIdOffset, sizeof(requestId));

    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
    std::memcpy(&begin, request + kRequestActionsBeginOffset, sizeof(begin));
    std::memcpy(&end, request + kRequestActionsEndOffset, sizeof(end));

    index = end >= begin ? (end - begin) / sizeof(void*) : 0;
    return true;
}

bool InventoryActionBridge::looksLikeStacks(const std::byte* slots, int count)
{
    if (slots == nullptr || count <= 0 || count > kMaxContainerSlots
        || !memory::isReadable(slots, kStackSize * static_cast<std::size_t>(count))) {
        return false;
    }
    void* const vtable = *reinterpret_cast<void* const*>(slots);
    if (vtable == nullptr || !mainModule().contains(vtable)) {
        return false;
    }
    for (int i = 1; i < count; ++i) {
        if (*reinterpret_cast<void* const*>(slots + kStackSize * i) != vtable) {
            return false;
        }
    }
    return true;
}

bool InventoryActionBridge::looksLikeInventory(const std::byte* slots)
{
    return looksLikeStacks(slots, kSlotCount);
}

bool InventoryActionBridge::containerFrom(void* model, const void* ownNotify,
                                          const void* ownContainer, void*& container)
{
    container = nullptr;
    if (model == nullptr || !memory::isReadable(model, kModelContainerOffset + sizeof(void*))) {
        return false;
    }
    void* found = nullptr;
    if (!readPointer(model, kModelContainerOffset, found) || found == nullptr
        || found == ownContainer || !memory::isReadable(found, sizeof(void*))) {
        return false;
    }
    void* const vtable = *reinterpret_cast<void* const*>(found);
    if (vtable == nullptr || !mainModule().contains(vtable)) {
        return false;
    }
    void* notify = nullptr;
    if (!readPointer(vtable, kContentChangedVtableOffset, notify) || notify != ownNotify) {
        return false;
    }
    container = found;
    return true;
}

bool InventoryActionBridge::looksLikeHeapPointer(const void* value)
{
    return value != nullptr && (reinterpret_cast<std::uintptr_t>(value) % sizeof(void*)) == 0
           && !mainModule().contains(value);
}

std::byte* InventoryActionBridge::findCollection(const void* items, const char* name,
                                                 std::size_t nameLength)
{
    if (items == nullptr) {
        return nullptr;
    }

    const int firstByte = static_cast<int>(reinterpret_cast<std::uintptr_t>(items) & 0xFFu);

    MEMORY_BASIC_INFORMATION info{};
    auto* at = reinterpret_cast<std::byte*>(kScanFrom);
    auto* const limit = reinterpret_cast<std::byte*>(kScanTo);
    while (at < limit && VirtualQuery(at, &info, sizeof(info)) == sizeof(info)) {
        auto* const base = static_cast<std::byte*>(info.BaseAddress);
        const std::size_t size = info.RegionSize;
        if (size == 0) {
            break;
        }

        const DWORD kind = info.Protect & 0xFF;
        const bool usable = info.State == MEM_COMMIT
                            && (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0
                            && (kind == PAGE_READWRITE || kind == PAGE_WRITECOPY
                                || kind == PAGE_EXECUTE_READWRITE);
        if (usable) {

            std::byte* p = base;
            std::byte* const regionEnd = base + size;
            while (static_cast<std::size_t>(regionEnd - p) >= sizeof(void*)) {
                const std::size_t span =
                    static_cast<std::size_t>(regionEnd - p) - sizeof(void*) + 1;
                void* const hit = std::memchr(p, firstByte, span);
                if (hit == nullptr) {
                    break;
                }
                auto* const site = static_cast<std::byte*>(hit);
                if ((reinterpret_cast<std::uintptr_t>(site) % sizeof(void*)) == 0
                    && *reinterpret_cast<void* const*>(site) == items) {

                    std::byte* const candidate = site - kCollectionItemsOffset;
                    if (candidate >= base
                        && memory::isReadable(candidate, kCollectionItemsOffset + sizeof(void*))
                        && std::memcmp(candidate + kCollectionNameOffset, name, nameLength) == 0) {
                        return candidate;
                    }
                }
                p = site + 1;
            }
        }
        at = base + size;
    }
    return nullptr;
}

std::byte* InventoryActionBridge::findSlots(void* container, int count, const std::byte* exclude)
{
    if (container == nullptr || count <= 0) {
        return nullptr;
    }
    for (std::ptrdiff_t at = 0;
         at + 2 * static_cast<std::ptrdiff_t>(sizeof(void*)) <= kContainerScanBytes;
         at += static_cast<std::ptrdiff_t>(sizeof(void*))) {
        void* begin = nullptr;
        void* end = nullptr;
        if (!readPointer(container, at, begin) || begin == nullptr
            || !readPointer(container, at + static_cast<std::ptrdiff_t>(sizeof(void*)), end)
            || end == nullptr) {
            continue;
        }
        auto* const head = static_cast<std::byte*>(begin);
        auto* const tail = static_cast<std::byte*>(end);

        if (head == exclude || tail - head != static_cast<std::ptrdiff_t>(kStackSize) * count) {
            continue;
        }
        if (!looksLikeStacks(head, count)) {
            continue;
        }
        return head;
    }
    return nullptr;
}

bool InventoryActionBridge::resolveOpenContainer(OpenContainer& out) const
{

    const InventoryScreen& screen = InventoryScreen::instance();
    if (screen.ownScreenOpen()) {
        return false;
    }
    const void* const controller = screen.controller();
    if (controller == nullptr) {
        return false;
    }

    void* resolver = nullptr;
    if (!readPointer(controller, kNameResolverOffset, resolver) || resolver == nullptr
        || !memory::isReadable(resolver, sizeof(void*))) {
        return false;
    }
    void* const vtable = *reinterpret_cast<void* const*>(resolver);
    if (vtable == nullptr || !mainModule().contains(vtable)) {
        return false;
    }
    void* fn = nullptr;
    if (!readPointer(vtable, kGetItemVtableOffset, fn) || fn == nullptr
        || !mainModule().contains(fn)) {
        return false;
    }

    const bool trace = m_traceLeft.fetch_sub(1, std::memory_order_relaxed) > 0;
    if (trace) {
    }

    static constexpr char kContainerName[] = "container_items";
    static constexpr std::size_t kNameLength = sizeof(kContainerName) - 1;
    static_assert(kNameLength <= kShortStringCapacity, "名前が短い文字列の枠に収まらない");

    alignas(void*) std::byte query[kQuerySize]{};
    std::memcpy(query, kContainerName, kNameLength);
    const std::uint64_t length = kNameLength;
    const std::uint64_t capacity = kShortStringCapacity;
    std::memcpy(query + kQuerySizeOffset, &length, sizeof(length));
    std::memcpy(query + kQueryCapacityOffset, &capacity, sizeof(capacity));
    const std::int32_t index = 0;
    std::memcpy(query + kQueryIndexOffset, &index, sizeof(index));

    std::byte* const first = callGetItemGuarded(fn, resolver, query);
    if (trace) {
    }
    if (first == nullptr || !memory::isReadable(first, kStackSize)) {
        return false;
    }

    void* screenObject = nullptr;
    const bool gotScreen =
        readPointer(resolver, kResolverScreenOffset, screenObject) && screenObject != nullptr;
    if (trace) {
    }
    if (!gotScreen) {
        return false;
    }

    Places places;
    const bool gotPlaces = resolve(places) && places.container != nullptr
                           && memory::isReadable(places.container, sizeof(void*));
    if (trace) {
    }
    if (!gotPlaces) {
        return false;
    }
    void* ownNotify = nullptr;
    if (!readPointer(*reinterpret_cast<void* const*>(places.container),
                     kContentChangedVtableOffset, ownNotify)
        || ownNotify == nullptr) {
        return false;
    }

    void* container = nullptr;
    void* model = m_openModel.load(std::memory_order_relaxed);
    if (containerFrom(model, ownNotify, places.container, container)
        && container == m_cachedContainer.load(std::memory_order_relaxed)) {
        auto* const cached = static_cast<std::byte*>(m_cachedSlots.load(std::memory_order_relaxed));
        const int cachedCount = m_cachedCount.load(std::memory_order_relaxed);
        if (cached != nullptr && looksLikeStacks(cached, cachedCount)) {
            out.container = container;
            out.slots = cached;
            out.count = cachedCount;
            return true;
        }
    }

    int copyCount = 1;
    while (copyCount < kMaxContainerSlots) {
        const std::int32_t probe = copyCount;
        std::memcpy(query + kQueryIndexOffset, &probe, sizeof(probe));
        if (callGetItemGuarded(fn, resolver, query)
            != first + kStackSize * static_cast<std::size_t>(copyCount)) {
            break;
        }
        ++copyCount;
    }

    if (trace) {
    }

    const unsigned long long now = GetTickCount64();
    if (now < m_nextScanMs.load(std::memory_order_relaxed)) {
        return false;
    }

    {
        const auto* const screenBase = static_cast<const std::byte*>(screenObject);
        int budget = kResolveBudget;
        const auto take = [&](void* model) {
            void* box = nullptr;
            if (!containerFrom(model, ownNotify, places.container, box)) {
                return false;
            }
            std::byte* const slots = findSlots(box, copyCount, first);
            if (slots == nullptr) {
                return false;
            }
            if (trace) {
            }
            out.container = box;
            out.slots = slots;
            out.count = copyCount;
            m_openModel.store(model, std::memory_order_relaxed);
            m_cachedContainer.store(box, std::memory_order_relaxed);
            m_cachedSlots.store(slots, std::memory_order_relaxed);
            m_cachedCount.store(copyCount, std::memory_order_relaxed);
            return true;
        };
        for (std::ptrdiff_t page = 0; page < kScreenScanBytes && budget > 0;
             page += kScreenPageBytes) {
            if (!memory::isReadable(screenBase + page, kScreenPageBytes)) {
                break;
            }
            for (std::ptrdiff_t at = 0; at < kScreenPageBytes && budget > 0;
                 at += static_cast<std::ptrdiff_t>(sizeof(void*))) {
                void* const outer = *reinterpret_cast<void* const*>(screenBase + page + at);
                if (!looksLikeHeapPointer(outer)) {
                    continue;
                }
                --budget;
                if (take(outer)) {
                    return true;
                }
                if (!memory::isReadable(outer, kInnerScanBytes)) {
                    continue;
                }
                const auto* const innerBase = static_cast<const std::byte*>(outer);
                for (std::ptrdiff_t k = 0; k < kInnerScanBytes && budget > 0;
                     k += static_cast<std::ptrdiff_t>(sizeof(void*))) {
                    void* const inner = *reinterpret_cast<void* const*>(innerBase + k);
                    if (!looksLikeHeapPointer(inner)) {
                        continue;
                    }
                    --budget;
                    if (take(inner)) {
                        return true;
                    }
                }
            }
        }
    }

    std::byte* const collection = findCollection(first, kContainerName, kNameLength);
    if (collection == nullptr) {
        m_nextScanMs.store(now + kScanRetryMs, std::memory_order_relaxed);
        if (trace) {
        }
        return false;
    }
    void* found = nullptr;
    void* candidate = nullptr;
    const bool gotModel = readPointer(collection, kCollectionModelOffset, candidate);
    const bool gotContainer =
        gotModel && containerFrom(candidate, ownNotify, places.container, found);
    std::byte* const slots = gotContainer ? findSlots(found, copyCount, first) : nullptr;
    if (trace) {
    }
    if (slots == nullptr) {
        m_nextScanMs.store(now + kScanRetryMs, std::memory_order_relaxed);
        return false;
    }
    if (trace) {
    }
    out.container = found;
    out.slots = slots;
    out.count = copyCount;
    m_openModel.store(candidate, std::memory_order_relaxed);
    m_cachedContainer.store(found, std::memory_order_relaxed);
    m_cachedSlots.store(slots, std::memory_order_relaxed);
    m_cachedCount.store(copyCount, std::memory_order_relaxed);
    return true;
    return false;
}

bool InventoryActionBridge::resolve(Places& out) const
{
    void* const holders[] = {m_holder.load(std::memory_order_acquire),
                             m_holderAlt.load(std::memory_order_acquire)};

    for (void* const holder : holders) {
        if (holder == nullptr) {
            continue;
        }
        Places places;
        if (!readPointer(holder, kContainerOffset, places.container)
            || places.container == nullptr) {
            continue;
        }
        void* slots = nullptr;
        if (!readPointer(places.container, kSlotsOffset, slots)) {
            continue;
        }
        places.slots = static_cast<std::byte*>(slots);
        if (!looksLikeInventory(places.slots)) {
            continue;
        }
        if (!readPointer(places.container, kPlayerOffset, places.player)
            || places.player == nullptr) {
            continue;
        }

        void* begin = nullptr;
        void* end = nullptr;
        if (!readPointer(places.player, kUiItemsOffset, begin)
            || !readPointer(places.player, kUiItemsOffset + sizeof(void*), end)) {
            continue;
        }
        auto* const first = static_cast<std::byte*>(begin);
        if (first == nullptr || end == nullptr || static_cast<std::byte*>(end) < first + kStackSize) {
            continue;
        }
        if (!memory::isReadable(first, kStackSize)) {
            continue;
        }
        if (*reinterpret_cast<void* const*>(first) != *reinterpret_cast<void* const*>(places.slots)) {
            continue;
        }
        places.cursor = first;

        out = places;
        return true;
    }
    return false;
}

bool InventoryActionBridge::translate(const Places& places, const OpenContainer& open, int openId,
                                      const SlotView& view, Spot& out)
{
    switch (view.container) {
    case kContainerWholeInventory:
    case kContainerHotbar:
    case kContainerInventory: {

        const int index = static_cast<int>(view.slot);
        if (index < 0 || index >= kSlotCount) {
            return false;
        }
        out.legacyContainer = LegacyTransaction::kContainerInventory;
        out.legacySlot = index;
        out.stack = places.slots + kStackSize * index;
        out.notifySlot = index;
        out.notifyContainer = places.container;
        return true;
    }
    case kContainerCursor:

        if (view.slot != 0) {
            return false;
        }
        out.legacyContainer = LegacyTransaction::kContainerPlayerUi;
        out.legacySlot = 0;
        out.stack = places.cursor;
        out.notifySlot = -1;
        out.notifyContainer = nullptr;
        return true;
    case kContainerOpened: {

        const int index = static_cast<int>(view.slot);
        if (open.container == nullptr || open.slots == nullptr || openId < 0 || openId > 0xFF
            || index < 0 || index >= open.count) {
            return false;
        }
        out.legacyContainer = static_cast<std::uint8_t>(openId);
        out.legacySlot = index;
        out.stack = open.slots + kStackSize * index;
        out.notifySlot = index;
        out.notifyContainer = open.container;
        return true;
    }
    default:

        return false;
    }
}

std::uint8_t InventoryActionBridge::countOf(const std::byte* stack)
{
    if (stack == nullptr || !memory::isReadable(stack, kStackSize)) {
        return 0;
    }
    return static_cast<std::uint8_t>(stack[kStackCountOffset]);
}

bool InventoryActionBridge::isEmptyStack(const std::byte* stack)
{
    if (stack == nullptr || !memory::isReadable(stack, kStackSize)) {
        return false;
    }
    const void* item = nullptr;
    std::memcpy(&item, stack + kStackItemOffset, sizeof(item));
    return item == nullptr || static_cast<std::uint8_t>(stack[kStackCountOffset]) == 0;
}

bool InventoryActionBridge::sameItem(const std::byte* a, const std::byte* b)
{
    if (!memory::isReadable(a, kStackSize) || !memory::isReadable(b, kStackSize)) {
        return false;
    }
    const void* itemA = nullptr;
    const void* itemB = nullptr;
    std::memcpy(&itemA, a + kStackItemOffset, sizeof(itemA));
    std::memcpy(&itemB, b + kStackItemOffset, sizeof(itemB));
    if (itemA == nullptr || itemA != itemB) {
        return false;
    }
    std::uint16_t auxA = 0;
    std::uint16_t auxB = 0;
    std::memcpy(&auxA, a + kStackAuxOffset, sizeof(auxA));
    std::memcpy(&auxB, b + kStackAuxOffset, sizeof(auxB));
    return auxA == auxB;
}

std::byte* InventoryActionBridge::findEmptyStack(const Places& places, const std::byte* skipA,
                                                 const std::byte* skipB)
{
    void* const vtable = *reinterpret_cast<void* const*>(places.slots);

    void* begin = nullptr;
    void* end = nullptr;
    if (readPointer(places.player, kUiItemsOffset, begin)
        && readPointer(places.player, kUiItemsOffset + sizeof(void*), end) && begin != nullptr
        && end != nullptr) {
        auto* const first = static_cast<std::byte*>(begin);
        auto* const last = static_cast<std::byte*>(end);
        for (std::byte* at = first; at + kStackSize <= last; at += kStackSize) {
            if (at == skipA || at == skipB || !memory::isReadable(at, kStackSize)) {
                continue;
            }
            if (*reinterpret_cast<void* const*>(at) == vtable && isEmptyStack(at)) {
                return at;
            }
        }
    }

    for (int i = 0; i < kSlotCount; ++i) {
        std::byte* const at = places.slots + kStackSize * i;
        if (at == skipA || at == skipB) {
            continue;
        }
        if (isEmptyStack(at)) {
            return at;
        }
    }
    return nullptr;
}

bool InventoryActionBridge::shouldBlockPacket(void* packet)
{

    if (!bridgeActive()) {
        return false;
    }
    if (packet == nullptr || m_blockLeft.load(std::memory_order_acquire) <= 0) {
        return false;
    }

    const unsigned long long until = m_blockUntilMs.load(std::memory_order_acquire);
    if (GetTickCount64() > until) {
        const int left = m_blockLeft.exchange(0, std::memory_order_acq_rel);

        log().info(L"InventoryActionBridge: {} request packets never went out, "
                   L"letting the next ones through",
                   left);
        return false;
    }

    const int id = ItemStackRequest::packetId(packet);
    if (id < 0) {
        return false;
    }

    if constexpr (kRequestPacketId < 0) {
        const int seen = m_packetProbes.fetch_add(1, std::memory_order_relaxed) + 1;
        if (seen <= kPacketProbeLimit) {
            log().info(L"InventoryActionBridge: a packet of type {} went out while a move "
                       L"was being replaced",
                       id);
        }
        return false;
    }

    if (id != kRequestPacketId) {
        return false;
    }
    m_blockLeft.fetch_sub(1, std::memory_order_acq_rel);
    return true;
}

void InventoryActionBridge::clearQueue()
{
    ItemStackOps& ops = ItemStackOps::instance();
    for (int i = 0; i < m_queued; ++i) {
        ops.destroyClone(m_queue[i].beforeA);
        ops.destroyClone(m_queue[i].beforeB);
        ops.destroyClone(m_queue[i].afterA);
        ops.destroyClone(m_queue[i].afterB);
    }
    m_queued = 0;
}

bool InventoryActionBridge::allowLog()
{
    const unsigned long long now = GetTickCount64();
    const unsigned long long start = m_windowStartMs.load(std::memory_order_relaxed);
    if (start == 0 || now < start || now - start >= kWindowMs) {
        const int dropped = m_dropped.exchange(0, std::memory_order_relaxed);
        if (dropped > 0) {
            log().warn(L"InventoryActionBridge: dropped {} action lines in the last second",
                       dropped);
        }
        m_windowStartMs.store(now, std::memory_order_relaxed);
        m_inWindow.store(0, std::memory_order_relaxed);
    }

    const int seen = m_inWindow.fetch_add(1, std::memory_order_relaxed) + 1;
    if (seen <= kMaxPerWindow) {
        return true;
    }

    if (seen == kMaxPerWindow + 1) {
        log().warn(L"InventoryActionBridge: more than {} actions in one second, "
                   L"dropping the rest of this second",
                   kMaxPerWindow);
    }
    m_dropped.fetch_add(1, std::memory_order_relaxed);
    return false;
}

InventoryActionBridge::Plan InventoryActionBridge::choosePlan(const Places& places,
                                                             std::uint8_t kind,
                                                             std::uint8_t amount, const Spot& a,
                                                             const Spot& b, std::byte*& empty)
{

    if (kind == kKindSwap) {
        return Plan::Swap;
    }

    const std::uint8_t have = countOf(a.stack);
    if (amount == 0 || amount > have) {
        return Plan::None;
    }

    if (isEmptyStack(b.stack)) {

        return amount == have ? Plan::Swap : Plan::Split;
    }

    if (!sameItem(a.stack, b.stack)) {
        return Plan::None;
    }
    if (static_cast<int>(countOf(b.stack)) + static_cast<int>(amount) > kMaxStackCount) {
        return Plan::None;
    }
    if (amount == have) {

        empty = findEmptyStack(places, a.stack, b.stack);
        if (empty == nullptr) {
            return Plan::None;
        }
    }
    return Plan::Merge;
}

void InventoryActionBridge::onAddRequestAction(void* const* clientHolder, void* const* action)
{

    if (!bridgeActive()) {
        return;
    }

    if (ItemStackRequest::instance().isBuildingRequest()) {
        if (!m_notedOwn.exchange(true, std::memory_order_acq_rel)) {
            log().info(L"InventoryActionBridge: our own request actions are skipped here "
                       L"(ItemStackRequest logs those)");
        }
        return;
    }

    if (action == nullptr || !memory::isReadable(action, sizeof(void*))) {
        return;
    }

    const auto* const object = static_cast<const std::byte*>(*action);
    if (object == nullptr || !memory::isReadable(object, kActionHeadSize)) {
        return;
    }

    std::uint8_t kind = 0;
    std::memcpy(&kind, object + kKindOffset, sizeof(kind));

    std::uint8_t amount = 0;
    std::memcpy(&amount, object + kAmountOffset, sizeof(amount));

    const wchar_t* const name = kindName(kind);
    const bool readable = name != nullptr && memory::isReadable(object, kActionSize);

    SlotView src;
    SlotView dst;
    const bool haveSlots =
        readable && readSlot(object + kSrcOffset, src) && readSlot(object + kDstOffset, dst);

    bool replaced = false;
    const wchar_t* skipped = nullptr;

    OpenContainer open;
    const int openId = m_openContainerId.load(std::memory_order_acquire);
    const bool ownScreen = InventoryScreen::instance().ownScreenOpen();
    const bool haveOpen = haveSlots && !ownScreen && openId >= 0 && resolveOpenContainer(open);

    if (haveSlots) {
        if (!bridgeActive()) {
            skipped = L"offhand features are off";
        } else if (!ownScreen && !haveOpen) {

            skipped = L"another screen is open and its container could not be reached";
        } else if (m_queued >= kQueueSize) {

            skipped = L"too many moves in one frame";
            if (!m_warnedBusy.exchange(true, std::memory_order_acq_rel)) {
                log().warn(L"InventoryActionBridge: more than {} moves landed in one frame, "
                           L"the rest were left to the game",
                           kQueueSize);
            }
        } else {
            Places places;
            Spot a;
            Spot b;
            Plan plan = Plan::None;
            std::byte* empty = nullptr;
            if (!resolve(places)) {
                skipped = L"the inventory could not be reached";
                if (!m_warnedPlaces.exchange(true, std::memory_order_acq_rel)) {
                    log().warn(L"InventoryActionBridge: could not reach the inventory, "
                               L"switch your hotbar slot once (1-9)");
                }
            } else if (!translate(places, open, openId, src, a)
                       || !translate(places, open, openId, dst, b)) {

                skipped = L"this move touches something we do not handle";
            } else if (a.stack == b.stack) {
                skipped = L"both sides point at the same stack";
            } else if (plan = choosePlan(places, kind, amount, a, b, empty);
                       plan == Plan::None) {

                skipped = L"this move cannot be expressed on the legacy path";
            } else if (queueMove(places, plan, amount, a, b, empty)) {
                replaced = true;
            } else {
                skipped = L"the move could not be applied here";
            }
        }
    }

    if (!allowLog()) {
        return;
    }

    std::int32_t requestId = 0;
    std::uint64_t index = 0;
    const bool haveRequest = readRequest(clientHolder, requestId, index);
    const std::wstring where = haveRequest ? std::format(L"request {} #{}", requestId, index)
                                           : std::wstring(L"request ? #?");

    if (name == nullptr) {

        log().info(L"InventoryActionBridge: {} kind {} (layout not confirmed, left to the game)",
                   where, kind);
        return;
    }
    if (!haveSlots) {
        log().warn(L"InventoryActionBridge: {} {} has slots that could not be read", where, name);
        return;
    }

    const std::wstring amountText =
        kind != kKindSwap ? std::format(L" amount {}", amount) : std::wstring();

    const auto describe = [](const SlotView& view) {
        std::wstring text = std::format(L"{}:{}", view.container, view.slot);
        if (const wchar_t* const container = containerName(view.container); container != nullptr) {
            text += std::format(L" ({})", container);
        }
        if (view.netTag == 0) {
            text += std::format(L" net {}", view.netValue);
        } else if (view.netTag == 1 || view.netTag == 2) {
            text += std::format(L" net {} (tag {})", view.netAlt, view.netTag);
        } else {
            text += L" net none";
        }
        return text;
    };

    if (replaced) {
        log().info(L"InventoryActionBridge: {} {}{} src {} -> dst {} (going out as legacy)", where,
                   name, amountText, describe(src), describe(dst));
    } else {
        log().info(L"InventoryActionBridge: {} {}{} src {} -> dst {} (left to the game: {})", where,
                   name, amountText, describe(src), describe(dst),
                   skipped != nullptr ? skipped : L"unchanged");
    }
}

bool InventoryActionBridge::applyPlan(Plan plan, std::uint8_t amount, const Spot& a,
                                      const Spot& b, std::byte* empty)
{
    ItemStackOps& ops = ItemStackOps::instance();
    std::byte* const src = a.stack;
    std::byte* const dst = b.stack;
    const std::uint8_t have = countOf(src);

    switch (plan) {
    case Plan::Swap:
        return ops.swap(src, dst);

    case Plan::Split:

        if (amount >= have) {
            return false;
        }
        if (!ops.assignFrom(dst, src)) {
            return false;
        }
        return ops.setCount(dst, amount) && ops.setCount(src, static_cast<std::uint8_t>(have - amount));

    case Plan::Merge: {
        const int total = static_cast<int>(countOf(dst)) + static_cast<int>(amount);
        if (total > kMaxStackCount || !ops.setCount(dst, static_cast<std::uint8_t>(total))) {
            return false;
        }
        if (amount < have) {
            return ops.setCount(src, static_cast<std::uint8_t>(have - amount));
        }

        return empty != nullptr && ops.assignFrom(src, empty);
    }

    case Plan::None:
    default:
        return false;
    }
}

bool InventoryActionBridge::queueMove(const Places& places, Plan plan, std::uint8_t amount,
                                      const Spot& a, const Spot& b, std::byte* empty)
{
    if (m_queued >= kQueueSize) {
        return false;
    }

    ItemStackOps& ops = ItemStackOps::instance();
    Entry& entry = m_queue[m_queued];

    if (!ops.cloneTo(entry.beforeA, a.stack)) {
        return false;
    }
    if (!ops.cloneTo(entry.beforeB, b.stack)) {
        ops.destroyClone(entry.beforeA);
        return false;
    }

    if (!applyPlan(plan, amount, a, b, empty)) {
        ops.destroyClone(entry.beforeA);
        ops.destroyClone(entry.beforeB);
        return false;
    }

    if (!ops.cloneTo(entry.afterA, a.stack) || !ops.cloneTo(entry.afterB, b.stack)) {

        log().error(L"InventoryActionBridge: could not keep the result of a move, "
                    L"the server will not hear about it");
        ops.destroyClone(entry.beforeA);
        ops.destroyClone(entry.beforeB);
        return true;
    }

    entry.containerA = a.legacyContainer;
    entry.slotA = a.legacySlot;
    entry.containerB = b.legacyContainer;
    entry.slotB = b.legacySlot;
    entry.player = places.player;
    ++m_queued;

    if (a.notifySlot >= 0 && a.notifyContainer != nullptr) {
        ops.notifySlotChanged(a.notifyContainer, a.notifySlot);
    }
    if (b.notifySlot >= 0 && b.notifyContainer != nullptr) {
        ops.notifySlotChanged(b.notifyContainer, b.notifySlot);
    }

    m_blockLeft.fetch_add(1, std::memory_order_acq_rel);
    m_blockUntilMs.store(GetTickCount64() + kBlockWindowMs, std::memory_order_release);
    return true;
}

void InventoryActionBridge::onContainerOpen(void* packet)
{

    if (!bridgeActive()) {
        return;
    }
    if (packet == nullptr
        || !memory::isReadable(static_cast<const std::byte*>(packet) + kOpenIdOffset,
                               kOpenPeekBytes)) {
        return;
    }
    const auto* const base = static_cast<const std::byte*>(packet);
    const int id = static_cast<int>(static_cast<std::uint8_t>(base[kOpenIdOffset]));
    const int kind = static_cast<int>(static_cast<std::uint8_t>(base[kOpenKindOffset]));
    m_openContainerId.store(id, std::memory_order_release);
    m_openContainerKind.store(kind, std::memory_order_release);

    if (m_containerOpenProbes.fetch_sub(1, std::memory_order_relaxed) > 0) {
        std::uint64_t words[kOpenPeekBytes / sizeof(std::uint64_t)] = {};
        std::memcpy(words, base + kOpenIdOffset, sizeof(words));
        log().info(L"InventoryActionBridge: container-open id {} kind {} "
                   L"({:#x} {:#x} {:#x} {:#x} {:#x} {:#x})",
                   id, kind, words[0], words[1], words[2], words[3], words[4], words[5]);
    }
}

void InventoryActionBridge::onFrame()
{

    if (!bridgeActive()) {
        return;
    }

    OpenContainer open;
    void* const reached = resolveOpenContainer(open) ? open.container : nullptr;
    if (m_openContainer.exchange(reached, std::memory_order_acq_rel) != reached
        && reached != nullptr) {
        log().info(L"InventoryActionBridge: reached the open container at {:#x} "
                   L"({} slots at {:#x})",
                   reinterpret_cast<std::uintptr_t>(open.container), open.count,
                   reinterpret_cast<std::uintptr_t>(open.slots));
    }

    if (m_queued <= 0) {
        return;
    }

    for (int i = 0; i < m_queued; ++i) {
        const Entry& entry = m_queue[i];
        const bool sent = LegacyTransaction::instance().apply(
            entry.player,
            LegacyTransaction::Change{entry.containerA, entry.slotA, entry.beforeA, entry.afterA},
            LegacyTransaction::Change{entry.containerB, entry.slotB, entry.beforeB, entry.afterB});

        if (sent) {
            log().info(L"InventoryActionBridge: told the server through the legacy path "
                       L"({}:{} and {}:{})",
                       entry.containerA, entry.slotA, entry.containerB, entry.slotB);
        } else {
            log().warn(L"InventoryActionBridge: the legacy path refused the move "
                       L"({}:{} and {}:{}), the client and the server may disagree now",
                       entry.containerA, entry.slotA, entry.containerB, entry.slotB);
        }
    }
    clearQueue();
}

}
