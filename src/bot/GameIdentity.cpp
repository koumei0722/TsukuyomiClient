#include "GameIdentity.h"
#include "BotNet.h"
#include "GameString.h"
#include "core/Logger.h"
#include "memory/Memory.h"
#include <Windows.h>
#include <mutex>

namespace tsukuyomi::bot::gameidentity {
namespace {
using u64 = std::uint64_t;
using AssertFn = u64(__fastcall*)(void* self, void* out, const std::string* contents);
using ControlFn = void(__fastcall*)(void* control);

std::byte* g_ctor = nullptr;
const void* g_vtable = nullptr;
std::mutex g_mutex;
void* g_held = nullptr;
std::string g_token;
std::string g_swapToken;

constexpr std::ptrdiff_t kControlFromObject = 0x10;
constexpr std::size_t kOptionalSize = 0x28;

int callException(unsigned code)
{
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR || code == 0xE06D7363u
        ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}
bool vtableIs(const void* object)
{
    const void* table = nullptr;
    return memory::plausiblePointer(object) && memory::copyGuarded(object, &table, sizeof table) && table == g_vtable;
}
volatile long* uses(void* object) { return reinterpret_cast<volatile long*>(static_cast<std::uint8_t*>(object) - kControlFromObject + 8); }
volatile long* weaks(void* object) { return reinterpret_cast<volatile long*>(static_cast<std::uint8_t*>(object) - kControlFromObject + 12); }
bool callControlGuarded(void* control, int slot)
{
    __try {
        const auto fn = (*reinterpret_cast<ControlFn* const*>(control))[slot];
        if (!memory::inGameModule(reinterpret_cast<const void*>(fn))) return false;
        fn(control);
        return true;
    } __except (callException(GetExceptionCode())) { return false; }
}
void releaseObject(void* object)
{
    if (!object) return;
    void* const control = static_cast<std::uint8_t*>(object) - kControlFromObject;
    if (InterlockedDecrement(uses(object)) != 0) return;
    if (!callControlGuarded(control, 0)) log().warn(L"Bot: destroying the game's identity faulted");
    if (InterlockedDecrement(weaks(object)) == 0 && !callControlGuarded(control, 1)) log().warn(L"Bot: freeing the game's identity faulted");
}
bool controlLooksValid(void* object)
{
    void* const control = static_cast<std::uint8_t*>(object) - kControlFromObject;
    const void* table = nullptr;
    long count = 0;
    return memory::copyGuarded(control, &table, sizeof table) && memory::inGameModule(table)
        && memory::copyGuarded(static_cast<std::uint8_t*>(control) + 8, &count, sizeof count) && count >= 1 && count < 1000;
}
bool assertGuarded(void* object, void* out, const std::string* contents)
{
    __try {
        const auto fn = (*reinterpret_cast<AssertFn* const*>(object))[1];
        if (!memory::inGameModule(reinterpret_cast<const void*>(fn))) return false;
        fn(object, out, contents);
        return true;
    } __except (callException(GetExceptionCode())) { return false; }
}
}

bool initialize()
{
    auto* const site = net::site(Site::NetherNetIdentityCtorSite);
    if (!site) { log().warn(L"Bot: the game's identity was not found - switching to a bot on its own connection is unavailable"); return false; }
    DWORD64 imageBase = 0;
    const auto* entry = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(site), &imageBase, nullptr);
    std::int32_t rel = 0;
    if (!entry || !memory::copyGuarded(site + 16, &rel, sizeof rel)) { log().warn(L"Bot: cannot read the game's identity constructor"); return false; }
    g_ctor = reinterpret_cast<std::byte*>(imageBase + entry->BeginAddress);
    g_vtable = site + 20 + rel;
    if (site - g_ctor != 0x20 || !memory::inGameModule(g_vtable)) {
        log().warn(L"Bot: the game's identity constructor is not where expected (+{:#x})", static_cast<std::uintptr_t>(site - g_ctor));
        g_ctor = nullptr; return false;
    }
    return true;
}
void* ctorTarget() { return g_ctor; }

void* onConstruct(CtorFn original, void* self, void* key, void* token)
{
    std::string swap;
    { std::lock_guard lock(g_mutex); swap = std::exchange(g_swapToken, {}); }
    bool swapped = false;
    if (!swap.empty()) {
        swapped = net::assignToGameString(token, swap);
        log().info(L"Bot: the game's new WebRTC identity {} the bot's token", swapped ? L"uses" : L"could not take");
    }
    void* const result = original(self, key, token);
    if (swapped || !vtableIs(self) || !controlLooksValid(self)) return result;
    auto copied = gamestring::read(static_cast<std::uint8_t*>(self) + 0x58);
    InterlockedIncrement(uses(self));
    void* previous = nullptr;
    {
        std::lock_guard lock(g_mutex);
        previous = std::exchange(g_held, self);
        g_token = copied.value_or(std::string());
    }
    releaseObject(previous);
    return result;
}

bool held() { std::lock_guard lock(g_mutex); return g_held != nullptr; }

std::optional<std::string> assertWith(const std::string& fingerprints)
{
    std::lock_guard lock(g_mutex);
    if (!g_held) return std::nullopt;
    alignas(8) std::uint8_t out[kOptionalSize]{};
    if (!assertGuarded(g_held, out, &fingerprints)) { log().warn(L"Bot: the game's identity faulted while signing"); return std::nullopt; }
    if (out[0x20] != 1) { log().warn(L"Bot: the game's identity returned no assertion"); return std::nullopt; }
    auto value = gamestring::read(out);
    gamestring::destroy(out);
    return value;
}

std::optional<std::string> token()
{
    std::lock_guard lock(g_mutex);
    return g_held && !g_token.empty() ? std::optional<std::string>(g_token) : std::nullopt;
}
void armTokenSwap(std::string token) { std::lock_guard lock(g_mutex); g_swapToken = std::move(token); }
void disarmTokenSwap() { std::lock_guard lock(g_mutex); g_swapToken.clear(); }
void release()
{
    void* previous = nullptr;
    { std::lock_guard lock(g_mutex); previous = std::exchange(g_held, nullptr); g_token.clear(); g_swapToken.clear(); }
    releaseObject(previous);
}

}
