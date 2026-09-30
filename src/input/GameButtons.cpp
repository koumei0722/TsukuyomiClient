#include "input/GameButtons.h"

#include "core/Logger.h"
#include "hooks/Detours.h"
#include "hooks/HookManager.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"
#include "game/ClientChat.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <string>
#include <typeinfo>

namespace tsukuyomi {
namespace {

using ActionNameFn = void* (__fastcall*)(void*, int);
using FindKeymapFn = void* (__fastcall*)(void*, const void*);
using BindActionFn = void (__fastcall*)(void*, void*, void*, const void*, int, bool);
using RegisterDownFn = void (__fastcall*)(void*, const void*, const void*, bool);
using InputUpdateFn = void (__fastcall*)(void*, void*, void*, void*, std::uint64_t);
using RebuildFn = void (__fastcall*)(void*);
using MappingFactoryFn = void* (__fastcall*)(void*);

ActionNameFn g_actionName = nullptr;
FindKeymapFn g_findKeymap = nullptr;
BindActionFn g_bindAction = nullptr;
RegisterDownFn g_registerDown = nullptr;
RegisterDownFn g_registerUp = nullptr;
InputUpdateFn g_inputUpdate = nullptr;
RebuildFn g_rebuild = nullptr;
std::size_t g_factorySlot = 0;
std::size_t g_templateTable = 0;
std::size_t g_activeTable = 0;
std::size_t g_chordField = 0;
std::size_t g_factoryField = 0;

struct KeymapRow {
    std::string name;
    int* begin = nullptr;
    int* end = nullptr;
    int* cap = nullptr;
    std::uint64_t reserved = 0;
};
static_assert(sizeof(KeymapRow) == 0x40);
struct ChordString { unsigned char bytes[32]{}; };
struct ChordEntry {
    ChordString output;
    ChordString* begin = nullptr;
    ChordString* end = nullptr;
    ChordString* cap = nullptr;
    float weight = 0.0f;
    int padding = 0;
};
struct ChordVector { ChordEntry* begin; ChordEntry* end; ChordEntry* cap; };
static_assert(sizeof(ChordEntry) == 0x40);

bool makeChordString(ChordString& out, const std::string& value)
{
    const std::size_t length = value.size();
    const std::size_t capacity = length < 16 ? 15 : length;
    if (length < 16) {
        std::memcpy(out.bytes, value.c_str(), length + 1);
    } else {
        void* data = hooks::callGameAllocate(length + 1);
        if (!data) return false;
        std::memcpy(data, value.c_str(), length + 1);
        std::memcpy(out.bytes, &data, sizeof(data));
    }
    std::memcpy(out.bytes + 0x10, &length, sizeof(length));
    std::memcpy(out.bytes + 0x18, &capacity, sizeof(capacity));
    return true;
}

void freeChordString(ChordString& value)
{
    std::size_t capacity = 0;
    std::memcpy(&capacity, value.bytes + 0x18, sizeof(capacity));
    if (capacity >= 16) {
        void* data = nullptr;
        std::memcpy(&data, value.bytes, sizeof(data));
        std::free(data);
    }
}

void freeNewChord(ChordEntry& entry)
{
    freeChordString(entry.output);
    for (ChordString* it = entry.begin; it && it < entry.end; ++it) freeChordString(*it);
    std::free(entry.begin);
}

bool callMappingFactory(void* client, void*& factory)
{
    void* vtable = nullptr;
    void* function = nullptr;
    if (!memory::copyGuarded(client, &vtable, sizeof(vtable)) || !memory::inGameModule(vtable)
        || !memory::copyGuarded(static_cast<const unsigned char*>(vtable) + g_factorySlot,
                                &function, sizeof(function))
        || !memory::inGameModule(function) || !memory::isExecutable(function, 1)) return false;
    void* owner = nullptr;
    __try {
        owner = reinterpret_cast<MappingFactoryFn>(function)(client);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return owner != nullptr
        && memory::copyGuarded(static_cast<const unsigned char*>(owner) + g_factoryField, &factory, sizeof(factory))
        && memory::plausiblePointer(factory);
}

bool readChordString(const ChordString& value, std::string& out)
{
    std::size_t length = 0;
    std::size_t capacity = 0;
    std::memcpy(&length, value.bytes + 0x10, sizeof(length));
    std::memcpy(&capacity, value.bytes + 0x18, sizeof(capacity));
    if (capacity < length || (capacity < 16 && capacity != 15) || length > out.max_size()) return false;
    if (length == 0) { out.clear(); return true; }
    if (capacity < 16) {
        out.assign(reinterpret_cast<const char*>(value.bytes), length);
    } else {
        const void* heap = nullptr;
        std::memcpy(&heap, value.bytes, sizeof(heap));
        if (!memory::isReadable(heap, length)) return false;
        out.resize(length);
        if (!memory::copyGuarded(heap, out.data(), length)) return false;
    }
    return true;
}

struct MappingNode { unsigned char* value; std::string name; };
struct ChordTable { ChordVector* vector; ChordVector old; std::vector<ChordEntry> entries; std::vector<gamebuttonlogic::ChordSpec> specs; };

bool readMappingTable(void* factory, std::size_t offset, std::vector<MappingNode>& nodes,
                      bool& normal, const wchar_t*& reason)
{
    const auto* table = static_cast<const unsigned char*>(factory) + offset;
    float load = 0;
    void* head = nullptr;
    std::size_t size = 0;
    if (!memory::copyGuarded(table, &load, sizeof(load)) || load != 1.0f) {
        reason = L"an input mapping table has an invalid load factor";
        return false;
    }
    if (!memory::copyGuarded(table + 8, &head, sizeof(head))
        || !memory::copyGuarded(table + 0x10, &size, sizeof(size))
        || size < 1 || size > 64 || !memory::plausiblePointer(head)) {
        reason = L"an input mapping table has an invalid head or count";
        return false;
    }
    void* node = nullptr;
    if (!memory::copyGuarded(head, &node, sizeof(node))) {
        reason = L"an input mapping list sentinel is unreadable";
        return false;
    }
    for (std::size_t i = 0; i < size; ++i) {
        if (!memory::plausiblePointer(node) || node == head) {
            reason = L"an input mapping list ended before its element count";
            return false;
        }
        ChordString raw{};
        if (!memory::copyGuarded(static_cast<const unsigned char*>(node) + 0x10, &raw, sizeof(raw))) {
            reason = L"an input mapping node name is unreadable";
            return false;
        }
        MappingNode item{static_cast<unsigned char*>(node) + 0x30, {}};
        if (!readChordString(raw, item.name)) {
            reason = L"an input mapping node name is invalid";
            return false;
        }
        if (item.name == "gamePlayNormal") normal = true;
        nodes.push_back(std::move(item));
        if (!memory::copyGuarded(node, &node, sizeof(node))) {
            reason = L"an input mapping list link is unreadable";
            return false;
        }
    }
    if (node != head) reason = L"an input mapping list does not return to its sentinel";
    return node == head;
}

bool readChordTable(ChordVector* address, ChordTable& table, const wchar_t*& reason)
{
    table.vector = address;
    if (!memory::copyGuarded(address, &table.old, sizeof(table.old))) {
        reason = L"a gameplay chord vector is unreadable";
        return false;
    }
    const auto begin = reinterpret_cast<std::uintptr_t>(table.old.begin);
    const auto end = reinterpret_cast<std::uintptr_t>(table.old.end);
    const auto cap = reinterpret_cast<std::uintptr_t>(table.old.cap);
    if ((!begin && (end || cap)) || (begin && (end < begin || cap < end || (end - begin) % sizeof(ChordEntry)))) {
        reason = L"a gameplay chord vector has an invalid range";
        return false;
    }
    const std::size_t bytes = end - begin;
    if (bytes >= 0x1000) {
        reason = L"a gameplay chord vector is too large";
        return false;
    }
    table.entries.resize(bytes / sizeof(ChordEntry));
    if (bytes && !memory::copyGuarded(table.old.begin, table.entries.data(), bytes)) {
        reason = L"gameplay chord entries are unreadable";
        return false;
    }
    for (const ChordEntry& entry : table.entries) {
        gamebuttonlogic::ChordSpec spec;
        if (!readChordString(entry.output, spec.output)) {
            reason = L"a gameplay chord output name is invalid";
            return false;
        }
        const auto first = reinterpret_cast<std::uintptr_t>(entry.begin);
        const auto last = reinterpret_cast<std::uintptr_t>(entry.end);
        const auto limit = reinterpret_cast<std::uintptr_t>(entry.cap);
        if ((!first && (last || limit)) || (first && (last < first || limit < last || (last - first) % sizeof(ChordString)
            || (last != first && !memory::isReadable(entry.begin, last - first))))) {
            reason = L"a gameplay chord input vector has an invalid range";
            return false;
        }
        for (std::size_t i = 0; i < (last - first) / sizeof(ChordString); ++i) {
            ChordString raw{};
            if (!memory::copyGuarded(entry.begin + i, &raw, sizeof(raw))) {
                reason = L"a gameplay chord input is unreadable";
                return false;
            }
            std::string name;
            if (!readChordString(raw, name)) {
                reason = L"a gameplay chord input name is invalid";
                return false;
            }
            spec.inputs.push_back(std::move(name));
        }
        table.specs.push_back(std::move(spec));
    }
    if (!memory::isWritable(address, sizeof(ChordVector))) {
        reason = L"a gameplay chord vector is not writable";
        return false;
    }
    return true;
}

bool writeChordTable(ChordTable& table, const std::vector<gamebuttonlogic::ChordSpec>& desired)
{
    const auto next = gamebuttonlogic::reorderChords(table.specs, desired);
    if (next == table.specs) return true;
    const std::size_t retained = next.size() - desired.size();
    const std::size_t bytes = next.size() * sizeof(ChordEntry);
    if (bytes >= 0x1000) return false;
    auto* fresh = bytes ? static_cast<ChordEntry*>(hooks::callGameAllocate(bytes)) : nullptr;
    if (bytes && !fresh) return false;
    if (bytes) std::memset(fresh, 0, bytes);
    std::size_t kept = 0;
    for (std::size_t i = 0; i < table.entries.size(); ++i) {
        if (!table.specs[i].ours()) {
            std::memcpy(fresh + kept++, &table.entries[i], sizeof(ChordEntry));
        }
    }
    std::size_t made = 0;
    bool okay = true;
    for (const auto& spec : desired) {
        ChordEntry& entry = fresh[retained + made++];
        if (!makeChordString(entry.output, spec.output)) { okay = false; break; }
        entry.begin = static_cast<ChordString*>(hooks::callGameAllocate(spec.inputs.size() * sizeof(ChordString)));
        if (!entry.begin) { okay = false; break; }
        std::memset(entry.begin, 0, spec.inputs.size() * sizeof(ChordString));
        entry.end = entry.begin;
        entry.cap = entry.begin + spec.inputs.size();
        for (const auto& name : spec.inputs) {
            if (!makeChordString(*entry.end, name)) { okay = false; break; }
            ++entry.end;
        }
        if (!okay) break;
    }
    const ChordVector replacement{fresh, fresh ? fresh + next.size() : nullptr, fresh ? fresh + next.size() : nullptr};
    if (okay) okay = memory::writeGuarded(table.vector, &replacement, sizeof(replacement));
    if (!okay) {
        for (std::size_t i = 0; i < made; ++i) freeNewChord(fresh[retained + i]);
        std::free(fresh);
        return false;
    }
    for (std::size_t i = 0; i < table.entries.size(); ++i) {
        if (table.specs[i].ours()) freeNewChord(table.entries[i]);
    }
    std::free(table.old.begin);
    return true;
}
std::array<KeymapRow, gamebuttonlogic::kMaxSlots> g_rows;
std::array<int, gamebuttonlogic::kMaxSlots> g_rowKeys{};
std::array<std::string, gamebuttonlogic::kMaxSlots> g_buttonNames;
std::array<const char*, gamebuttonlogic::kMaxSlots> g_outputOverride{};
std::array<KeymapRow, gamebuttonlogic::kMaxInputRows> g_inputRows;
std::array<int, gamebuttonlogic::kMaxInputRows> g_inputKeys{};
std::array<int, gamebuttonlogic::kMaxInputRows> g_inputVk{};
std::array<std::string, gamebuttonlogic::kMaxInputRows> g_inputButtonNames;
std::array<std::array<char, 16>, gamebuttonlogic::kMaxInputRows> g_inputKeyNames{};

constexpr char kKeyPrefix[] = "key.tk.hk";
constexpr char kButtonPrefix[] = "button.tk.hk";
constexpr char kInputKeyPrefix[] = "key.tk.kb";
constexpr char kInputButtonPrefix[] = "button.tk.kb";
constexpr char kMarker[] = "button.copy_coordinates";

std::string slotName(const char* prefix, int slot)
{
    std::string out(prefix);
    out.push_back(static_cast<char>('0' + slot / 10));
    out.push_back(static_cast<char>('0' + slot % 10));
    return out;
}

bool gameStringEquals(const void* object, const char* expected, std::size_t length)
{
    if (!object) return false;
    struct Head { char text[16]; std::size_t size; std::size_t capacity; } head{};
    if (!memory::copyGuarded(object, &head, sizeof(head)) || head.size != length) return false;
    char buffer[32]{};
    if (length > sizeof(buffer)) return false;
    if (head.capacity < 16) {
        std::memcpy(buffer, head.text, length);
    } else {
        const void* heap = nullptr;
        std::memcpy(&heap, head.text, sizeof(heap));
        if (!memory::copyGuarded(heap, buffer, length)) return false;
    }
    return std::memcmp(buffer, expected, length) == 0;
}

std::array<std::array<char, 16>, gamebuttonlogic::kMaxSlots> g_keyNames{};

int keySlot(const void* object, const char* prefix, int limit)
{
    constexpr std::size_t kPrefix = sizeof(kKeyPrefix) - 1;
    constexpr std::size_t kLength = kPrefix + 2;
    struct Head { char text[16]; std::size_t size; std::size_t capacity; } head{};
    if (!object || !memory::copyGuarded(object, &head, sizeof(head)) || head.size != kLength || head.capacity >= 16) {
        return -1;
    }
    if (std::memcmp(head.text, prefix, kPrefix) != 0) return -1;
    const char tens = head.text[kPrefix];
    const char ones = head.text[kPrefix + 1];
    if (tens < '0' || tens > '9' || ones < '0' || ones > '9') return -1;
    const int slot = (tens - '0') * 10 + (ones - '0');
    return slot < limit ? slot : -1;
}

bool mouseKey(int key)
{
    return key == 0;
}

bool callRegister(RegisterDownFn target, void* handler, const std::string* name, const void* callback)
{
    __try {
        target(handler, name, callback, false);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool callRebuild(void* client)
{
    void* fake[2] = {nullptr, client};
    __try {
        g_rebuild(fake);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool makeCallbackPage(void*& out)
{
    auto* page = static_cast<unsigned char*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT,
                                                          PAGE_EXECUTE_READWRITE));
    if (!page) return false;
    const void* destination = reinterpret_cast<const void*>(&onGameButton);
    std::memcpy(page, &destination, sizeof(destination));
    const void* infoVptr = nullptr;
    if (!memory::copyGuarded(&typeid(int), &infoVptr, sizeof(infoVptr))) return false;
    std::memcpy(page + 0x08, &infoVptr, sizeof(infoVptr));
    constexpr char kTypeName[] = ".?AUTsukuyomiButton@@";
    std::memcpy(page + 0x18, kTypeName, sizeof(kTypeName));
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(page);
    constexpr std::uintptr_t kMethods[7] = {0x80, 0x80, 0xA0, 0xC0, 0xD0, 0xE0, 0xD0};
    for (int i = 0; i < 7; ++i) {
        const std::uintptr_t method = base + kMethods[i];
        std::memcpy(page + 0x40 + i * 8, &method, 8);
    }
    constexpr unsigned char kCopy[] = {0x48,0x8B,0x01,0x48,0x89,0x02,0x48,0x8B,0x41,0x08,0x48,0x89,0x42,0x08,0x48,0x89,0xD0,0xC3};
    constexpr unsigned char kCall[] = {0x48,0x8B,0x05,0x59,0xFF,0xFF,0xFF,0x48,0x85,0xC0,0x74,0x0B,0x4D,0x89,0xC1,0x49,0x89,0xD0,0x8B,0x51,0x08,0xFF,0xE0,0xC3};
    constexpr unsigned char kType[] = {0x48,0x8D,0x05,0x41,0xFF,0xFF,0xFF,0xC3};
    constexpr unsigned char kDestroy[] = {0xC3};
    constexpr unsigned char kAddress[] = {0x48,0x8D,0x41,0x08,0xC3};
    std::memcpy(page + 0x80, kCopy, sizeof(kCopy));
    std::memcpy(page + 0xA0, kCall, sizeof(kCall));
    std::memcpy(page + 0xC0, kType, sizeof(kType));
    std::memcpy(page + 0xD0, kDestroy, sizeof(kDestroy));
    std::memcpy(page + 0xE0, kAddress, sizeof(kAddress));
    FlushInstructionCache(GetCurrentProcess(), page, 0xE0 + sizeof(kAddress));
    out = page;
    return true;
}

}

GameButtons& GameButtons::instance()
{
    static GameButtons buttons;
    return buttons;
}

int GameButtons::attach(std::vector<int> combo, const char* output)
{
    if (combo.size() > gamebuttonlogic::kMaxComboKeys) {
        fail(L"a hotkey has more than 4 keys");
        return -1;
    }
    const std::lock_guard guard(m_attachMutex);
    const int slot = m_allocator.attach();
    if (slot < 0) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) fail(L"all 96 hotkey slots are in use");
        return -1;
    }
    Slot& state = m_slots[slot];
    state.comboCount = static_cast<int>(combo.size());
    std::copy(combo.begin(), combo.end(), state.combo.begin());
    if (combo.size() >= 2) m_anyChords.store(true, std::memory_order_release);
    state.key.store(combo.size() == 1 ? combo[0] : (combo.empty() ? 0 : -1), std::memory_order_relaxed);
    g_outputOverride[slot] = output;
    if (output != nullptr && m_installed.load(std::memory_order_acquire)) g_buttonNames[slot] = output;
    m_count.store(m_allocator.count(), std::memory_order_release);
    m_revision.fetch_add(1, std::memory_order_relaxed);
    m_rowsSeen.store(false, std::memory_order_release);
    m_chordsSynced.store(false, std::memory_order_release);
    return slot;
}

int GameButtons::watchKey(int vk)
{
    const std::lock_guard guard(m_attachMutex);
    for (int i = 0; i < m_allocator.count(); ++i) {
        if (m_keyWatch[i] && m_slots[i].key.load(std::memory_order_relaxed) == vk) return i;
    }
    const int slot = m_allocator.attach();
    if (slot < 0) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) log().error(L"GameButtons: all 96 key slots are in use");
        return -1;
    }
    m_keyWatch[slot] = true;
    m_slots[slot].key.store(vk, std::memory_order_relaxed);
    m_slots[slot].combo[0] = vk;
    m_slots[slot].comboCount = 1;
    m_count.store(m_allocator.count(), std::memory_order_release);
    m_revision.fetch_add(1, std::memory_order_relaxed);
    m_rowsSeen.store(false, std::memory_order_release);
    m_chordsSynced.store(false, std::memory_order_release);
    return slot;
}

