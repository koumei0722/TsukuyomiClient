#include "FriendWorld.h"
#include "BotNet.h"
#include "BotSignatures.h"
#include "NetworkLayout.h"
#include "core/Logger.h"
#include "memory/Memory.h"
#include <Windows.h>
#include <atomic>
#include <cstddef>
#include <cstring>

namespace tsukuyomi::bot::friendworld {
namespace {
std::uint32_t g_iface = 0;
std::uint32_t g_leaveJoinSlot = 0;
std::uint32_t g_holderGetter = 0;
std::uint32_t g_holderField = 0;
std::uint32_t g_finderField = 0;
std::uint32_t g_listField = 0;
std::uint32_t g_headField = 0;
std::uint32_t g_keyField = 0;
bool g_ready = false, g_guarded = false;
std::atomic<std::uint64_t> g_expectId{0}, g_expectUntil{0};
std::atomic<int> g_lookup{0};

constexpr std::size_t kValueScan = 0x600, kVectorMax = 0x2000, kMaxNodes = 256;
constexpr const char* kNoWorld = "-";

std::int32_t disp32(const std::byte* at) { std::int32_t value = 0; std::memcpy(&value, at, 4); return value; }
bool inMainModule(const void* address)
{
    static const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto value = reinterpret_cast<std::uintptr_t>(address);
    return value >= base && value < base + 0x20000000;
}
const std::byte* findIn(const std::byte* from, std::size_t span, std::initializer_list<int> pattern)
{
    std::byte window[0x400];
    if (span > sizeof window || !memory::copyGuarded(from, window, span)) return nullptr;
    const std::size_t length = pattern.size();
    for (std::size_t at = 0; at + length <= span; ++at) {
        std::size_t k = 0;
        for (const int expected : pattern) {
            if (expected >= 0 && std::to_integer<int>(window[at + k]) != expected) break;
            ++k;
        }
        if (k == length) return from + at;
    }
    return nullptr;
}
int filter(unsigned code) { return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH; }

void* readPointer(const void* at)
{
    void* value = nullptr;
    return memory::copyGuarded(at, &value, sizeof value) && memory::plausiblePointer(value) ? value : nullptr;
}
using GetterFn = void*(__fastcall*)(void*);
void* callGetterGuarded(void* game, std::uint32_t slot)
{
    __try {
        const auto* table = *static_cast<const std::byte* const*>(game);
        const auto fn = *reinterpret_cast<const GetterFn*>(table + slot);
        if (!inMainModule(reinterpret_cast<const void*>(fn))) return nullptr;
        return fn(game);
    } __except (filter(GetExceptionCode())) { return nullptr; }
}
bool valueHasNetherNetId(const std::byte* value, std::uint64_t id)
{
    std::uintptr_t words[kValueScan / 8];
    if (!memory::copyGuarded(value, words, sizeof words)) return false;
    for (std::size_t i = 0; i + 2 < std::size(words); ++i) {
        const auto begin = words[i], end = words[i + 1], capacity = words[i + 2];
        if (!memory::plausiblePointer(begin) || end <= begin || end - begin > kVectorMax || capacity < end || (end - begin) % 8 != 0) continue;
        std::uint64_t items[kVectorMax / 8];
        const std::size_t count = (end - begin) / 8;
        if (!memory::copyGuarded(reinterpret_cast<const void*>(begin), items, count * 8)) continue;
        for (std::size_t k = 0; k < count; ++k) if (items[k] == id) return true;
    }
    return false;
}
std::optional<std::string> readString(const std::byte* at)
{
    struct Raw { std::byte bytes[16]; std::size_t size, capacity; } raw{};
    if (!memory::copyGuarded(at, &raw, sizeof raw) || raw.size > 256 || raw.capacity < raw.size) return std::nullopt;
    std::string text(raw.size, '\0');
    const void* source = raw.bytes;
    if (raw.capacity >= 16) { std::memcpy(&source, raw.bytes, sizeof source); }
    if (raw.size && !memory::copyGuarded(source, text.data(), raw.size)) return std::nullopt;
    return text;
}
struct View { const char* data; std::size_t size; };
using JoinFn = void(__fastcall*)(void* self, const View* id);
bool callJoinGuarded(void* self, JoinFn fn, const View* id)
{
    __try { fn(self, id); return true; } __except (filter(GetExceptionCode())) { return false; }
}
}

bool initialize()
{
    auto* const handler = net::site(Site::FriendWorldJoinHandler);
    auto* const joiner = net::site(Site::FriendWorldJoinerCtor);
    auto* const request = net::site(Site::FriendJoinRequestHandle);
    auto* const find = net::site(Site::FriendWorldFind);
    if (!handler || !joiner || !request || !find) return false;
    g_leaveJoinSlot = std::to_integer<std::uint32_t>(handler[0x6b]) + 0x10;
    g_iface = std::to_integer<std::uint32_t>(joiner[7]);
    g_holderGetter = static_cast<std::uint32_t>(disp32(request + 40));
    g_holderField = static_cast<std::uint32_t>(disp32(request + 56));
    g_listField = static_cast<std::uint32_t>(disp32(find + 31));
    const auto* const lookup = find + 40 + disp32(find + 36);
    const auto* add = findIn(request, 0x100, {0x48, 0x81, 0xC6, -1, -1, -1, -1});
    std::byte code[0x180]{};
    const auto fields = inMainModule(lookup) && memory::copyGuarded(lookup, code, sizeof code)
        ? networklayout::readFriendLookup(code) : std::nullopt;
    if (!add || !fields) {
        log().warn(L"Bot: cannot read the game's friend world list; switching in a friend's world is unavailable");
        return false;
    }
    g_finderField = static_cast<std::uint32_t>(disp32(add + 3));
    g_headField = fields->head;
    g_keyField = fields->key;
    const bool sane = g_iface && g_iface < 0x100 && g_leaveJoinSlot == 0x28 && g_holderGetter && g_holderGetter < 0x2000 && g_holderField < 0x400
        && g_finderField < 0x1000 && g_listField < 0x400 && g_headField < 0x400 && g_keyField < 0x40;
    log().info(L"Bot: friend world joiner +{:#x} slot {:#x}; list = [[game vt+{:#x}]+{:#x}]+{:#x}+{:#x}, head +{:#x}, key +{:#x}",
        g_iface, g_leaveJoinSlot, g_holderGetter, g_holderField, g_finderField, g_listField, g_headField, g_keyField);
    if (!sane) { log().warn(L"Bot: the friend world joiner's fields look wrong; switching in a friend's world is unavailable"); return false; }
    g_ready = true;
    return true;
}
bool available() { return g_ready && g_guarded; }
void setGuarded(bool guarded) { g_guarded = guarded; }
namespace {
std::optional<std::string> keyIn(void* list, std::uint64_t netherNetId)
{
    auto* const head = list ? readPointer(static_cast<std::byte*>(list) + g_headField) : nullptr;
    if (!head) return std::nullopt;
    auto* node = readPointer(head);
    for (std::size_t n = 0; node && node != head && n < kMaxNodes; ++n, node = readPointer(node)) {
        const auto* const key = static_cast<const std::byte*>(node) + g_keyField;
        if (valueHasNetherNetId(key + 0x20, netherNetId)) return readString(key);
    }
    return std::nullopt;
}
}
std::optional<std::string> idFor(void* game, std::uint64_t netherNetId)
{
    if (!g_ready || !game || !netherNetId) return std::nullopt;
    auto* const holder = callGetterGuarded(game, g_holderGetter);
    auto* const owner = holder ? readPointer(static_cast<std::byte*>(holder) + g_holderField) : nullptr;
    auto* const list = owner ? readPointer(static_cast<std::byte*>(owner) + g_finderField + g_listField) : nullptr;
    return keyIn(list, netherNetId);
}
void expectRejoin(std::uint64_t netherNetId, std::uint64_t untilMs)
{
    g_lookup.store(0);
    g_expectUntil.store(untilMs);
    g_expectId.store(netherNetId);
}
void cancelRejoin() { g_expectId.store(0); }
Lookup takeLookup() { return static_cast<Lookup>(g_lookup.exchange(0)); }
void __fastcall onFind(FindFn original, void* finder, const std::string* id, void* callback)
{
    const auto expected = g_expectId.exchange(0);
    if (!expected || GetTickCount64() > g_expectUntil.load() || !id) { original(finder, id, callback); return; }
    const auto key = keyIn(readPointer(static_cast<std::byte*>(finder) + g_listField), expected);
    static int logged = 0;
    const bool log1 = logged++ < 16;
    std::string replaced = key ? *key : std::string(kNoWorld);
    if (log1) {
        const auto asked = readString(reinterpret_cast<const std::byte*>(id)).value_or("?");
        if (!key) log().warn(L"Bot: the friend's world is gone from the game's list when it joins (asked for list id {}); not joining",
            std::wstring(asked.begin(), asked.end()));
        else log().info(L"Bot: the game joins list id {} (asked for {}; looked up again by the NetherNet ID)",
            std::wstring(key->begin(), key->end()), std::wstring(asked.begin(), asked.end()));
    }
    g_lookup.store(static_cast<int>(key ? Lookup::Matched : Lookup::Missing));
    original(finder, &replaced, callback);
}
namespace {
bool callSlot(void* game, std::uint32_t slot, const std::string& id)
{
    if (!g_ready || !game || id.empty()) return false;
    auto* const self = static_cast<std::byte*>(game) + g_iface;
    const auto* const table = static_cast<const std::byte*>(readPointer(self));
    const auto fn = table ? reinterpret_cast<JoinFn>(readPointer(table + slot)) : nullptr;
    if (!fn || !inMainModule(reinterpret_cast<const void*>(fn))) return false;
    const View view{id.data(), id.size()};
    return callJoinGuarded(self, fn, &view);
}
}
bool leaveThenJoin(void* game, const std::string& id) { return callSlot(game, g_leaveJoinSlot, id); }
}
