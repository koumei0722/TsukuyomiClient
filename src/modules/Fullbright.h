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

    void* onGetEffect(int effectId, void* original, const void* returnAddress);

    void setFogColorReturns(const void* first, const void* second);

    static constexpr int kNightVisionEffectId = 16;

protected:

private:
    Fullbright() = default;

    static constexpr std::size_t kFakeBytes = 0x60;
    static constexpr std::int32_t kFakeDuration = 1000000;

    std::atomic<bool> m_logged{false};

    alignas(16) std::byte m_fake[kFakeBytes]{};
    bool m_fakeReady = false;

    std::atomic<const void*> m_fogReturn[2]{};

    void ensureFake();
};

}