int GameButtons::watchButton(const char* buttonName)
{
    if (!buttonName || !*buttonName) return -1;
    const std::lock_guard guard(m_attachMutex);
    const int count = m_watchCount.load(std::memory_order_relaxed);
    for (int i = 0; i < count; ++i) {
        if (m_watches[i].name == buttonName) return i;
    }
    if (count == gamebuttonlogic::kMaxWatchedButtons) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) log().error(L"GameButtons: all 32 button watches are in use");
        return -1;
    }
    m_watches[count].name = buttonName;
    m_watchCount.store(count + 1, std::memory_order_release);
    return count;
}

void GameButtons::setKeys(int slot, std::vector<int> combo)
{
    if (slot < 0 || slot >= m_count.load(std::memory_order_acquire)) return;
    if (combo.size() > gamebuttonlogic::kMaxComboKeys) {
        fail(L"a hotkey has more than 4 keys");
        combo.clear();
    }
    const std::lock_guard guard(m_attachMutex);
    Slot& state = m_slots[slot];
    if (state.comboCount == static_cast<int>(combo.size())
        && std::equal(combo.begin(), combo.end(), state.combo.begin())) return;
    state.comboCount = static_cast<int>(combo.size());
    std::copy(combo.begin(), combo.end(), state.combo.begin());
    bool anyChords = false;
    for (int i = 0; i < m_count.load(std::memory_order_relaxed); ++i) {
        if (m_slots[i].comboCount >= 2) { anyChords = true; break; }
    }
    m_anyChords.store(anyChords, std::memory_order_release);
    state.key.store(combo.size() == 1 ? combo[0] : (combo.empty() ? 0 : -1), std::memory_order_release);
    state.held.store(false, std::memory_order_release);
    m_revision.fetch_add(1, std::memory_order_relaxed);
    m_rowsSeen.store(false, std::memory_order_release);
    m_chordsSynced.store(false, std::memory_order_release);
}

