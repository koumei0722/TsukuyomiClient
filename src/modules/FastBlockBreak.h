#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "modules/FastBlockBreakLogic.h"
#include "modules/Module.h"

namespace tsukuyomi {

class FastBlockBreak : public Module {
public:
    static FastBlockBreak& instance();

    const wchar_t* name() const override { return L"FastBlockBreak"; }
    bool available() const override;
    MenuItem buildMenu() override;
    void onScansReady() override;

    bool onContinueDestroyBlock(void* gameMode, const void* pos, std::uint8_t face,
                                const void* playerPos, bool* out);
    bool onDestroyBlock(void* gameMode, const void* pos, std::uint8_t face);

protected:
    void onEnabledChanged(bool enabled) override;
    bool persistEnabled() const override { return true; }

private:
    FastBlockBreak() = default;

    bool isLocalGameMode(void* gameMode) const;

    std::atomic<bool> m_active{false};
    std::atomic<unsigned int> m_toggleGeneration{0};
    unsigned int m_seenGeneration = 0;
    std::atomic<bool> m_ready{false};
    void* m_localPlayerVtable = nullptr;
    std::size_t m_playerOff = 0;
    std::size_t m_untilOff = 0;

    void* m_armedGm = nullptr;
    fastblockbreak::BlockPos m_brokenPos{};
    unsigned int m_skipLogs = 0;
};

}
