#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "modules/Module.h"

namespace tsukuyomi {

class Fullbright : public Module {
public:
    static Fullbright& instance();

    const wchar_t* name() const override { return L"Fullbright"; }
    bool available() const override;

    void* onGetEffect(int effectId, void* original);

    static constexpr int kNightVisionEffectId = 16;

protected:
    bool persistEnabled() const override { return true; }

private:
    Fullbright() = default;

    static constexpr std::size_t kFakeBytes = 0x60;
    static constexpr std::int32_t kFakeDuration = 1000000;

    std::atomic<bool> m_logged{false};

    alignas(16) std::byte m_fake[kFakeBytes]{};
    bool m_fakeReady = false;

    void ensureFake();
};

}