bool GameButtons::hasChords() const
{
    return m_anyChords.load(std::memory_order_acquire);
}

bool GameButtons::ready(int slot) const
{
    return slot >= 0 && slot < m_count.load(std::memory_order_acquire)
        && !mouseKey(m_slots[slot].key.load(std::memory_order_acquire))
        && m_installed.load(std::memory_order_acquire)
        && m_revision.load(std::memory_order_acquire) == m_builtRevision.load(std::memory_order_acquire)
        && m_rowsSeen.load(std::memory_order_acquire)
        && ((!hasChords() && !m_hadChords.load(std::memory_order_acquire))
            || m_chordsSynced.load(std::memory_order_acquire))
        && m_registeredCount.load(std::memory_order_acquire) >= m_count.load(std::memory_order_acquire)
        && !m_stopping.load(std::memory_order_acquire);
}

std::uint64_t GameButtons::pressSeq(int slot) const
{
    return slot < 0 || slot >= m_count.load() ? 0 : m_slots[slot].pressSeq.load(std::memory_order_acquire);
}
std::uint64_t GameButtons::aloneReleaseSeq(int slot) const
{
    return slot < 0 || slot >= m_count.load() ? 0 : m_slots[slot].aloneSeq.load(std::memory_order_acquire);
}
std::uint64_t GameButtons::lastPressMs(int slot) const
{
    return slot < 0 || slot >= m_count.load() ? 0 : m_slots[slot].lastPressMs.load(std::memory_order_acquire);
}
bool GameButtons::held(int slot) const
{
    return slot >= 0 && slot < m_count.load() && m_slots[slot].held.load(std::memory_order_acquire);
}

