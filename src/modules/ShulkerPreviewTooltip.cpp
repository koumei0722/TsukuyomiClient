#include "modules/ShulkerPreview.h"

#include "config/WriteSwitches.h"
#include "core/Logger.h"
#include "core/Notice.h"
#include "core/Strings.h"
#include "game/GameString.h"
#include "game/ShulkerPreviewLayout.h"
#include "input/GameButtons.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>

namespace tsukuyomi {

namespace {

namespace cui = containerui;
namespace sp = spreview;

static_assert(sp::kTextSlotName < cui::kPersistentTextSlots && sp::kTextSlotSelectedName < cui::kPersistentTextSlots);

using CtorFn = void(__fastcall*)(void* stack);
using LoadFn = void(__fastcall*)(void* stack, const void* tag);
using ItemStackFn = std::uintptr_t(__fastcall*)(const void* item, void* stack);
using DtorFn = void(__fastcall*)(void* stack, int flags);
using DeleteFn = gamestring::DeleteFn;
using TagHashFn = std::uint64_t(__fastcall*)(const void* tag);
using HoverNameFn = void(__fastcall*)(const void* stack, void* out);

constexpr std::size_t kTagHashSlot = 0x50 / 8;

constexpr std::ptrdiff_t kStackItem = 0x08;
constexpr std::ptrdiff_t kStackAux = 0x20;

struct Api {
    CtorFn ctor = nullptr;
    LoadFn load = nullptr;
    int fixupSlot = -1;
    void** allocatorAt = nullptr;
    DeleteFn gameDelete = nullptr;
    TagHashFn tagHash = nullptr;
    HoverNameFn hoverName = nullptr;
    bool ready = false;
    bool broken = false;
};
Api g_api;

std::byte* findInFunction(std::byte* fn, std::string_view pattern)
{
    if (fn == nullptr) {
        return nullptr;
    }
    const std::size_t size = memory::functionSize(fn);
    if (size == 0) {
        return nullptr;
    }
    if (!memory::isReadable(fn, size)) {
        return nullptr;
    }
    const ScanHit hit = scanRange(std::span<std::byte>(fn, size), pattern);
    return hit.count == 1 ? hit.address : nullptr;
}

void* callTarget(const std::byte* e8)
{
    if (e8 == nullptr || !memory::isReadable(e8, 5) || e8[0] != std::byte{0xe8}) {
        return nullptr;
    }
    std::int32_t rel = 0;
    std::memcpy(&rel, e8 + 1, 4);
    void* const target = const_cast<std::byte*>(e8 + 5 + rel);
    return memory::inGameModule(target) ? target : nullptr;
}

int readDisp32(const std::byte* at)
{
    std::int32_t disp = 0;
    std::memcpy(&disp, at, 4);
    return disp;
}

bool buildStackGuarded(void* elem, const void* tag)
{
    __try {
        std::memset(elem, 0, sizeof(ShulkerPreview::Elem));
        g_api.ctor(elem);
        g_api.load(elem, tag);
        auto* const bytes = static_cast<std::byte*>(elem);
        void* weak = *reinterpret_cast<void**>(bytes + kStackItem);
        void* item = (weak != nullptr) ? *static_cast<void**>(weak) : nullptr;
        if (item != nullptr) {
            void** const vt = *static_cast<void***>(item);
            reinterpret_cast<ItemStackFn>(vt[g_api.fixupSlot / 8])(item, elem);
        }

        auto* const aux = reinterpret_cast<std::uint16_t*>(bytes + kStackAux);
        if (*aux == 0x7FFF) {
            *aux = 0;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool destroyStackGuarded(void* elem)
{
    __try {
        void** const vt = *static_cast<void***>(elem);
        reinterpret_cast<DtorFn>(vt[0])(elem, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool tagHashGuarded(const void* tag, std::uint64_t& out)
{
    __try {
        void* const* const vt = *static_cast<void* const* const*>(tag);
        if (vt == nullptr || vt[kTagHashSlot] != reinterpret_cast<void*>(g_api.tagHash)) {
            return false;
        }
        out = g_api.tagHash(tag);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool isPreviewCollection(const void* str)
{
    constexpr std::size_t kLength = sizeof(sp::kCollection) - 1;
    __try {
        const auto* const bytes = static_cast<const std::byte*>(str);
        if (*reinterpret_cast<const std::size_t*>(bytes + 0x10) != kLength) {
            return false;
        }
        const std::size_t capacity = *reinterpret_cast<const std::size_t*>(bytes + 0x18);
        const char* const text = capacity >= 16 ? *reinterpret_cast<const char* const*>(bytes)
                                                : reinterpret_cast<const char*>(bytes);
        return std::memcmp(text, sp::kCollection, kLength) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool hoverNameCallGuarded(const void* stack, void* out)
{
    __try {
        g_api.hoverName(stack, out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool freeGameStringGuarded(void* str)
{
    __try {
        const std::uint64_t capacity = *reinterpret_cast<const std::uint64_t*>(static_cast<std::byte*>(str) + 0x18);
        if (capacity >= 16) {
            g_api.gameDelete(*reinterpret_cast<void**>(str), static_cast<std::size_t>(capacity + 1));
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::atomic<bool> g_loggedAsk{false};
std::atomic<bool> g_loggedCurrent{false};
std::atomic<bool> g_loggedShow{false};

}

void ShulkerPreview::resolveTooltip()
{
    static bool resolved = false;
    if (resolved) {
        return;
    }
    resolved = true;
    Scanner& scanner = Scanner::instance();
    Api api;
    std::byte* const contents = scanner.address(Target::ShulkerContentsText);
    std::byte* const append = scanner.address(Target::ShulkerHoverAppend);

    if (std::byte* at = findInFunction(contents, "48 8D 4D ? E8 ? ? ? ? 48 8D 4D ? 48 89 F2 E8 ? ? ? ?")) {
        api.ctor = reinterpret_cast<CtorFn>(callTarget(at + 4));
        api.load = reinterpret_cast<LoadFn>(callTarget(at + 16));
    }
    if (std::byte* at = findInFunction(contents, "48 8B 01 48 8B 80 ? ? ? ? 48 8D 55")) {
        api.fixupSlot = readDisp32(at + 6);
    }

    if (std::byte* at = findInFunction(append, "4D 8D 7C 24 01 48 8B 0D ? ? ? ? 48 8B 01 48 8B 40 08 4C 89 FA FF 15")) {
        api.allocatorAt = static_cast<void**>(memory::ripTarget(at, 8));
    }

    if (append != nullptr) {
        const std::size_t size = memory::functionSize(append);
        if (size != 0 && memory::isReadable(append, size)) {
            const ScanHit hit = scanRange(std::span<std::byte>(append, size), "48 83 C0 28 48 89 C2 4C 89 C1 E8");
            if (hit.address != nullptr) {
                api.gameDelete = reinterpret_cast<DeleteFn>(callTarget(hit.address + 10));
            }
        }
    }

    api.tagHash = scanner.addressAs<TagHashFn>(Target::CompoundTagHash);
    api.hoverName = scanner.addressAs<HoverNameFn>(Target::ItemStackHoverName);
    auto slotOk = [](int slot) { return slot > 0 && slot < 0x1000 && slot % 8 == 0; };
    api.ready = api.ctor != nullptr && api.load != nullptr && slotOk(api.fixupSlot)
                && api.allocatorAt != nullptr && api.gameDelete != nullptr;
    g_api = api;
    gamestring::configure(api.allocatorAt, api.gameDelete);
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    auto rva = [base](const void* p) { return p != nullptr ? reinterpret_cast<std::uintptr_t>(p) - base : 0; };
    log().info(L"ShulkerPreview: tooltip parts {} (ctor {:#x} load {:#x} fixup +{:#x} alloc {:#x} delete {:#x} "
               L"tag hash {:#x} hover name {:#x})",
               api.ready ? L"ready" : L"NOT usable", rva(reinterpret_cast<const void*>(api.ctor)),
               rva(reinterpret_cast<const void*>(api.load)), api.fixupSlot, rva(api.allocatorAt),
               rva(reinterpret_cast<const void*>(api.gameDelete)),
               rva(reinterpret_cast<const void*>(api.tagHash)), rva(reinterpret_cast<const void*>(api.hoverName)));
}

bool ShulkerPreview::tooltipAvailable() const
{

    return g_api.ready && !g_api.broken && Scanner::instance().found(Target::ShulkerContentsText)
           && Scanner::instance().found(Target::ContainerGetItem)
           && cui::available() && cui::collectionBindingsAvailable() && !writes::blocked("ShulkerPreview:tooltip");
}

void ShulkerPreview::freeSetLocked(ContentSet& set)
{
    for (int i = 0; i < kSlots; ++i) {
        const auto at = static_cast<std::size_t>(i);
        if (!set.live[at]) {
            continue;
        }
        if (!destroyStackGuarded(set.elems[at].bytes)) {
            g_api.broken = true;
            log().error(L"ShulkerPreview: destroying a preview item faulted; the tooltip preview is turned off");
        }
        set.live[at] = false;
    }
    if (set.id != 0 && m_currentSet == set.id) {
        m_currentSet = 0;
    }
    set.id = 0;
    set.key.clear();
    set.usedAt = 0;
}

void ShulkerPreview::freeStacksLocked()
{
    for (ContentSet& set : m_sets) {
        freeSetLocked(set);
    }
    m_slotSets.clear();
    m_currentSet = 0;
}

void ShulkerPreview::freeStacks()
{
    const std::lock_guard<std::mutex> lock(m_stacksLock);
    freeStacksLocked();
}

ShulkerPreview::ContentSet* ShulkerPreview::findSetLocked(std::uint32_t id)
{
    if (id == 0) {
        return nullptr;
    }
    for (ContentSet& set : m_sets) {
        if (set.id == id) {
            return &set;
        }
    }
    return nullptr;
}

bool ShulkerPreview::contentsKey(const std::vector<containerui::NbtItem>& items, std::string& key)
{
    key.clear();
    key.reserve(items.size() * 48);
    for (const cui::NbtItem& one : items) {
        key += std::to_string(one.slot) + ':' + one.name + ':' + std::to_string(one.count) + ':'
               + std::to_string(one.aux) + (one.enchanted ? "e" : "");
        std::uint64_t hash = 0;
        if (g_api.tagHash == nullptr || one.elem == nullptr || !tagHashGuarded(one.elem, hash)) {
            notice::failOnce("ShulkerPreview.tagHash",
                             std::format(L"ShulkerPreview: the NBT content hash could not be used (function {}); the "
                                         L"contents are not drawn in tooltips",
                                         g_api.tagHash != nullptr ? L"found" : L"missing"),
                             "ShulkerPreview is not drawing tooltip contents: the item data could not be hashed");
            return false;
        }
        key += '#' + std::to_string(hash);
        key += ';';
    }
    return true;
}

ShulkerPreview::ContentSet* ShulkerPreview::acquireSetLocked(const std::string& key,
                                                            const std::vector<containerui::NbtItem>& items)
{

    for (ContentSet& set : m_sets) {
        if (set.id != 0 && set.key == key) {
            set.usedAt = ++m_setUseSeq;
            return &set;
        }
    }

    ContentSet* victim = &m_sets[0];
    for (ContentSet& set : m_sets) {
        if (set.id == 0) {
            victim = &set;
            break;
        }
        if (set.usedAt < victim->usedAt) {
            victim = &set;
        }
    }
    freeSetLocked(*victim);
    int built = 0;
    for (const cui::NbtItem& one : items) {
        if (one.slot < 0 || one.slot >= kSlots || one.elem == nullptr || victim->live[static_cast<std::size_t>(one.slot)]) {
            continue;
        }
        const auto at = static_cast<std::size_t>(one.slot);
        if (!buildStackGuarded(victim->elems[at].bytes, one.elem)) {
            g_api.broken = true;
            log().error(L"ShulkerPreview: building a preview item faulted; the tooltip preview is turned off");

            victim->live.fill(false);
            return nullptr;
        }
        victim->live[at] = true;
        ++built;
    }
    if (built == 0) {
        return nullptr;
    }
    victim->id = m_nextSetId++;
    if (m_nextSetId == 0) {
        m_nextSetId = 1;
    }
    victim->key = key;
    victim->usedAt = ++m_setUseSeq;
    return victim;
}

std::uint32_t ShulkerPreview::setForStackLocked(const void* stack)
{
    if (stack == nullptr || cui::isEmpty(stack)) {
        return 0;
    }
    const void* const root = cui::userDataOf(stack);
    if (root == nullptr) {
        return 0;
    }
    const void* first = nullptr;
    const void* last = nullptr;
    if (!cui::itemsListBounds(root, first, last)) {
        return 0;
    }
    const SlotKey key{reinterpret_cast<std::uintptr_t>(stack), reinterpret_cast<std::uintptr_t>(root),
                      reinterpret_cast<std::uintptr_t>(first), reinterpret_cast<std::uintptr_t>(last)};
    if (const auto it = m_slotSets.find(key); it != m_slotSets.end()) {
        if (it->second == 0 || findSetLocked(it->second) != nullptr) {
            return it->second;
        }
        m_slotSets.erase(it);
    }
    std::uint32_t id = 0;

    if (cui::itemName(stack).find("shulker_box") != std::string::npos) {
        std::vector<cui::NbtItem> items;
        std::string contents;
        if (cui::nbtItemsOfTag(root, items) && !items.empty() && contentsKey(items, contents)) {
            if (const ContentSet* const set = acquireSetLocked(contents, items)) {
                id = set->id;
            }
        }
    }
    if (g_api.broken) {
        return 0;
    }
    if (m_slotSets.size() >= 256) {
        m_slotSets.clear();
    }
    m_slotSets.emplace(key, id);
    return id;
}

int ShulkerPreview::currentSlot(void* ctrl, const std::string& coll, int index, std::uintptr_t)
{
    ShulkerPreview& self = instance();
    if (!self.tooltipAvailable()) {
        return 0;
    }
    const void* const stack = cui::screenStackOf(ctrl, coll, index);
    std::uint32_t id = 0;
    {
        const std::lock_guard<std::mutex> lock(self.m_stacksLock);
        id = self.setForStackLocked(stack);
        self.m_currentSet = id;
        self.m_currentTick = self.m_tick;
        if (ContentSet* const set = self.findSetLocked(id)) {
            set->usedAt = ++self.m_setUseSeq;
        }
    }

    const HoveredBox box{ctrl, coll, index};
    if (box != self.m_currentBox) {
        self.m_currentBox = box;
        self.select(-1);
    }
    if (id != 0 && (id != self.m_publishedSet || box != self.m_publishedBox)) {
        self.publishCurrent(stack, id);
        self.m_publishedBox = box;
    }
    if (id != 0 && !g_loggedCurrent.exchange(true)) {
        log().info(L"ShulkerPreview: the hovered cell {} #{} holds contents set {}", toUtf16(coll), index, id);
    }
    return static_cast<int>(id);
}

bool ShulkerPreview::gridVisible(std::uintptr_t)
{
    ShulkerPreview& self = instance();
    if (!self.wantPreview() || !self.tooltipAvailable()) {
        return false;
    }
    std::uint32_t current = 0;
    {
        const std::lock_guard<std::mutex> lock(self.m_stacksLock);
        current = self.m_currentSet;
    }
    if (current == 0 || self.m_tick > self.m_currentTick + 1) {
        return false;
    }
    if (!g_loggedShow.exchange(true)) {
        log().info(L"ShulkerPreview: showing the contents grid of set {}", current);
    }
    return true;
}

bool ShulkerPreview::normalTooltip(std::uintptr_t arg)
{
    return !gridVisible(arg);
}

namespace {

std::string displayNameOf(const void* stack)
{
    std::string name;
    if (g_api.hoverName == nullptr || stack == nullptr) {
        return name;
    }
    alignas(8) std::byte str[32]{};
    if (!hoverNameCallGuarded(stack, str)) {
        g_api.hoverName = nullptr;
        log().error(L"ShulkerPreview: reading an item's display name faulted; the names stay empty");
        return name;
    }
    gamestring::read(str, name);
    if (!freeGameStringGuarded(str)) {
        log().error(L"ShulkerPreview: freeing an item's display name faulted");
    }
    return name;
}
}

void ShulkerPreview::publishCurrent(const void* stack, std::uint32_t id)
{
    m_publishedSet = id;
    const std::string name = displayNameOf(stack);
    cui::writePersistentText(sp::kTextSlotName, name.data(), name.size());
}

bool ShulkerPreview::ownsWheel() const
{
    return gridVisible(0);
}

bool ShulkerPreview::cellSelected(std::uintptr_t index)
{
    const ShulkerPreview& self = instance();
    return self.m_selected >= 0 && static_cast<std::uintptr_t>(self.m_selected) == index && self.m_selectedSet != 0
           && gridVisible(0);
}

bool ShulkerPreview::hasSelected(std::uintptr_t)
{
    const ShulkerPreview& self = instance();
    return self.m_selected >= 0 && self.m_selectedSet != 0 && gridVisible(0);
}

void ShulkerPreview::select(int slot)
{
    m_selected = slot;
    std::string name;
    if (slot >= 0) {
        const std::lock_guard<std::mutex> lock(m_stacksLock);
        if (const ContentSet* const set = findSetLocked(m_selectedSet);
            set != nullptr && set->live[static_cast<std::size_t>(slot)]) {
            name = displayNameOf(set->elems[static_cast<std::size_t>(slot)].bytes);
        }
    }
    cui::writePersistentText(sp::kTextSlotSelectedName, name.data(), name.size());
}

void ShulkerPreview::consumeWheel()
{
    const auto& buttons = GameButtons::instance();
    const std::uint64_t left = buttons.buttonPressSeq(m_wheelLeftButton);
    const std::uint64_t right = buttons.buttonPressSeq(m_wheelRightButton);
    const int notches = gamebuttonlogic::wheelNotches(left, m_wheelLeftSeen, right, m_wheelRightSeen);
    m_wheelLeftSeen = left;
    m_wheelRightSeen = right;
    std::uint32_t current = 0;
    std::array<bool, kSlots> live{};
    {
        const std::lock_guard<std::mutex> lock(m_stacksLock);
        current = m_currentSet;
        if (const ContentSet* const set = findSetLocked(current)) {
            live = set->live;
        }
    }

    if (!gridVisible(0) || current != m_selectedSet || m_currentBox != m_selectedBox) {
        m_selectedSet = gridVisible(0) ? current : 0;
        m_selectedBox = m_currentBox;
        if (m_selected >= 0) {
            select(-1);
        }
        return;
    }
    if (notches == 0) {
        return;
    }

    const int step = notches < 0 ? 1 : -1;
    int slot = m_selected;
    for (int n = 0; n < std::abs(notches); ++n) {
        for (int tries = 0; tries < kSlots; ++tries) {
            slot = slot < 0 ? (step > 0 ? 0 : kSlots - 1) : (slot + step + kSlots) % kSlots;
            if (live[static_cast<std::size_t>(slot)]) {
                break;
            }
        }
    }
    if (slot >= 0 && live[static_cast<std::size_t>(slot)] && slot != m_selected) {
        select(slot);
    }
}

const void* ShulkerPreview::stackFor(const void* collectionName, int index)
{
    if (collectionName == nullptr || index < 0 || index >= kSlots || !isPreviewCollection(collectionName)) {
        return nullptr;
    }
    ShulkerPreview& self = instance();
    if (!g_loggedAsk.exchange(true)) {
        log().info(L"ShulkerPreview: the screen asked for {} (answering with the hovered box's contents)",
                   toUtf16(std::string(sp::kCollection)));
    }
    if (!self.tooltipAvailable()) {
        return nullptr;
    }
    const std::lock_guard<std::mutex> lock(self.m_stacksLock);
    const ContentSet* const set = self.findSetLocked(self.m_currentSet);
    if (set == nullptr || !set->live[static_cast<std::size_t>(index)]) {
        return nullptr;
    }
    return set->elems[static_cast<std::size_t>(index)].bytes;
}

}
