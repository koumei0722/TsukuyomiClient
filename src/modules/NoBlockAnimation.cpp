#include "modules/NoBlockAnimation.h"

#include "core/Logger.h"
#include "core/Notice.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"

#include <Windows.h>

#include <cstdint>
#include <cstring>

namespace tsukuyomi {

namespace {

constexpr char kZoneName[] = "Ticking textures - Render";

bool hasZoneName(std::byte* function)
{
    ULONG64 base = 0;
    const auto* const entry = RtlLookupFunctionEntry(reinterpret_cast<ULONG64>(function), &base, nullptr);
    if (entry == nullptr || base == 0) {
        return false;
    }
    auto* const begin = reinterpret_cast<const unsigned char*>(base + entry->BeginAddress);
    auto* const end = reinterpret_cast<const unsigned char*>(base + entry->EndAddress);
    if (begin != reinterpret_cast<const unsigned char*>(function) || end - begin < 8
        || !memory::isReadable(begin, static_cast<std::size_t>(end - begin))) {
        return false;
    }
    for (const unsigned char* at = begin; at + 7 <= end; ++at) {
        if ((at[0] != 0x48 && at[0] != 0x4C) || at[1] != 0x8D || (at[2] & 0xC7) != 0x05) {
            continue;
        }
        std::int32_t disp = 0;
        std::memcpy(&disp, at + 3, sizeof(disp));
        const auto* const target = at + 7 + disp;
        if (memory::isReadable(target, sizeof(kZoneName))
            && std::memcmp(target, kZoneName, sizeof(kZoneName)) == 0) {
            return true;
        }
    }
    return false;
}

}

NoBlockAnimation& NoBlockAnimation::instance()
{
    static NoBlockAnimation module;
    return module;
}

void NoBlockAnimation::onScansReady()
{
    std::byte* const function = Scanner::instance().address(Target::TickingTextureRender);
    if (function == nullptr) {
        return;
    }
    if (static_cast<unsigned char>(function[0]) != 0x55 || !hasZoneName(function)) {
        notice::failOnce("NoBlockAnimation.verify",
                         L"NoBlockAnimation: the ticking texture stage did not match (no push rbp or zone name); "
                         L"NoBlockAnimation is off",
                         "NoBlockAnimation is off: the ticking texture stage was not found");
        return;
    }
    m_patch = makeAtomicPatch(function, {std::byte{0xC3}}, "NoBlockAnimation.Render");
    if (!m_patch.valid()) {
        return;
    }
    if (enabled()) {
        m_patch.apply();
    }
    log().info(L"NoBlockAnimation: ticking texture stage found ({})", enabled() ? L"on" : L"off");
}

void NoBlockAnimation::onEnabledChanged(bool enabled)
{
    if (m_patch.valid()) {
        m_patch.setEnabled(enabled);
    }
}

void NoBlockAnimation::shutdown()
{
    m_patch.restore();
}

}
