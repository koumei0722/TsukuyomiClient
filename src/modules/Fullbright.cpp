#include "modules/Fullbright.h"

#include "core/Logger.h"
#include "memory/Scanner.h"

#include <cstring>

namespace tsukuyomi {

Fullbright& Fullbright::instance()
{
    static Fullbright module;
    return module;
}

bool Fullbright::available() const
{
    return Scanner::instance().found(Target::GetActorEffect);
}

void Fullbright::ensureFake()
{
    if (m_fakeReady) {
        return;
    }
    std::memset(m_fake, 0, sizeof(m_fake));

    const std::int32_t id = kNightVisionEffectId;
    const std::int32_t duration = kFakeDuration;
    std::memcpy(m_fake + 0x00, &id, sizeof(id));
    for (std::size_t at = 0x04; at <= 0x10; at += sizeof(duration)) {
        std::memcpy(m_fake + at, &duration, sizeof(duration));
    }
    m_fakeReady = true;
}

void* Fullbright::onGetEffect(int effectId, void* original)
{
    if (!enabled() || effectId != kNightVisionEffectId) {
        return original;
    }
    if (original != nullptr) {
        return original;
    }

    ensureFake();

    if (!m_logged.exchange(true, std::memory_order_relaxed)) {
        log().info(L"Fullbright: night vision (effect {}) is now reported as active",
                   kNightVisionEffectId);
    }
    return m_fake;
}

}
