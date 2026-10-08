#include "game/MobEffectList.h"

#include "game/GameData.h"
#include "game/GameString.h"
#include "game/InventoryEffectsLogic.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"
#include "memory/Signatures.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace tsukuyomi::mobeffects {

namespace {

constexpr std::size_t kInstanceSize = 0x90;
constexpr std::size_t kDuration = 0x04;
constexpr std::size_t kAmplifier = 0x20;
constexpr std::size_t kAmbient = 0x26;
constexpr std::size_t kEffectHidden = 0xa0;
constexpr std::size_t kEffectColor = 0x10;
constexpr std::size_t kLoopSkipIdDisp = 0x0D;
constexpr std::size_t kNameTableDisp = 0x69;

constexpr std::uint32_t fnv1a(const char* s)
{
    std::uint32_t h = 0x811C9DC5u;
    for (; *s != 0; ++s) {
        h ^= static_cast<std::uint8_t>(*s);
        h *= 0x01000193u;
    }
    return h;
}
constexpr std::uint32_t kMobEffectsComponent = fnv1a("MobEffectsComponent");
static_assert(kMobEffectsComponent == 0xE6A1B550u);
constexpr std::size_t kComponentSize = 0x18;
constexpr std::size_t kMaxInstances = 256;

using DisplayNameFn = void*(__fastcall*)(const void* instance, void* ret);
using DurationTextFn = void*(__fastcall*)(void* ret, int ticks);

DisplayNameFn g_displayName = nullptr;
DurationTextFn g_durationText = nullptr;
const void* const* g_effectTable = nullptr;
const std::int32_t* g_skipId = nullptr;

LONG accessFilter(DWORD code)
{
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR
        ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

bool callDisplayNameGuarded(const void* instance, void* out)
{
    __try {
        g_displayName(instance, out);
        return true;
    } __except (accessFilter(GetExceptionCode())) {
        return false;
    }
}

bool callDurationTextGuarded(int ticks, void* out)
{
    __try {
        g_durationText(out, ticks);
        return true;
    } __except (accessFilter(GetExceptionCode())) {
        return false;
    }
}

bool takeGameString(void* str, std::string& out)
{
    const bool read = gamestring::read(str, out);
    gamestring::release(str);
    return read;
}

}

void resolve()
{
    auto& scanner = Scanner::instance();
    std::byte* const loop = scanner.address(Target::MobEffectScreenListLoop);
    std::byte* const name = scanner.address(Target::MobEffectInstanceDisplayName);
    g_displayName = reinterpret_cast<DisplayNameFn>(name);
    g_durationText = scanner.addressAs<DurationTextFn>(Target::MobEffectDurationText);
    g_skipId = loop != nullptr ? static_cast<const std::int32_t*>(memory::ripTarget(loop, kLoopSkipIdDisp)) : nullptr;
    g_effectTable = name != nullptr ? static_cast<const void* const*>(memory::ripTarget(name, kNameTableDisp)) : nullptr;
}

bool ready()
{
    return g_displayName != nullptr && g_durationText != nullptr && g_skipId != nullptr && g_effectTable != nullptr
           && gamestring::available();
}

namespace {

bool effectRange(std::uintptr_t& begin, std::uintptr_t& end)
{
    const void* const player = GameData::instance().player();
    if (player == nullptr) return false;
    if (const void* const vtable = GameData::instance().playerVtable(); vtable != nullptr) {
        const void* have = nullptr;
        if (!memory::copyGuarded(player, &have, sizeof(have)) || have != vtable) return false;
    }
    std::array<std::uintptr_t, 3> vec{};
    if (!GameData::copyComponent(player, kMobEffectsComponent, kComponentSize, vec.data())) return false;
    begin = vec[0];
    end = vec[1];
    if (begin == end) return true;
    return memory::plausiblePointer(begin) && end > begin && (end - begin) % kInstanceSize == 0
           && (end - begin) / kInstanceSize <= kMaxInstances;
}

}

bool listedByIndex(std::vector<std::uint8_t>& out)
{
    out.clear();
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
    if (!ready() || !effectRange(begin, end)) return false;
    const std::size_t count = std::min<std::size_t>((end - begin) / kInstanceSize, kMaxId + 1);
    out.assign(count, 0);
    for (std::size_t i = 0; i < count; ++i) {
        const void* effect = nullptr;
        if (!memory::copyGuarded(g_effectTable + i, &effect, sizeof(effect))) return false;
        if (effect == nullptr) continue;
        if (!memory::plausiblePointer(effect)) return false;
        std::int32_t hidden = 0;
        if (!memory::copyGuarded(static_cast<const char*>(effect) + kEffectHidden, &hidden, sizeof(hidden))) return false;
        out[i] = hidden >= 0 ? 1 : 0;
    }
    return true;
}

bool read(std::vector<Effect>& out)
{
    out.clear();
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
    if (!ready() || !effectRange(begin, end)) return false;
    if (begin == end) return true;
    const std::size_t count = (end - begin) / kInstanceSize;
    std::vector<unsigned char> bytes(count * kInstanceSize);
    if (!memory::copyGuarded(reinterpret_cast<const void*>(begin), bytes.data(), bytes.size())) return false;
    std::int32_t skip = -1;
    if (!memory::copyGuarded(g_skipId, &skip, sizeof(skip))) return false;
    for (std::size_t i = 0; i < count; ++i) {
        const unsigned char* const one = bytes.data() + i * kInstanceSize;
        Effect e;
        e.index = static_cast<int>(i);
        std::memcpy(&e.id, one, sizeof(e.id));
        std::memcpy(&e.duration, one + kDuration, sizeof(e.duration));
        std::memcpy(&e.amplifier, one + kAmplifier, sizeof(e.amplifier));
        e.ambient = one[kAmbient] != 0;
        if (e.id < 0 || e.id > kMaxId || e.id == skip) continue;
        if (!memory::copyGuarded(g_effectTable + e.id, &e.effect, sizeof(e.effect))) return false;
        if (e.effect == nullptr) continue;
        if (!memory::plausiblePointer(e.effect)) return false;
        std::int32_t hidden = 0;
        float rgb[3]{};
        if (!memory::copyGuarded(static_cast<const char*>(e.effect) + kEffectHidden, &hidden, sizeof(hidden))
            || !memory::copyGuarded(static_cast<const char*>(e.effect) + kEffectColor, rgb, sizeof(rgb))) {
            return false;
        }
        if (hidden < 0) continue;
        if (e.duration == 0 || (e.duration <= 0 && e.duration != -1)) continue;
        e.color = invfx::colorFromFloats(rgb[0], rgb[1], rgb[2]);
        out.push_back(e);
    }
    return true;
}

bool displayName(int id, int amplifier, std::string& out)
{
    out.clear();
    if (g_displayName == nullptr) return false;
    alignas(16) unsigned char instance[kInstanceSize]{};
    std::memcpy(instance, &id, sizeof(id));
    std::memcpy(instance + kAmplifier, &amplifier, sizeof(amplifier));
    alignas(8) unsigned char str[0x20]{};
    if (!callDisplayNameGuarded(instance, str) || !takeGameString(str, out)) {
        out.clear();
        return false;
    }
    return true;
}

bool durationText(int ticks, std::string& out)
{
    out.clear();
    if (g_durationText == nullptr) return false;
    alignas(8) unsigned char str[0x20]{};
    if (!callDurationTextGuarded(ticks, str) || !takeGameString(str, out)) {
        out.clear();
        return false;
    }
    return true;
}

}