bool GameButtons::buttonHeld(int index) const
{
    return index >= 0 && index < m_watchCount.load(std::memory_order_acquire)
        && m_watches[index].held.load(std::memory_order_acquire);
}
std::uint64_t GameButtons::buttonPressSeq(int index) const
{
    return index < 0 || index >= m_watchCount.load(std::memory_order_acquire) ? 0
        : m_watches[index].pressSeq.load(std::memory_order_acquire);
}
std::uint64_t GameButtons::buttonLastPressMs(int index) const
{
    return index < 0 || index >= m_watchCount.load(std::memory_order_acquire) ? 0
        : m_watches[index].lastPressMs.load(std::memory_order_acquire);
}
std::uint64_t GameButtons::buttonLastReleaseMs(int index) const
{
    return index < 0 || index >= m_watchCount.load(std::memory_order_acquire) ? 0
        : m_watches[index].lastReleaseMs.load(std::memory_order_acquire);
}

void GameButtons::onCallback(int packed)
{
    if (m_stopping.load(std::memory_order_acquire) || packed < 0) return;
    const auto kind = gamebuttonlogic::callbackKind(packed);
    const int index = gamebuttonlogic::callbackIndex(packed);
    const bool down = kind == gamebuttonlogic::CallbackKind::KeyDown
        || kind == gamebuttonlogic::CallbackKind::ButtonDown
        || kind == gamebuttonlogic::CallbackKind::InputDown;
    if (kind == gamebuttonlogic::CallbackKind::InputDown || kind == gamebuttonlogic::CallbackKind::InputUp) {
        if (index < m_registeredInputCount.load(std::memory_order_acquire)) {
            m_inputHeld[index].store(down, std::memory_order_release);
        }
        return;
    }
    if (kind == gamebuttonlogic::CallbackKind::ButtonDown || kind == gamebuttonlogic::CallbackKind::ButtonUp) {
        if (index >= m_registeredWatchCount.load(std::memory_order_acquire)) return;
        WatchedButton& state = m_watches[index];
        state.held.store(down, std::memory_order_release);
        if (down) {
            state.lastPressMs.store(GetTickCount64(), std::memory_order_release);
            state.pressSeq.fetch_add(1, std::memory_order_release);
        } else {
            state.lastReleaseMs.store(GetTickCount64(), std::memory_order_release);
        }
        return;
    }
    if (index >= m_registeredCount.load(std::memory_order_acquire)) return;
    Slot& state = m_slots[index];
    const std::lock_guard guard(m_attachMutex);
    if (!down) {
        const bool wasHeld = state.held.exchange(false, std::memory_order_acq_rel);
        if (wasHeld && state.comboCount == 1 && state.combo[0] >= 0 && state.combo[0] < 256) {
            const auto now = m_modifierUses[state.combo[0]].load(std::memory_order_acquire);
            state.aloneSeq.store(gamebuttonlogic::aloneAfterRelease(state.modifierAtPress, now,
                state.aloneSeq.load(std::memory_order_relaxed)), std::memory_order_release);
        }
        return;
    }
    if (m_revision.load(std::memory_order_acquire) != m_builtRevision.load(std::memory_order_acquire)) return;
    const int key = state.key.load(std::memory_order_acquire);
    if (state.comboCount == 1 && chordPartsHeld(key)) {
        static std::atomic<int> s_toldSkip{0};
        if (s_toldSkip.fetch_add(1, std::memory_order_relaxed) < 4) {
            log().info(L"GameButtons: button slot {} (key {:#x}) is part of a held chord; not fired alone", index, key);
        }
        return;
    }
    if (state.comboCount == 1 && key >= 0 && key < 256) {
        state.modifierAtPress = m_modifierUses[key].load(std::memory_order_acquire);
    } else if (state.comboCount >= 2) {
        for (int k = 0; k < state.comboCount; ++k) {
            const int part = state.combo[k];
            if (part >= 0 && part < 256) m_modifierUses[part].fetch_add(1, std::memory_order_release);
        }
    }
    static std::atomic<int> s_told{0};
    if (s_told.fetch_add(1, std::memory_order_relaxed) < 8) {
        log().info(L"GameButtons: button slot {} (key {:#x}, chord {}) pressed", index, key,
                   state.comboCount >= 2);
    }
    if (mouseKey(key)) return;
    state.held.store(true, std::memory_order_release);
    state.lastPressMs.store(GetTickCount64(), std::memory_order_release);
    state.pressSeq.fetch_add(1, std::memory_order_release);
}

