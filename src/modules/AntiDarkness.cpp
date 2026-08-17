#include "modules/AntiDarkness.h"

#include <cstring>

#include "core/Logger.h"
#include "game/ItemStackRequest.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"

namespace tsukuyomi {

AntiDarkness& AntiDarkness::instance()
{
    static AntiDarkness module;
    return module;
}

bool AntiDarkness::available() const
{
    return Scanner::instance().found(Target::MobEffectGetId);
}

std::byte* AntiDarkness::findReader()
{

    return ItemStackRequest::resolvePacketReader(Target::MobEffectGetId, L"mob-effect reader");
}

void AntiDarkness::onMobEffectRead(void* packet)
{
    if (packet == nullptr || !memory::isReadable(packet, kPacketSize)) {
        return;
    }

    auto* const bytes = static_cast<std::byte*>(packet);
    std::int32_t effectId = 0;
    std::memcpy(&effectId, bytes + kEffectIdOffset, sizeof(effectId));

    std::uint8_t event = 0;
    std::int32_t duration = 0;
    std::int32_t amplifier = 0;
    std::memcpy(&event, bytes + kEventOffset, sizeof(event));
    std::memcpy(&duration, bytes + kDurationOffset, sizeof(duration));
    std::memcpy(&amplifier, bytes + kAmplifierOffset, sizeof(amplifier));

    if (m_logged.fetch_add(1, std::memory_order_acq_rel) < 3) {

        log().info(L"AntiDarkness: mob-effect packet (effect {}, event {}, "
                   L"duration {}, amplifier {})",
                   effectId, event, duration, amplifier);
    }

    if (!enabled() || effectId != kDarknessEffectId) {
        return;
    }
    if (event != kEventAdd && event != kEventModify) {
        return;
    }

    const std::uint8_t remove = kEventRemove;
    std::memcpy(bytes + kEventOffset, &remove, sizeof(remove));
}

}
