#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "modules/Module.h"

namespace tsukuyomi {

class AntiDarkness : public Module {
public:
    static AntiDarkness& instance();

    const wchar_t* name() const override { return L"AntiDarkness"; }
    bool available() const override;

    static std::byte* findReader();

    void onMobEffectRead(void* packet);

private:
    AntiDarkness() = default;

    static constexpr std::int32_t kDarknessEffectId = 30;

    static constexpr std::ptrdiff_t kDurationOffset = 0x38;
    static constexpr std::ptrdiff_t kEventOffset = 0x3C;
    static constexpr std::ptrdiff_t kEffectIdOffset = 0x40;
    static constexpr std::ptrdiff_t kAmplifierOffset = 0x44;

    static constexpr std::uint8_t kEventAdd = 1;
    static constexpr std::uint8_t kEventModify = 2;
    static constexpr std::uint8_t kEventRemove = 3;

    static constexpr std::size_t kPacketSize = 0x50;

    std::atomic<int> m_logged{0};
};

}