bool GameButtons::keyHeldForChord(int vk) const
{
    const int key = keys::normalize(vk);
    const int modifier = key == VK_CONTROL ? 0 : key == VK_SHIFT ? 1 : key == VK_MENU ? 2 : -1;
    if (modifier >= 0) return buttonHeld(m_modifierWatch[modifier]);
    for (int j = 0; j < gamebuttonlogic::kMaxInputRows; ++j) {
        if (g_inputVk[j] == key && key != 0) return m_inputHeld[j].load(std::memory_order_acquire);
    }
    return false;
}

bool GameButtons::chordPartsHeld(int key) const
{
    const int normalized = keys::normalize(key);
    const int count = m_count.load(std::memory_order_acquire);
    for (int i = 0; i < count; ++i) {
        const Slot& chord = m_slots[i];
        if (chord.comboCount < 2) continue;
        bool contains = false;
        bool othersHeld = true;
        for (int k = 0; k < chord.comboCount; ++k) {
            const int part = keys::normalize(chord.combo[k]);
            if (part == normalized) {
                contains = true;
            } else if (!keyHeldForChord(part)) {
                othersHeld = false;
            }
        }
        if (contains && othersHeld) return true;
    }
    return false;
}

void GameButtons::onRowAdded()
{
    static std::atomic<bool> told{false};
    if (!told.exchange(true)) {
        log().info(L"GameButtons: added {} hotkey row(s)", m_count.load(std::memory_order_acquire));
    }
    m_rowsSeen.store(true, std::memory_order_release);
    const std::uint64_t revision = m_revision.load(std::memory_order_acquire);
    if (revision == m_appliedRevision.load(std::memory_order_acquire)
        && ((!hasChords() && !m_hadChords.load(std::memory_order_acquire))
            || m_chordsSynced.load(std::memory_order_acquire))) {
        m_builtRevision.store(revision, std::memory_order_release);
    }
}

bool GameButtons::syncChordTables(void* client)
{
    std::vector<gamebuttonlogic::ChordSpec> desired;
    gamebuttonlogic::ChordInputs inputs;
    {
        const std::lock_guard guard(m_attachMutex);
        const int count = m_count.load(std::memory_order_acquire);
        for (int i = 0; i < count; ++i) {
            const Slot& slot = m_slots[i];
            if (slot.comboCount < 2) continue;
            if (!inputs.add(std::span(slot.combo.data(), slot.comboCount))) {
                fail(L"more than 32 distinct chord input keys");
                return false;
            }
        }
        for (int i = 0; i < count; ++i) {
            const Slot& slot = m_slots[i];
            if (slot.comboCount < 2) continue;
            gamebuttonlogic::ChordSpec spec{g_buttonNames[i], {}};
            for (int j = 0; j < slot.comboCount; ++j) {
                std::string name = inputs.buttonName(slot.combo[j]);
                if (name.empty()) { fail(L"a chord input name is missing"); return false; }
                spec.inputs.push_back(std::move(name));
            }
            desired.push_back(std::move(spec));
        }
    }
    void* factory = nullptr;
    if (!callMappingFactory(client, factory)) {
        fail(L"the input mapping factory could not be read or called");
        return false;
    }
    void* factoryVptr = nullptr;
    if (!memory::copyGuarded(factory, &factoryVptr, sizeof(factoryVptr)) || !memory::inGameModule(factoryVptr)) {
        fail(L"the input mapping factory vtable is outside the game");
        return false;
    }
    std::vector<MappingNode> nodes;
    bool normal = false;
    const wchar_t* reason = nullptr;
    if (!readMappingTable(factory, g_templateTable, nodes, normal, reason)
        || !readMappingTable(factory, g_activeTable, nodes, normal, reason) || !normal) {
        fail(reason ? reason : L"gamePlayNormal is missing from the input mapping tables");
        return false;
    }
    std::vector<ChordTable> tables;
    for (const MappingNode& node : nodes) {
        if (node.name.rfind("gamePlay", 0) != 0) continue;
        ChordTable table{};
        if (!readChordTable(reinterpret_cast<ChordVector*>(node.value + g_chordField), table, reason)
            || (gamebuttonlogic::reorderChords(table.specs, desired).size() * sizeof(ChordEntry) >= 0x1000)) {
            fail(reason ? reason : L"a synchronized gameplay chord vector would be too large");
            return false;
        }
        tables.push_back(std::move(table));
    }
    if (tables.empty()) { fail(L"the gameplay mapping nodes are missing"); return false; }
    int changed = 0;
    for (ChordTable& table : tables) {
        const bool differs = gamebuttonlogic::reorderChords(table.specs, desired) != table.specs;
        if (!writeChordTable(table, desired)) {
            fail(L"updating a gameplay chord vector failed");
            return false;
        }
        if (differs) ++changed;
    }
    static int told = 0;
    if (told++ < 3) {
        log().info(L"GameButtons: chord sync: {} chord(s) wanted, {} gameplay table(s), {} changed", desired.size(),
                   tables.size(), changed);
    }
    m_hadChords.store(!desired.empty(), std::memory_order_release);
    m_chordTablesChecked.store(true, std::memory_order_release);
    m_chordsSynced.store(true, std::memory_order_release);
    return true;
}

