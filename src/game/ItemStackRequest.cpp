#include "game/ItemStackRequest.h"

#include <Windows.h>

#include <cstring>

#include "core/Logger.h"
#include "hooks/Detours.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"

namespace tsukuyomi {

namespace {

int accessViolationFilter(unsigned long code)
{
    return (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR)
               ? EXCEPTION_EXECUTE_HANDLER
               : EXCEPTION_CONTINUE_SEARCH;
}

bool readPointerGuarded(const void* address, void*& value)
{
    __try {
        value = *static_cast<void* const*>(address);
        return true;
    } __except (accessViolationFilter(GetExceptionCode())) {
        return false;
    }
}

bool readU8Guarded(const void* address, std::uint8_t& value)
{
    __try {
        value = *static_cast<const std::uint8_t*>(address);
        return true;
    } __except (accessViolationFilter(GetExceptionCode())) {
        return false;
    }
}

bool readBytesGuarded(const void* address, void* out, std::size_t size)
{
    __try {
        std::memcpy(out, address, size);
        return true;
    } __except (accessViolationFilter(GetExceptionCode())) {
        return false;
    }
}

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

struct SendPayload {
    void (__fastcall* begin)(void*, void*);
    void (__fastcall* makeTake)(void**, const std::uint8_t*, const void*, const void*);
    void (__fastcall* makePlace)(void**, const std::uint8_t*, const void*, const void*);
    void (__fastcall* makeSwap)(void**, const void*, const void*);
    void (__fastcall* addAction)(void**, void**);
    void (__fastcall* end)(void*);

    struct EndGuard {
        void* client = nullptr;
        unsigned char engaged = 0;
    } endGuard;
    void* client;

    bool useSwap = false;
    const void* from;
    const void* cursorEmpty;
    const void* cursorHeld;
    const void* to;
    const std::uint8_t* amount;

