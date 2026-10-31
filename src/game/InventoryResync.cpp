#include "game/InventoryResync.h"

#include <Windows.h>

#include <cstring>

#include "core/Logger.h"
#include "game/ItemStackOps.h"
#include "game/ContainerUi.h"
#include "core/Notice.h"
#include "game/LegacyTransaction.h"
#include "memory/Memory.h"
#include "modules/HandRestock.h"

namespace tsukuyomi {

namespace {

bool readArrived(const std::byte* entry, std::ptrdiff_t itemAt, std::ptrdiff_t countAt, std::ptrdiff_t netAt,
                 bool& present, std::uint16_t& count, std::int32_t& net)
{
    const void* item = nullptr;
    if (!memory::copyGuarded(entry + itemAt, &item, sizeof(item))
        || !memory::copyGuarded(entry + countAt, &count, sizeof(count))
        || !memory::copyGuarded(entry + netAt, &net, sizeof(net))) {
        return false;
    }
    present = item != nullptr && count > 0;
    return true;
}

}

InventoryResync& InventoryResync::instance()
{
    static InventoryResync object;
    return object;
}

void InventoryResync::markStale()
{
    m_staleSerial.fetch_add(1, std::memory_order_acq_rel);
    if (!m_stale.exchange(true, std::memory_order_acq_rel)
        && m_logs.fetch_add(1, std::memory_order_relaxed) < kMaxLogs) {
        log().info(L"InventoryResync: a legacy move left the inventory net ids stale, the server is asked "
                   L"to resend them when a screen opens");
    }
}

void InventoryResync::onInventoryOpening()
{
    if (!stale()) return;
    m_wantedBy.store(1, std::memory_order_relaxed);
    m_wanted.store(true, std::memory_order_release);
}

void InventoryResync::onContainerOpened()
{
    if (const unsigned long long sentAt = m_sentAtMs.load(std::memory_order_acquire);
        sentAt != 0 && m_logs.fetch_add(1, std::memory_order_relaxed) < kMaxLogs) {
        log().info(L"InventoryResync: a container opened {} ms after the request", GetTickCount64() - sentAt);
    }
    if (!stale()) return;
    int expected = 0;
    m_wantedBy.compare_exchange_strong(expected, 2, std::memory_order_relaxed);
    m_wanted.store(true, std::memory_order_release);
}

void InventoryResync::onInventoryContent(int containerId, const void* packet)
{
    const unsigned long long sentAt = m_sentAtMs.load(std::memory_order_acquire);
    if (sentAt == 0 || packet == nullptr) return;
    if (containerId != kInventoryWindow && containerId != kOffhandWindow) return;
    const unsigned long long now = GetTickCount64();
    if (m_logs.fetch_add(1, std::memory_order_relaxed) < kMaxLogs) {
        log().info(L"InventoryResync: the server sent window {} {} ms after the request", containerId, now - sentAt);
    }

    const int wanted = containerId == kInventoryWindow ? kSlotCount : 1;
    std::uint64_t range[2]{};
    if (!memory::copyGuarded(static_cast<const std::byte*>(packet) + kPacketSlotsOffset, range, sizeof(range))
        || range[0] == 0 || range[1] < range[0] + kEntrySize * wanted) {
        return;
    }
    Window slots{};
    for (int slot = 0; slot < wanted; ++slot) {
        const auto* const entry = reinterpret_cast<const std::byte*>(range[0]) + kEntrySize * slot;
        Entry& one = slots[slot];
        if (!readArrived(entry, kEntryItemOffset, kEntryCountOffset, kEntryNetIdOffset, one.present, one.count, one.net)) {
            return;
        }
        if (!one.present) one = Entry{};
    }
    const std::lock_guard<std::mutex> lock(m_arrivedLock);
    if (m_sentAtMs.load(std::memory_order_acquire)!=sentAt) return;
    if (containerId == kOffhandWindow) {
        m_arrivedOffhandAtMs = now;
        m_arrivedOffhandSeen = true;
        m_arrivedOffhand = slots[0];
        return;
    }
    m_arrived = true;
    m_arrivedAtMs = now;
    m_arrivedSlots = slots;
}

void InventoryResync::onPlayerViewUpdate()
{
    checkArrived();
    const unsigned long long sentAt = m_sentAtMs.load(std::memory_order_acquire);
    if (sentAt != 0 && GetTickCount64() > sentAt + kAnswerWaitMs) {
        m_sentAtMs.store(0, std::memory_order_release);
        if (m_logs.fetch_add(1, std::memory_order_relaxed) < kMaxLogs) {
            log().warn(L"InventoryResync: the server did not resend the inventory within {} ms", kAnswerWaitMs);
        }
    }
    if (m_wanted.exchange(false, std::memory_order_acq_rel)) send();
}

void InventoryResync::send()
{
    const int by = m_wantedBy.exchange(0, std::memory_order_relaxed);
    if (!stale() || m_sentAtMs.load(std::memory_order_acquire) != 0) return;
    HandRestock::ClientInventory inventory;
    if (!HandRestock::instance().clientInventory(inventory)) {
        if (m_logs.fetch_add(1, std::memory_order_relaxed) < kMaxLogs) {
            log().warn(L"InventoryResync: could not reach the local player, the resend is skipped");
        }
        return;
    }
    for (int slot = 0; slot < kSlotCount; ++slot) {
        Entry one;
        const bool read = readClient(inventory.slots + kStackSize * slot, one);
        m_beforeNet[slot] = read && one.present ? one.net : 0;
        m_beforeIdentity[slot] = read && one.present ? one.identity : inventoryresync::StackIdentity{};
    }
    {
        Entry one;
        m_beforeOffhand = readClient(inventory.offhand, one) && one.present ? one.identity : inventoryresync::StackIdentity{};
    }
    {
        const std::lock_guard<std::mutex> lock(m_arrivedLock);
        m_arrived = false;
        m_arrivedOffhandSeen = false;
    }
    m_sentSerial = m_staleSerial.load(std::memory_order_acquire);
    m_sentAtMs.store(GetTickCount64(), std::memory_order_release);
    if (!LegacyTransaction::instance().requestResync(inventory.player)) {
        m_sentAtMs.store(0,std::memory_order_release);
        if (m_logs.fetch_add(1, std::memory_order_relaxed) < kMaxLogs) {
            log().warn(L"InventoryResync: could not ask the server to resend the inventory");
        }
        return;
    }
    if (m_logs.fetch_add(1, std::memory_order_relaxed) < kMaxLogs) {
        log().info(L"InventoryResync: asked the server to resend the inventory ({})",
                   by == 1 ? L"the inventory is opening" : L"a container opened");
    }
}

bool InventoryResync::readClient(const std::byte* stack, Entry& out)
{
    out = Entry{};
    const void* item = nullptr;
    std::uint8_t count = 0;
    std::int32_t value = 0;
    std::uint8_t tag = 0xFF;
    if (!memory::copyGuarded(stack + kStackItemOffset, &item, sizeof(item))
        || !memory::copyGuarded(stack + kStackCountOffset, &count, sizeof(count))
        || !memory::copyGuarded(stack + kStackNetIdOffset, &value, sizeof(value))
        || !memory::copyGuarded(stack + kStackNetTagOffset, &tag, sizeof(tag))) {
        return false;
    }
    out.identity.item = reinterpret_cast<std::uintptr_t>(item);
    out.identity.known = memory::copyGuarded(stack+0x20,&out.identity.aux,sizeof(out.identity.aux))
        && containerui::tagHashOf(stack,out.identity.nbt);
    out.present = item != nullptr && count > 0;
    out.count = count;
    out.net = tag == 0 ? value : -static_cast<std::int32_t>(tag);
    return true;
}

bool InventoryResync::lineUp(std::byte* stack, const Entry& arrived, const inventoryresync::StackIdentity& sent,
                             std::int32_t& before)
{
    Entry now;
    if (!readClient(stack, now) || !now.present || !arrived.present) return false;
    before = now.net;
    if (!inventoryresync::sameItem(now.identity, sent)
        || now.count != arrived.count || now.net == arrived.net || now.net < 0) return false;
    return ItemStackOps::instance().setNetIdValue(stack, arrived.net);
}

void InventoryResync::checkArrived()
{
    const auto waiting=m_sentAtMs.load(std::memory_order_acquire);
    if (waiting==0 || GetTickCount64()>waiting+kAnswerWaitMs) return;
    Window slots{};
    Entry offhand{};
    bool offhandSeen = false;
    unsigned long long arrivedAt = 0;
    {
        const std::lock_guard<std::mutex> lock(m_arrivedLock);
        if (m_sentAtMs.load(std::memory_order_acquire) == 0 || !m_arrived) return;
        const unsigned long long now = GetTickCount64();
        if (now < m_arrivedAtMs + kSettleMs) return;
        if (m_arrivedOffhandSeen ? now < m_arrivedOffhandAtMs + kSettleMs : now < m_arrivedAtMs + kOffhandGraceMs) return;
        m_arrived = false;
        slots = m_arrivedSlots;
        offhand = m_arrivedOffhand;
        offhandSeen = m_arrivedOffhandSeen;
        arrivedAt = m_arrivedAtMs;
    }
    const unsigned long long sentAt = m_sentAtMs.load(std::memory_order_acquire);
    m_sentAtMs.store(0, std::memory_order_release);

    const bool movedSince = m_staleSerial.load(std::memory_order_acquire) != m_sentSerial;
    HandRestock::ClientInventory inventory;
    if (movedSince || !HandRestock::instance().clientInventory(inventory)) {
        if (m_logs.fetch_add(1, std::memory_order_relaxed) < kMaxLogs) {
            log().info(L"InventoryResync: the resend arrived {} ms after the request but {}; kept stale",
                       sentAt != 0 ? arrivedAt - sentAt : 0,
                       movedSince ? L"the inventory moved again since" : L"the inventory could not be reached");
        }
        return;
    }

    int matched = 0, byGame = 0, wrote = 0, differed = 0;
    int firstSlot = -1;
    std::int32_t firstServer = 0, firstClient = 0;
    for (int slot = 0; slot < kSlotCount; ++slot) {
        if (!slots[slot].present) {
            Entry now;
            if (!readClient(inventory.slots+kStackSize*slot,now) || now.present) ++differed;
            continue;
        }
        std::byte* const stack = inventory.slots + kStackSize * slot;
        Entry now;
        if (!readClient(stack, now)) { ++differed; continue; }
        if (now.present && now.net == slots[slot].net && now.count == slots[slot].count) {
            ++matched;
            if (m_beforeNet[slot] != slots[slot].net) ++byGame;
            continue;
        }
        std::int32_t before = 0;
        if (lineUp(stack, slots[slot], m_beforeIdentity[slot], before)) {
            ++wrote;
            Entry mirror;
            const void* mirrorItem = nullptr;
            const void* slotItem = nullptr;
            if (slot == inventory.hand && readClient(inventory.mainhand, mirror) && mirror.net == before
                && inventoryresync::sameItem(mirror.identity, m_beforeIdentity[slot])
                && memory::copyGuarded(inventory.mainhand + kStackItemOffset, &mirrorItem, sizeof(mirrorItem))
                && memory::copyGuarded(stack + kStackItemOffset, &slotItem, sizeof(slotItem)) && mirrorItem == slotItem) {
                ItemStackOps::instance().setNetIdValue(inventory.mainhand, slots[slot].net);
            }
            continue;
        }
        ++differed;
        if (firstSlot < 0) {
            firstSlot = slot;
            firstServer = slots[slot].net;
            firstClient = now.present ? now.net : 0;
        }
    }
    const wchar_t* offhandResult = L"not resent";
    bool offhandDone = false;
    if (offhandSeen) {
        Entry now;
        std::int32_t before = 0;
        if (!offhand.present) {
            offhandDone = readClient(inventory.offhand,now) && !now.present;
            offhandResult = offhandDone ? L"empty" : L"differs";
        }
        else if (readClient(inventory.offhand, now) && now.present && now.net == offhand.net && now.count == offhand.count) { offhandResult = L"matched";offhandDone=true; }
        else if (lineUp(inventory.offhand, offhand, m_beforeOffhand, before)) { offhandResult = L"lined up";offhandDone=true; }
        else offhandResult = L"differs";
    }
    if (m_logs.fetch_add(1, std::memory_order_relaxed) < kMaxLogs) {
        log().info(L"InventoryResync: the resend arrived {} ms after the request: {} stack(s) carry the server's net "
                   L"ids ({} lined up by the game), {} written here, {} differ (first slot {}: server {} / client {}); "
                   L"offhand {}",
                   sentAt != 0 ? arrivedAt - sentAt : 0, matched, byGame, wrote, differed, firstSlot, firstServer,
                   firstClient, offhandResult);
    }
    if (differed == 0 && offhandDone) {
        m_stale.store(false, std::memory_order_release);
        if (m_staleSerial.load(std::memory_order_acquire) != m_sentSerial) m_stale.store(true, std::memory_order_release);
    }
}

}