void GameButtons::onInputUpdate(void* self)
{
    static bool toldFirst = false;
    if (!toldFirst) {
        toldFirst = true;
        log().info(L"GameButtons: the input update is arriving ({} hotkey slot(s))", m_count.load(std::memory_order_acquire));
    }
    void* handler = nullptr;
    if (self && memory::copyGuarded(static_cast<const unsigned char*>(self) + m_handlerOffset,
                                    &handler, sizeof(handler)) && memory::plausiblePointer(handler)) {
        if (handler != m_registeredHandler) {
            m_registeredHandler = handler;
            m_registeredCount.store(0, std::memory_order_release);
            m_registeredWatchCount.store(0, std::memory_order_release);
            m_registeredInputCount.store(0, std::memory_order_release);
            for (Slot& state : m_slots) state.held.store(false, std::memory_order_release);
            for (WatchedButton& state : m_watches) state.held.store(false, std::memory_order_release);
            for (auto& held : m_inputHeld) held.store(false, std::memory_order_release);
        }
        auto registerPair = [this, handler](const std::string& name, int downValue, int upValue) {
            auto add = [this, handler, &name](RegisterDownFn target, int value) {
                alignas(8) unsigned char callback[0x40]{};
                const void* vptr = static_cast<unsigned char*>(m_callbackPage) + 0x40;
                std::memcpy(callback, &vptr, sizeof(vptr));
                std::memcpy(callback + 8, &value, sizeof(value));
                const void* body = callback;
                std::memcpy(callback + 0x38, &body, sizeof(body));
                const std::string copy = name;
                return callRegister(target, handler, &copy, callback);
            };
            return add(g_registerDown, downValue) && add(g_registerUp, upValue);
        };
        const int count = m_count.load(std::memory_order_acquire);
        for (int i = m_registeredCount.load(std::memory_order_relaxed); i < count; ++i) {
            if (!registerPair(g_buttonNames[i],
                    gamebuttonlogic::packCallback(gamebuttonlogic::CallbackKind::KeyDown, i),
                    gamebuttonlogic::packCallback(gamebuttonlogic::CallbackKind::KeyUp, i))) {
                static bool warned = false;
                if (!warned) { warned = true; fail(L"registering the button handlers faulted"); }
                break;
            }
            m_registeredCount.store(i + 1, std::memory_order_release);
        }
        const int watchCount = m_watchCount.load(std::memory_order_acquire);
        for (int i = m_registeredWatchCount.load(std::memory_order_relaxed); i < watchCount; ++i) {
            if (!registerPair(m_watches[i].name,
                    gamebuttonlogic::packCallback(gamebuttonlogic::CallbackKind::ButtonDown, i),
                    gamebuttonlogic::packCallback(gamebuttonlogic::CallbackKind::ButtonUp, i))) {
                fail(L"registering the watched button handlers faulted");
                break;
            }
            m_registeredWatchCount.store(i + 1, std::memory_order_release);
        }
        for (int i = m_registeredInputCount.load(std::memory_order_relaxed); i < gamebuttonlogic::kMaxInputRows; ++i) {
            if (!registerPair(g_inputButtonNames[i],
                    gamebuttonlogic::packCallback(gamebuttonlogic::CallbackKind::InputDown, i),
                    gamebuttonlogic::packCallback(gamebuttonlogic::CallbackKind::InputUp, i))) {
                fail(L"registering the chord input handlers faulted");
                break;
            }
            m_registeredInputCount.store(i + 1, std::memory_order_release);
        }
        static bool toldRegistered = false;
        if (!toldRegistered && count > 0 && m_registeredCount.load(std::memory_order_acquire) >= count) {
            toldRegistered = true;
            log().info(L"GameButtons: registered {} button handler(s) on the input handler {:#x} (field +{:#x})", count,
                       reinterpret_cast<std::uintptr_t>(handler), m_handlerOffset);
        }
    } else {
        const bool changed = m_registeredHandler != nullptr;
        m_registeredHandler = nullptr;
        m_registeredCount.store(0, std::memory_order_release);
        m_registeredWatchCount.store(0, std::memory_order_release);
        m_registeredInputCount.store(0, std::memory_order_release);
        if (changed) {
            for (Slot& state : m_slots) state.held.store(false, std::memory_order_release);
            for (WatchedButton& state : m_watches) state.held.store(false, std::memory_order_release);
            for (auto& held : m_inputHeld) held.store(false, std::memory_order_release);
        }
    }

    const std::uint64_t revisionBeforeApply = m_revision.load(std::memory_order_acquire);
    const int count = m_count.load(std::memory_order_acquire);
    for (int i = 0; i < count; ++i) {
        Slot& state = m_slots[i];
        const int key = state.key.load(std::memory_order_acquire);
        {
            const std::lock_guard guard(m_attachMutex);
            g_rowKeys[i] = gamebuttonlogic::gameKeyCode(key);
            g_rows[i].end = g_rows[i].begin + (m_slots[i].comboCount == 1 && !mouseKey(key) ? 1 : 0);
        }
    }
    if (revisionBeforeApply == m_revision.load(std::memory_order_acquire)) {
        m_appliedRevision.store(revisionBeforeApply, std::memory_order_release);
    }

    constexpr int kRebuildAttempts = 3;
    static int s_rebuildFailures = 0;
    const std::uint64_t now = GetTickCount64();
    if (m_revision.load(std::memory_order_acquire) != m_builtRevision.load(std::memory_order_acquire) && g_rebuild
        && s_rebuildFailures < kRebuildAttempts && now - m_lastRebuildMs >= 500) {
        void* client = hooks::gameClientInstance();
        if (memory::plausiblePointer(client)) {
            const std::uint64_t revision = m_revision.load(std::memory_order_acquire);
            m_lastRebuildMs = now;
            m_rowsSeen.store(false, std::memory_order_release);
            const bool synced = (m_chordTablesChecked.load(std::memory_order_acquire)
                && !hasChords() && !m_hadChords.load(std::memory_order_acquire))
                || syncChordTables(client);
            m_chordsSynced.store(synced, std::memory_order_release);
            const bool called = synced && callRebuild(client);
            static int s_toldRebuild = 0;
            if (s_toldRebuild++ < 3) {
                log().info(L"GameButtons: rebuilt the input mappings ({}, rows {})", called ? L"returned" : L"FAULTED",
                           m_rowsSeen.load(std::memory_order_acquire) ? L"added" : L"missing");
            }
            if (called && m_rowsSeen.load(std::memory_order_acquire)
                && revision == m_appliedRevision.load(std::memory_order_acquire)
                && revision == m_revision.load(std::memory_order_acquire)
                && (!hasChords() || m_chordsSynced.load(std::memory_order_acquire))) {
                m_builtRevision.store(revision, std::memory_order_release);
                s_rebuildFailures = 0;
            } else if (++s_rebuildFailures >= kRebuildAttempts) {
                fail(L"rebuilding the input mappings failed 3 times in a row");
            }
        }
    }
}

