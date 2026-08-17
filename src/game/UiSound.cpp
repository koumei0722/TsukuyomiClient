#include "game/UiSound.h"

#include <Windows.h>

#include <cstring>

#include "core/Logger.h"
#include "game/GameVersion.h"
#include "hooks/HookManager.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"

namespace tsukuyomi {

namespace {

using PlaySoundFn = void(__fastcall*)(void* self, const void* name, const void* position, float a,
                                      float b, bool c, const void* extra);

PlaySoundFn g_playSound = nullptr;

void __fastcall detourPlaySound(void* self, const void* name, const void* position, float a,
                                float b, bool c, const void* extra)
{
    UiSound::instance().onPlayed(self, name, position, a, b, c, extra);
    if (g_playSound != nullptr) {
        g_playSound(self, name, position, a, b, c, extra);
    }
}

struct SsoString {
    char buffer[16]{};
    std::uintptr_t size = 0;
    std::uintptr_t capacity = 15;
};

SsoString makeString(const char* text)
{
    SsoString out;
    size_t length = std::strlen(text);
    if (length > 15) {
        length = 15;
    }
    std::memcpy(out.buffer, text, length);
    out.size = length;
    return out;
}

}

UiSound& UiSound::instance()
{
    static UiSound sound;
    return sound;
}

void UiSound::onScansReady()
{
    if (g_playSound != nullptr) {
        return;
    }

    void* const target = Scanner::instance().address(Target::PlaySound);
    if (target == nullptr) {
        log().warn(L"UiSound: the sound function was not found; the toggle will be silent");
        return;
    }
    HookManager::instance().create(target, reinterpret_cast<void*>(&detourPlaySound),
                                   reinterpret_cast<void**>(&g_playSound), L"PlaySound");
}

void UiSound::request()
{
    m_wanted.store(true, std::memory_order_relaxed);
}

void UiSound::onPlayed(void* self, const void* name, const void* position, float a, float b,
                       bool c, const void* extra)
{
    (void)name;
    if (m_ready.load(std::memory_order_acquire) || self == nullptr) {
        return;
    }
    if (position == nullptr || extra == nullptr || !memory::isReadable(position, sizeof(m_position))
        || !memory::isReadable(extra, sizeof(m_extra))) {
        return;
    }
    std::memcpy(m_position, position, sizeof(m_position));
    std::memcpy(m_extra, extra, sizeof(m_extra));
    m_a = a;
    m_b = b;
    m_c = c;
    m_self.store(self, std::memory_order_release);
    m_ready.store(true, std::memory_order_release);
    log().info(L"UiSound: remembered how the game plays a sound ({:#x})",
               reinterpret_cast<std::uintptr_t>(self));
}

void UiSound::pump()
{
    if (!m_wanted.exchange(false, std::memory_order_relaxed)) {
        return;
    }
    if (g_playSound == nullptr || !m_ready.load(std::memory_order_acquire)) {
        return;
    }
    void* const self = m_self.load(std::memory_order_acquire);
    if (self == nullptr || !memory::isReadable(self, 0x1200)) {
        return;
    }

    alignas(16) unsigned char position[16]{};
    std::memcpy(position, m_position, sizeof(position));
    std::memcpy(position, static_cast<const char*>(self) + 0x11b8, sizeof(float) * 3);

    const SsoString name = makeString("random.click");
    g_playSound(self, &name, position, m_a, m_b, m_c, m_extra);
    static std::atomic<int> said{0};
    if (said.fetch_add(1) < 6) {
        log().info(L"UiSound: played random.click");
    }
}

}