    void* pendingBefore = nullptr;
    void* pendingAfterBegin = nullptr;
    void* firstAction = nullptr;
    void* secondAction = nullptr;
    std::uint64_t actionCount = 0;
    void* pendingAfterEnd = nullptr;
    std::int32_t requestId = 0;
};

bool sendGuarded(SendPayload& p, const void** faultPc, const void** faultAddress)
{
    __try {
        auto* const bytes = static_cast<std::byte*>(p.client);
        p.pendingBefore = *reinterpret_cast<void**>(bytes + 0x60);

        p.begin(p.client, nullptr);
        p.pendingAfterBegin = *reinterpret_cast<void**>(bytes + 0x60);

        if (p.useSwap) {
            p.makeSwap(&p.firstAction, p.from, p.to);
        } else {
            p.makeTake(&p.firstAction, p.amount, p.from, p.cursorEmpty);
        }
        if (p.firstAction == nullptr) {

            return false;
        }

        void* holderA = p.client;
        void* first = p.firstAction;
        p.addAction(&holderA, &first);

        if (!p.useSwap) {
            p.makePlace(&p.secondAction, p.amount, p.cursorHeld, p.to);
            if (p.secondAction == nullptr) {
                return false;
            }
            void* holderB = p.client;
            void* second = p.secondAction;
            p.addAction(&holderB, &second);
        }

        if (p.pendingAfterBegin != nullptr) {
            auto* const req = static_cast<std::byte*>(p.pendingAfterBegin);
            const auto begin = *reinterpret_cast<std::uintptr_t*>(req + 0x30);
            const auto end = *reinterpret_cast<std::uintptr_t*>(req + 0x38);
            p.actionCount = end >= begin ? (end - begin) / sizeof(void*) : 0;
            p.requestId = *reinterpret_cast<std::int32_t*>(req + 0x08);
        }

        p.endGuard.client = p.client;
        p.endGuard.engaged = 1;
        p.end(&p.endGuard);
        p.pendingAfterEnd = *reinterpret_cast<void**>(bytes + 0x60);
        return true;
    } __except (faultFilter(GetExceptionInformation(), faultPc, faultAddress)) {
        return false;
    }
}

bool notifyGuarded(void (*fn)(void*), void* client, const void** faultPc,
                   const void** faultAddress)
{
    __try {
        fn(client);
        return true;
    } __except (faultFilter(GetExceptionInformation(), faultPc, faultAddress)) {
        return false;
    }
}

constexpr std::size_t kSenderVtableOffset = 0x920;
constexpr std::size_t kSendVtableIndex = 2;

bool sendPacketGuarded(void* client, void* packet, const std::byte* moduleBase,
                       std::size_t moduleSize, const void** faultPc, const void** faultAddress)
{
    __try {
        auto** const clientVtable = *reinterpret_cast<void***>(client);
        using GetSenderFn = void*(__fastcall*)(void*);
        const auto getSender = reinterpret_cast<GetSenderFn>(
            clientVtable[kSenderVtableOffset / sizeof(void*)]);
        void* const sender = getSender(client);
        if (sender == nullptr) {
            return false;
        }
        auto** const senderVtable = *reinterpret_cast<void***>(sender);
        const auto* const vtableBytes = reinterpret_cast<const std::byte*>(senderVtable);
        if (vtableBytes < moduleBase || vtableBytes >= moduleBase + moduleSize) {
            return false;
        }
        using SendFn = void(__fastcall*)(void*, void*);
        const auto send = reinterpret_cast<SendFn>(senderVtable[kSendVtableIndex]);
        send(sender, packet);
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

ItemStackRequest& ItemStackRequest::instance()
{
    static ItemStackRequest object;
    return object;
}

void ItemStackRequest::onScansReady()
{

    if (m_scansReady.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    const Scanner& scanner = Scanner::instance();
    m_beginRequest = scanner.addressAs<BeginRequestFn>(Target::BeginRequest);
    m_makeTakeAction = scanner.addressAs<MakeTransferActionFn>(Target::MakeTakeAction);
    m_makePlaceAction = scanner.addressAs<MakeTransferActionFn>(Target::MakePlaceAction);
    m_makeSwapAction = scanner.addressAs<MakeSwapActionFn>(Target::MakeSwapAction);
    m_addRequestAction = scanner.addressAs<AddRequestActionFn>(Target::AddRequestAction);
    m_endRequest = scanner.addressAs<EndRequestFn>(Target::EndRequest);

    if (!available()) {
        log().warn(L"ItemStackRequest: the server-authoritative path is unavailable "
                   L"(begin {}, take {}, place {}, add {}, end {})",
                   m_beginRequest != nullptr, m_makeTakeAction != nullptr,
                   m_makePlaceAction != nullptr, m_addRequestAction != nullptr,
                   m_endRequest != nullptr);
    }

    if (available() && !canSwap()) {
        log().warn(L"ItemStackRequest: makeSwapAction was not found, "
                   L"swapping two non-empty slots is unavailable");
    }
}

bool ItemStackRequest::available() const
{
    return m_beginRequest != nullptr && m_makeTakeAction != nullptr
           && m_makePlaceAction != nullptr && m_addRequestAction != nullptr
           && m_endRequest != nullptr;
}

void ItemStackRequest::forget()
{
    m_client.store(nullptr, std::memory_order_release);
}

int ItemStackRequest::packetId(void* packet)
{

    if (packet == nullptr || !memory::isReadable(packet, sizeof(void*))) {
        return -1;
    }
    auto** const vtable = *reinterpret_cast<void***>(packet);
    if (!memory::isReadable(vtable, sizeof(void*) * (kGetIdVtableIndex + 1))) {
        return -1;
    }
    using GetIdFn = int(__fastcall*)(void*);
    const auto getId = reinterpret_cast<GetIdFn>(vtable[kGetIdVtableIndex]);
    return getId != nullptr ? getId(packet) : -1;
}

void ItemStackRequest::observePacket(void* packet)
{

    const int id = packetId(packet);

    if (id == kInteractPacketId && memory::isReadable(packet, kInteractPacketSize)) {
        const auto* const bytes = static_cast<const std::uint8_t*>(packet);
        if (bytes[kInteractActionOffset] == kInteractOpenInventory) {
            m_serverInventoryOpen.store(true, std::memory_order_release);

        }
        return;
    }

    if (id == kContainerClosePacketId && memory::isReadable(packet, kContainerCloseSize)) {
        const auto* const bytes = static_cast<const std::uint8_t*>(packet);
        if (bytes[kContainerCloseTypeOffset] == kContainerTypeNone) {
            m_serverInventoryOpen.store(false, std::memory_order_release);

            m_openedByUs.store(false, std::memory_order_release);

            if (packet != m_closeCopy
                && readBytesGuarded(packet, m_closeCopy, sizeof(m_closeCopy))) {
                m_hasCloseCopy.store(true, std::memory_order_release);
            }
        }
    }
}

void ItemStackRequest::onNotifyInventoryOpen(void* client)
{

    if (client == nullptr) {
        return;
    }
    void* vtable = nullptr;
    if (!readPointerGuarded(client, vtable) || vtable == nullptr) {
        return;
    }
    if (m_clientInstance.exchange(client, std::memory_order_acq_rel) != client) {
        m_clientVtable.store(vtable, std::memory_order_release);
        m_warnedNoClient.store(false, std::memory_order_release);
        log().info(L"ItemStackRequest: client instance found at {:#x}",
                   reinterpret_cast<std::uintptr_t>(client));
    }
}

bool ItemStackRequest::suppressingInputReset() const
{
    const unsigned long owner = m_inputResetThread.load(std::memory_order_acquire);
    return owner != 0 && owner == GetCurrentThreadId();
}

bool ItemStackRequest::notifyServerInventoryOpen()
{
    void* const client = m_clientInstance.load(std::memory_order_acquire);

    if (client == nullptr || !hooks::hasNotifyInventoryOpen()
        || !m_hasCloseCopy.load(std::memory_order_acquire)) {
        if (!m_warnedNoClient.exchange(true, std::memory_order_acq_rel)) {
            log().warn(L"ItemStackRequest: open and close your inventory once (E) so the mod "
                       L"can learn how to talk to the server about it, "
                       L"otherwise every request is rejected");
        }
        return false;
    }

    void* vtable = nullptr;
    if (!readPointerGuarded(client, vtable)
        || vtable != m_clientVtable.load(std::memory_order_acquire)) {
        m_clientInstance.store(nullptr, std::memory_order_release);
        log().warn(L"ItemStackRequest: the client instance went stale, "
                   L"open your inventory once (E) again");
        return false;
    }

    m_serverInventoryOpen.store(false, std::memory_order_release);

    const void* faultPc = nullptr;
    const void* faultAddress = nullptr;

    m_inputResetThread.store(GetCurrentThreadId(), std::memory_order_release);
    const bool told = notifyGuarded(&hooks::callNotifyInventoryOpen, client, &faultPc,
                                    &faultAddress);
    m_inputResetThread.store(0, std::memory_order_release);

    if (!told) {
        const ModuleRange& module = mainModule();
        const auto rva = module.contains(faultPc)
                             ? static_cast<std::size_t>(
                                   static_cast<const std::byte*>(faultPc) - module.base)
                             : 0;
        log().error(L"ItemStackRequest: telling the server the inventory opened faulted "
                    L"(pc rva {:#x}, touched {:#x}), dropping the client instance",
                    rva, reinterpret_cast<std::uintptr_t>(faultAddress));
        m_clientInstance.store(nullptr, std::memory_order_release);
        return false;
    }

    if (!m_serverInventoryOpen.load(std::memory_order_acquire)) {
        return false;
    }

    m_openedByUs.store(true, std::memory_order_release);

    m_suppressOpens.store(1, std::memory_order_release);
    m_suppressUntilMs.store(GetTickCount64() + kSuppressWindowMs, std::memory_order_release);
    return true;
}

std::byte* ItemStackRequest::findContainerOpenHandle()
{
    return findPacketHandle(Scanner::instance().address(Target::ContainerOpenGetId),
                            L"container-open", kHandleVtableOffset);
}

std::byte* ItemStackRequest::resolvePacketHandle(Target getIdTarget, const wchar_t* what)
{
    return findPacketHandle(Scanner::instance().address(getIdTarget), what, kHandleVtableOffset);
}

std::byte* ItemStackRequest::resolvePacketReader(Target getIdTarget, const wchar_t* what)
{
    return findPacketHandle(Scanner::instance().address(getIdTarget), what, kReadVtableOffset);
}

std::byte* ItemStackRequest::findContainerOpenReader()
{
    return findPacketHandle(Scanner::instance().address(Target::ContainerOpenGetId),
                            L"container-open reader", kReadVtableOffset);
}

std::byte* ItemStackRequest::findInventoryContentReader()
{
    return findPacketHandle(Scanner::instance().address(Target::InventoryContentGetId),
                            L"inventory-content reader", kReadVtableOffset);
}

std::byte* ItemStackRequest::findPacketHandle(std::byte* getId, const wchar_t* what,
                                              std::ptrdiff_t vtableOffset)
{
    if (getId == nullptr) {
        return nullptr;
    }

    static constexpr char kDigits[] = "0123456789ABCDEF";
    const auto value = reinterpret_cast<std::uintptr_t>(getId);
    char text[8 * 3];
    for (int i = 0; i < 8; ++i) {
        const auto byte = static_cast<unsigned>((value >> (i * 8)) & 0xFF);
        text[i * 3] = kDigits[byte >> 4];
        text[i * 3 + 1] = kDigits[byte & 0xF];
        text[i * 3 + 2] = ' ';
    }
    const ScanHit hit = scanMainModule(std::string_view(text, sizeof(text) - 1), ".rdata");
    if (hit.count != 1) {
        log().warn(L"ItemStackRequest: could not pin down the {} vtable ({} matches)", what,
                   hit.count);
        return nullptr;
    }

    std::byte* const vtable = hit.address - sizeof(void*);
    void* handle = nullptr;
    if (!readPointerGuarded(vtable + vtableOffset, handle) || handle == nullptr) {
        return nullptr;
    }
    if (!mainModule().contains(handle)) {
        return nullptr;
    }
    return static_cast<std::byte*>(handle);
}

bool ItemStackRequest::onContainerOpenHandle(void* packet, void* result)
{

    if (packet != nullptr && memory::isReadable(packet, 0x80)
        && m_openPacketLogs.fetch_add(1, std::memory_order_acq_rel) < kOpenPacketLogLimit) {

        std::uint64_t words[10]{};
        std::memcpy(words, static_cast<const std::byte*>(packet) + 0x30, sizeof(words));
        log().info(L"ItemStackRequest: container-open packet +0x30 "
                   L"({:#x} {:#x} {:#x} {:#x} {:#x} | {:#x} {:#x} {:#x} {:#x} {:#x})",
                   words[0], words[1], words[2], words[3], words[4], words[5], words[6],
                   words[7], words[8], words[9]);
    }

    if (m_suppressOpens.load(std::memory_order_acquire) <= 0 || result == nullptr) {
        return false;
    }

    if (GetTickCount64() > m_suppressUntilMs.load(std::memory_order_acquire)) {
        m_suppressOpens.store(0, std::memory_order_release);
        return false;
    }
    m_suppressOpens.fetch_sub(1, std::memory_order_acq_rel);

    std::memset(result, 0, kOpenResultSize);
    static_cast<std::byte*>(result)[kOpenResultTagOffset] = std::byte{1};
    return true;
}

void ItemStackRequest::rememberContainerOpenResult(const void* result)
{

    if (result == nullptr || m_loggedOpenResult.load(std::memory_order_acquire)) {
        return;
    }
    std::byte copy[kOpenResultSize];
    if (!readBytesGuarded(result, copy, sizeof(copy))) {
        return;
    }

    if (!m_loggedOpenResult.exchange(true, std::memory_order_acq_rel)) {
        std::uint64_t words[sizeof(copy) / sizeof(std::uint64_t)]{};
        std::memcpy(words, copy, sizeof(words));
        log().info(L"ItemStackRequest: container-open result "
                   L"({:#x} {:#x} {:#x} {:#x} {:#x} {:#x} {:#x} {:#x} {:#x})",
                   words[0], words[1], words[2], words[3], words[4], words[5], words[6],
                   words[7], words[8]);
    }

}

void ItemStackRequest::onFrame()
{

    const unsigned long long due = m_pendingCloseAtMs.load(std::memory_order_acquire);
    if (due == 0 || GetTickCount64() < due) {
        return;
    }
    m_pendingCloseAtMs.store(0, std::memory_order_release);

    m_suppressOpens.store(0, std::memory_order_release);

    closeServerInventory();
}

bool ItemStackRequest::closeServerInventory()
{
    if (!m_hasCloseCopy.load(std::memory_order_acquire)) {
        if (!m_warnedNoClose.exchange(true, std::memory_order_acq_rel)) {
            log().warn(L"ItemStackRequest: close your inventory once (E) so the mod can learn "
                       L"how to tell the server it closed, otherwise the screen stops opening");
        }
        return false;
    }
    void* const client = m_clientInstance.load(std::memory_order_acquire);
    if (client == nullptr) {
        return false;
    }

    const ModuleRange& module = mainModule();
    const void* faultPc = nullptr;
    const void* faultAddress = nullptr;
    if (!sendPacketGuarded(client, m_closeCopy, module.base, module.size, &faultPc,
                           &faultAddress)) {
        const auto rva = module.contains(faultPc)
                             ? static_cast<std::size_t>(
                                   static_cast<const std::byte*>(faultPc) - module.base)
                             : 0;
        log().error(L"ItemStackRequest: telling the server the inventory closed failed "
                    L"(pc rva {:#x}, touched {:#x}), the inventory screen may stop opening",
                    rva, reinterpret_cast<std::uintptr_t>(faultAddress));
        return false;
    }

    const bool stillOpen = m_serverInventoryOpen.load(std::memory_order_acquire);
    if (stillOpen) {
        log().warn(L"ItemStackRequest: sent the close but the server still looks open; "
                   L"the inventory screen may not open with E");
    } else {
        log().info(L"ItemStackRequest: told the server the inventory closed");
    }
    return !stillOpen;
}

ItemStackRequest::SlotRef ItemStackRequest::inventorySlot(const void* slots, int index)
{
    SlotRef ref;
    if (slots == nullptr || index < 0 || index >= kSlotCount) {
        return ref;
    }

    ref.container = index < kHotbarSlots ? kContainerHotbar : kContainerInventory;
    ref.slot = index;
    ref.stack = static_cast<const std::byte*>(slots) + index * kSlotStride;
    return ref;
}

ItemStackRequest::SlotRef ItemStackRequest::offhandSlot(const void* stack)
{
    SlotRef ref;
    ref.container = kContainerOffhand;
    ref.slot = kOffhandSlotIndex;
    ref.stack = stack;
    return ref;
}

ItemStackRequest::SlotInfo ItemStackRequest::makeSlotInfo(const SlotRef& ref, const NetId& net)
{
    SlotInfo info;
    info.container = ref.container;
    info.slot = static_cast<std::uint8_t>(ref.slot);
    info.netValue = net.value;
    info.netAlt = net.alt;
    info.netTag = net.tag;
    return info;
}

ItemStackRequest::SlotInfo ItemStackRequest::makeCursorInfo(const NetId& net)
{

    SlotInfo info;
    info.container = kContainerCursor;
    info.slot = 0;
    info.netValue = net.value;
    info.netAlt = net.alt;
    info.netTag = net.tag;
    return info;
}

bool ItemStackRequest::readStackAt(const void* stack, NetId& net, std::uint8_t& count)
{
    if (stack == nullptr) {
        return false;
    }
    std::byte raw[kSlotStride];
    if (!readBytesGuarded(stack, raw, sizeof(raw))) {
        return false;
    }
    count = static_cast<std::uint8_t>(raw[kStackCountOffset]);
    std::memcpy(&net.value, raw + kStackNetValueOffset, sizeof(net.value));
    std::memcpy(&net.alt, raw + kStackNetAltOffset, sizeof(net.alt));
    net.tag = static_cast<std::uint8_t>(raw[kStackNetTagOffset]);
    return true;
}

void* ItemStackRequest::findNetManager()
{
    std::byte* const ref = Scanner::instance().address(Target::NetManagerVtableRef);
    if (ref == nullptr) {
        return nullptr;
    }
    const auto* const vtable = static_cast<const std::byte*>(
        memory::ripTarget(ref, kNetManagerVtableDisp));
    if (vtable == nullptr) {
        return nullptr;
    }

    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    auto* address = static_cast<std::byte*>(info.lpMinimumApplicationAddress);
    auto* const limit = static_cast<std::byte*>(info.lpMaximumApplicationAddress);

    MEMORY_BASIC_INFORMATION region{};
    while (address < limit && VirtualQuery(address, &region, sizeof(region)) == sizeof(region)) {
        auto* const base = static_cast<std::byte*>(region.BaseAddress);
        const std::size_t size = region.RegionSize;
        const bool usable = region.State == MEM_COMMIT && region.Type == MEM_PRIVATE
                            && (region.Protect == PAGE_READWRITE
                                || region.Protect == PAGE_WRITECOPY)
                            && (region.Protect & PAGE_GUARD) == 0;
        if (usable) {

            auto* const end = base + (size & ~static_cast<std::size_t>(7));
            for (auto* p = base; p + sizeof(void*) <= end; p += sizeof(void*)) {
                if (*reinterpret_cast<const std::byte* const*>(p) != vtable) {
                    continue;
                }

                std::uint8_t valid = 0;
                if (!readU8Guarded(p + kValidFlagOffset, valid)) {
                    continue;
                }
                return p;
            }
        }
        address = base + size;
    }
    return nullptr;
}

void* ItemStackRequest::netManager()
{
    void* cached = m_client.load(std::memory_order_acquire);
    if (cached != nullptr) {

        void* head = nullptr;
        std::byte* const ref = Scanner::instance().address(Target::NetManagerVtableRef);
        if (ref != nullptr && readPointerGuarded(cached, head)
            && head == memory::ripTarget(ref, kNetManagerVtableDisp)) {
            return cached;
        }
        m_client.store(nullptr, std::memory_order_release);
    }

    void* const found = findNetManager();
    if (found != nullptr) {
        m_client.store(found, std::memory_order_release);

        m_openedByUs.store(false, std::memory_order_release);
        log().info(L"ItemStackRequest: net manager found at {:#x}",
                   reinterpret_cast<std::uintptr_t>(found));
    }
    return found;
}

bool ItemStackRequest::requestMove(const void* slots, int fromSlot, int toSlot)
{
    if (slots == nullptr || fromSlot == toSlot) {
        return false;
    }
    const SlotRef from = inventorySlot(slots, fromSlot);
    const SlotRef to = inventorySlot(slots, toSlot);
    if (from.stack == nullptr || to.stack == nullptr) {
        return false;
    }

    NetId toNet;
    std::uint8_t toCount = 0;
    if (!readStackAt(to.stack, toNet, toCount)) {
        return false;
    }
    if (toCount != 0) {
        if (!m_warnedOccupied.exchange(true, std::memory_order_acq_rel)) {
            log().warn(L"ItemStackRequest: slot {} already holds {} items, "
                       L"the move needs a swap and is skipped",
                       toSlot, toCount);
        }
        return false;
    }
    m_warnedOccupied.store(false, std::memory_order_release);

    return requestSwap(from, to);
}

bool ItemStackRequest::requestSwap(const SlotRef& a, const SlotRef& b)
{
    return sendSwap(a, b);
}

bool ItemStackRequest::sendSwap(const SlotRef& a, const SlotRef& b)
{
    if (!available() || a.stack == nullptr || b.stack == nullptr || a.stack == b.stack) {
        return false;
    }

    if (a.slot < 0 || a.slot > 0xFF || b.slot < 0 || b.slot > 0xFF) {
        return false;
    }

    NetId netA;
    std::uint8_t countA = 0;
    NetId netB;
    std::uint8_t countB = 0;
    if (!readStackAt(a.stack, netA, countA) || !readStackAt(b.stack, netB, countB)) {
        return false;
    }
    if (countA == 0 && countB == 0) {
        return false;
    }

    const bool useSwap = countA != 0 && countB != 0;
    if (useSwap && !canSwap()) {
        if (!m_warnedNoSwap.exchange(true, std::memory_order_acq_rel)) {
            log().warn(L"ItemStackRequest: makeSwapAction is missing, "
                       L"swapping two non-empty slots is skipped");
        }
        return false;
    }
    m_warnedNoSwap.store(false, std::memory_order_release);

    const bool forward = countA != 0;
    const SlotRef& srcRef = forward ? a : b;
    const SlotRef& dstRef = forward ? b : a;
    const NetId& srcNet = forward ? netA : netB;
    const NetId& dstNet = forward ? netB : netA;
    const std::uint8_t amount = forward ? countA : countB;

    void* const client = netManager();
    if (client == nullptr) {
        if (!m_warnedMissing.exchange(true, std::memory_order_acq_rel)) {
            log().warn(L"ItemStackRequest: no net manager in memory, "
                       L"the server-authoritative path is skipped");
        }
        return false;
    }
    m_warnedMissing.store(false, std::memory_order_release);

    auto* const bytes = static_cast<std::byte*>(client);

    std::uint8_t valid = 0;
    if (!readU8Guarded(bytes + kValidFlagOffset, valid) || valid != 1) {
        return false;
    }

    void* pending = nullptr;
    if (!readPointerGuarded(bytes + kPendingOffset, pending)) {
        return false;
    }
    if (pending != nullptr) {
        return false;
    }

    const bool alreadyOpen = m_serverInventoryOpen.load(std::memory_order_acquire);
    if (!alreadyOpen && !notifyServerInventoryOpen()) {

        if (!m_warnedNotOpen.exchange(true, std::memory_order_acq_rel)) {
            log().warn(L"ItemStackRequest: could not tell the server the inventory is open, "
                       L"the move is skipped");
        }
        return false;
    }
    m_warnedNotOpen.store(false, std::memory_order_release);

    const NetId emptyNet;
    const SlotInfo from = makeSlotInfo(srcRef, srcNet);
    const SlotInfo cursorEmpty = makeCursorInfo(emptyNet);
    const SlotInfo cursorHeld = makeCursorInfo(srcNet);
    const SlotInfo to = makeSlotInfo(dstRef, dstNet);

    SendPayload payload{};
    payload.begin = m_beginRequest;
    payload.makeTake = m_makeTakeAction;
    payload.makePlace = m_makePlaceAction;
    payload.makeSwap = m_makeSwapAction;
    payload.addAction = m_addRequestAction;
    payload.end = m_endRequest;
    payload.client = client;
    payload.useSwap = useSwap;
    payload.from = &from;
    payload.cursorEmpty = &cursorEmpty;
    payload.cursorHeld = &cursorHeld;
    payload.to = &to;
    payload.amount = &amount;

    const void* faultPc = nullptr;
    const void* faultAddress = nullptr;

    m_building.fetch_add(1, std::memory_order_acq_rel);
    const bool sent = sendGuarded(payload, &faultPc, &faultAddress);
    m_building.fetch_sub(1, std::memory_order_acq_rel);

    if (m_openedByUs.load(std::memory_order_acquire)) {
        m_pendingCloseAtMs.store(GetTickCount64() + kCloseDelayMs, std::memory_order_release);
    }

    if (!sent) {
        const ModuleRange& module = mainModule();
        const auto rva = module.contains(faultPc)
                             ? static_cast<std::size_t>(
                                   static_cast<const std::byte*>(faultPc) - module.base)
                             : 0;
        log().error(L"ItemStackRequest: the move request faulted "
                    L"(pc rva {:#x}, touched {:#x}), dropping the net manager",
                    rva, reinterpret_cast<std::uintptr_t>(faultAddress));
        m_client.store(nullptr, std::memory_order_release);
        return false;
    }

    m_lastRequestId.store(payload.requestId, std::memory_order_release);
    m_responseId.store(0, std::memory_order_release);

    log().info(L"ItemStackRequest: {} {}:{} -> {}:{} (count {}, netId {} tag {}, "
               L"pending {:#x}->{:#x}->{:#x}, actions {}, request {}, screen {})",
               useSwap ? L"swap" : L"move", srcRef.container,
               srcRef.slot, dstRef.container,
               dstRef.slot, amount, srcNet.value, srcNet.tag,
               reinterpret_cast<std::uintptr_t>(payload.pendingBefore),
               reinterpret_cast<std::uintptr_t>(payload.pendingAfterBegin),
               reinterpret_cast<std::uintptr_t>(payload.pendingAfterEnd),
               payload.actionCount, payload.requestId, alreadyOpen ? L"kept" : L"opened");

    return true;
}

void ItemStackRequest::onResponse(const void* entries)
{
    const std::int32_t wanted = m_lastRequestId.load(std::memory_order_acquire);
    if (entries == nullptr || wanted == 0) {
        return;
    }

    std::byte* range[2] = {};
    if (!readBytesGuarded(entries, range, sizeof(range))) {
        return;
    }
    if (range[0] == nullptr || range[1] == nullptr || range[1] < range[0]) {
        return;
    }

    std::byte* it = range[0];
    for (int i = 0; i < kMaxResponseEntries; ++i) {
        if (it + kResponseEntrySize > range[1]) {
            return;
        }
        std::byte raw[kResponseEntrySize];
        if (!readBytesGuarded(it, raw, sizeof(raw))) {
            return;
        }
        std::int32_t id = 0;
        std::memcpy(&id, raw + kResponseRequestIdOffset, sizeof(id));
        if (id == wanted) {
            const int code =
                static_cast<int>(static_cast<std::uint8_t>(raw[kResponseResultOffset]));
            m_responseResult.store(code, std::memory_order_release);
            m_responseId.store(id, std::memory_order_release);

            std::uint64_t words[kResponseEntrySize / sizeof(std::uint64_t)] = {};
            std::memcpy(words, raw, sizeof(words));
            log().info(L"ItemStackRequest: request {} answered with result {} "
                       L"({:#x} {:#x} {:#x} {:#x} {:#x} {:#x})",
                       id, code, words[0], words[1], words[2], words[3], words[4], words[5]);
            return;
        }
        it += kResponseEntrySize;
    }
}

bool ItemStackRequest::takeResponse(std::int32_t id, int& result)
{
    if (id == 0 || m_responseId.load(std::memory_order_acquire) != id) {
        return false;
    }
    result = m_responseResult.load(std::memory_order_acquire);

    m_responseId.store(0, std::memory_order_release);
    return true;
}

}