void GameButtons::install()
{
    const Scanner& scan = Scanner::instance();
    const Target targets[] = {Target::GameButtonActionName, Target::GameButtonFindKeymap,
        Target::GameButtonBindAction, Target::GameButtonRegisterDownSite, Target::GameButtonRegisterUpSite,
        Target::GameButtonInputUpdate, Target::GameButtonRebuild,
        Target::InputMappingFactoryDtorSite, Target::InputMappingChordsField, Target::InputMappingFactoryPtrSite};
    const wchar_t* labels[] = {L"GameButtonActionName", L"GameButtonFindKeymap",
        L"GameButtonBindAction", L"GameButtonRegisterDownSite", L"GameButtonRegisterUpSite", L"GameButtonInputUpdate", L"GameButtonRebuild",
        L"InputMappingFactoryDtorSite", L"InputMappingChordsField", L"InputMappingFactoryPtrSite"};
    static_assert(std::size(targets) == std::size(labels));
    for (std::size_t i = 0; i < std::size(targets); ++i) {
        if (!scan.found(targets[i])) {
            static std::wstring reason;
            reason = std::wstring(labels[i]) + L" signature is missing";
            fail(reason.c_str());
            return;
        }
    }
    const auto* site = scan.address(Target::GameButtonRegisterDownSite);
    unsigned char call[5]{};
    if (!memory::copyGuarded(site + 20, call, sizeof(call)) || call[0] != 0xE8) {
        fail(L"the button-down registration call site changed");
        return;
    }
    g_registerDown = reinterpret_cast<RegisterDownFn>(memory::ripTarget(site + 20, 1));
    const auto* upSite = scan.address(Target::GameButtonRegisterUpSite);
    unsigned char upCall[5]{};
    if (!memory::copyGuarded(upSite + 20, upCall, sizeof(upCall)) || upCall[0] != 0xE8) {
        fail(L"the button-up registration call site changed");
        return;
    }
    g_registerUp = reinterpret_cast<RegisterDownFn>(memory::ripTarget(upSite + 20, 1));
    unsigned char downHead[16]{};
    unsigned char upHead[16]{};
    if (!g_registerDown || !g_registerUp
        || !memory::copyGuarded(reinterpret_cast<const void*>(g_registerDown), downHead, sizeof(downHead))
        || !memory::copyGuarded(reinterpret_cast<const void*>(g_registerUp), upHead, sizeof(upHead))
        || std::memcmp(downHead, upHead, sizeof(downHead)) != 0 || g_registerDown == g_registerUp) {
        fail(L"the button-up registration function does not match the button-down one");
        return;
    }
    auto* update = scan.address(Target::GameButtonInputUpdate);
    unsigned char field[4]{};
    if (!g_registerDown || !g_registerUp || !memory::inGameModule(reinterpret_cast<void*>(g_registerDown))
        || !memory::inGameModule(reinterpret_cast<void*>(g_registerUp))
        || !memory::isExecutable(reinterpret_cast<void*>(g_registerUp), 1)
        || !memory::isExecutable(reinterpret_cast<void*>(g_registerDown), 1)
        || !memory::copyGuarded(update + 0x3E, field, sizeof(field))
        || field[0] != 0x4C || field[1] != 0x8B || field[2] != 0x7F) {
        fail(L"the input update layout changed");
        return;
    }
    m_handlerOffset = field[3];
    g_rebuild = scan.addressAs<RebuildFn>(Target::GameButtonRebuild);
    m_modifierWatch[0] = watchButton(gamebuttonlogic::button::control);
    m_modifierWatch[1] = watchButton(gamebuttonlogic::button::shift);
    m_modifierWatch[2] = watchButton(gamebuttonlogic::button::alt);
    const auto* dtor = scan.address(Target::InputMappingFactoryDtorSite);
    const auto* ctor = scan.address(Target::InputMappingChordsField);
    const auto* rebuild = scan.address(Target::GameButtonRebuild);
    unsigned char templateHead = 0;
    unsigned char activeHead = 0;
    std::uint32_t nodeBytes = 0;
    std::int32_t chordField = 0;
    std::int32_t factorySlot = 0;
    const auto* ptrSite = scan.address(Target::InputMappingFactoryPtrSite);
    unsigned char factoryField = 0;
    unsigned char ptrActiveHead = 0;
    if (!memory::copyGuarded(ptrSite + 11, &factoryField, 1) || !memory::copyGuarded(ptrSite + 28, &ptrActiveHead, 1)
        || factoryField == 0 || factoryField % sizeof(void*) != 0) {
        fail(L"the input mapping owner layout changed");
        return;
    }
    g_factoryField = factoryField;
    if (!memory::copyGuarded(dtor + 3, &templateHead, 1)
        || !memory::copyGuarded(dtor + 0x66, &activeHead, 1)
        || !memory::copyGuarded(dtor + 0x12, &nodeBytes, sizeof(nodeBytes))
        || !memory::copyGuarded(ctor + 7, &chordField, sizeof(chordField))
        || !memory::copyGuarded(rebuild + 42, &factorySlot, sizeof(factorySlot))
        || nodeBytes != 0x300 || templateHead < 8 || activeHead < 8
        || templateHead == activeHead || activeHead != ptrActiveHead || chordField < 0 || chordField + sizeof(ChordVector) > 0x2d0
        || factorySlot < 0 || factorySlot % sizeof(void*) != 0) {
        fail(L"the input mapping factory field layout changed");
        return;
    }
    g_templateTable = templateHead - 8;
    g_activeTable = activeHead - 8;
    g_chordField = static_cast<std::size_t>(chordField);
    g_factorySlot = static_cast<std::size_t>(factorySlot);
    for (int i = 0; i < gamebuttonlogic::kMaxSlots; ++i) {
        g_rows[i].name = slotName(kKeyPrefix, i);
        std::memcpy(g_keyNames[i].data(), g_rows[i].name.c_str(), g_rows[i].name.size() + 1);
        g_rows[i].begin = &g_rowKeys[i];
        g_rows[i].end = &g_rowKeys[i];
        g_rows[i].cap = &g_rowKeys[i] + 1;
        g_buttonNames[i] = g_outputOverride[i] != nullptr ? std::string(g_outputOverride[i]) : slotName(kButtonPrefix, i);
    }
    for (int i = 0; i < gamebuttonlogic::kMaxInputRows; ++i) {
        g_inputRows[i].name = slotName(kInputKeyPrefix, i);
        std::memcpy(g_inputKeyNames[i].data(), g_inputRows[i].name.c_str(), g_inputRows[i].name.size() + 1);
        g_inputRows[i].begin = &g_inputKeys[i];
        g_inputRows[i].end = &g_inputKeys[i];
        g_inputRows[i].cap = &g_inputKeys[i] + 1;
        g_inputButtonNames[i] = slotName(kInputButtonPrefix, i);
    }
    if (!makeCallbackPage(m_callbackPage)) {
        fail(L"the callback page could not be allocated");
        return;
    }
    HookManager& hooks = HookManager::instance();
    const bool action = hooks.create(scan.address(Target::GameButtonActionName), &gameButtonActionName,
                                     reinterpret_cast<void**>(&g_actionName), L"GameButtonActionName");
    const bool find = hooks.create(scan.address(Target::GameButtonFindKeymap), &gameButtonFindKeymap,
                                   reinterpret_cast<void**>(&g_findKeymap), L"GameButtonFindKeymap");
    const bool bind = hooks.create(scan.address(Target::GameButtonBindAction), &gameButtonBindAction,
                                   reinterpret_cast<void**>(&g_bindAction), L"GameButtonBindAction");
    const bool input = hooks.create(update, &gameButtonInputUpdate,
                                    reinterpret_cast<void**>(&g_inputUpdate), L"GameButtonInputUpdate");
    if (!(action && find && bind && input)) {
        fail(L"a required hook could not be created");
        return;
    }
    m_installed.store(true, std::memory_order_release);
}

void GameButtons::fail(const wchar_t* reason)
{
    const wchar_t* expected = nullptr;
    if (m_failure.compare_exchange_strong(expected, reason, std::memory_order_acq_rel)) {
        log().error(L"GameButtons: hotkeys are NOT working: {} (END still unloads)", reason);
    }
}

void GameButtons::pumpNotice()
{
    const wchar_t* const reason = m_failure.load(std::memory_order_acquire);
    if (reason == nullptr || m_noticeShown.load(std::memory_order_acquire)) {
        return;
    }
    std::string text = "[Tsukuyomi] Hotkeys are not working: ";
    for (const wchar_t* c = reason; *c != 0; ++c) {
        text.push_back(*c < 0x80 ? static_cast<char>(*c) : '?');
    }
    text += ". See Tsukuyomi.log (END still unloads).";
    if (clientchat::printLocal(text)) {
        m_noticeShown.store(true, std::memory_order_release);
    }
}

void GameButtons::shutdown()
{
    m_stopping.store(true, std::memory_order_release);
    if (m_callbackPage) {
        InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(m_callbackPage), nullptr);
    }
    m_installed.store(false, std::memory_order_release);
}

void __fastcall onGameButton(void*, int packed, const void*, void*)
{
    GameButtons::instance().onCallback(packed);
}

void* __fastcall gameButtonActionName(void* out, int action)
{
    const int slot = action - 0x4000;
    const int input = action - 0x4100;
    if (((slot >= 0 && slot < gamebuttonlogic::kMaxSlots)
        || (input >= 0 && input < gamebuttonlogic::kMaxInputRows)) && out) {
        unsigned char value[32]{};
        std::memcpy(value, slot >= 0 && slot < gamebuttonlogic::kMaxSlots
            ? g_keyNames[slot].data() : g_inputKeyNames[input].data(), 11);
        const std::size_t length = 11;
        const std::size_t capacity = 15;
        std::memcpy(value + 0x10, &length, 8);
        std::memcpy(value + 0x18, &capacity, 8);
        memory::writeGuarded(out, value, sizeof(value));
        return out;
    }
    return g_actionName ? g_actionName(out, action) : out;
}

void* __fastcall gameButtonFindKeymap(void* layout, const void* name)
{
    const int slot = keySlot(name, kKeyPrefix, gamebuttonlogic::kMaxSlots);
    if (slot >= 0) {
        g_rows[slot].name.assign(g_keyNames[slot].data());
        return &g_rows[slot];
    }
    const int input = keySlot(name, kInputKeyPrefix, gamebuttonlogic::kMaxInputRows);
    if (input >= 0) {
        g_inputRows[input].name.assign(g_inputKeyNames[input].data());
        return &g_inputRows[input];
    }
    return g_findKeymap ? g_findKeymap(layout, name) : nullptr;
}

void __fastcall gameButtonBindAction(void* owner, void* table, void* mouse,
                                     const void* name, int action, bool flag)
{
    if (!g_bindAction) return;
    g_bindAction(owner, table, mouse, name, action, flag);
    GameButtons& buttons = GameButtons::instance();
    if (!buttons.m_installed.load(std::memory_order_acquire)
        || buttons.m_stopping.load(std::memory_order_acquire)
        || !gameStringEquals(name, kMarker, sizeof(kMarker) - 1)) return;
    const int count = buttons.m_count.load(std::memory_order_acquire);
    gamebuttonlogic::ChordInputs inputs;
    {
        const std::lock_guard guard(buttons.m_attachMutex);
        for (int i = 0; i < count; ++i) {
            const auto& slot = buttons.m_slots[i];
            if (slot.comboCount >= 2 && !inputs.add(std::span(slot.combo.data(), slot.comboCount))) {
                buttons.fail(L"more than 32 distinct chord input keys");
                return;
            }
        }
    }
    for (int i = 0; i < static_cast<int>(inputs.keys().size()); ++i) {
        g_inputKeys[i] = gamebuttonlogic::gameKeyCode(inputs.keys()[i]);
        g_inputVk[i] = inputs.keys()[i];
        g_inputRows[i].end = g_inputRows[i].begin + 1;
    }
    for (int i = static_cast<int>(inputs.keys().size()); i < gamebuttonlogic::kMaxInputRows; ++i) {
        g_inputRows[i].end = g_inputRows[i].begin;
        g_inputVk[i] = 0;
    }
    for (int i = 0; i < count; ++i) {
        std::string buttonName = g_buttonNames[i];
        g_bindAction(owner, table, mouse, &buttonName, 0x4000 + i, false);
    }
    for (int i = 0; i < static_cast<int>(inputs.keys().size()); ++i) {
        std::string buttonName = g_inputButtonNames[i];
        g_bindAction(owner, table, mouse, &buttonName, 0x4100 + i, false);
    }
    if (count != 0) buttons.onRowAdded();
}

void __fastcall gameButtonInputUpdate(void* self, void* a2, void* a3, void* a4, std::uint64_t a5)
{
    GameButtons& buttons = GameButtons::instance();
    if (buttons.m_installed.load(std::memory_order_acquire)
        && !buttons.m_stopping.load(std::memory_order_acquire)) buttons.onInputUpdate(self);
    if (g_inputUpdate) g_inputUpdate(self, a2, a3, a4, a5);
}

}
