#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "game/EffectTimerLayout.h"
#include "modules/Module.h"

namespace tsukuyomi {

class EffectTimer : public Module {
public:
    static EffectTimer& instance();

    const wchar_t* name() const override { return L"EffectTimer"; }
    bool available() const override { return m_ready.load(std::memory_order_relaxed); }

    void onScansReady() override;
    void shutdown() override;

    void onRendered(void* client, void* owner);

    void beforeLayout(void* layout);
    void afterLayout(void* layout);

protected:
    void onEnabledChanged(bool enabled) override;

private:
    EffectTimer() = default;

    static void onHudCreated(void* ctrl);
    void hideAll();
    void* guiDataOf(void* client);
    bool guiScaleOf(void* client, float& scale) const;
    void shiftColumns(void* layout);
    bool render(void* client, void* owner);
    const std::string& infiniteText();

    std::atomic<bool> m_ready{false};
    std::atomic<bool> m_stopping{false};
    std::atomic<int> m_inside{0};

    std::int32_t m_guiDataSlot = 0;
    std::uint8_t m_recordsField = 0;
    std::uint8_t m_backgroundField = 0;
    std::uint8_t m_positionX = 0;
    std::uint8_t m_positionY = 0;
    std::uint8_t m_dirtyField = 0;
    std::int32_t m_realGuiField = 0;
    void* m_guiScaleFn = nullptr;

    const void* m_clientVtable = nullptr;
    std::int32_t m_guiDataField = -1;
    std::array<int, efxtimer::kRows> m_shownKey{};
    std::string m_infinite;
    std::array<std::string, efxtimer::kRows> m_texts;

    struct Shifted {
        bool on = false;
        float shiftPx = 0;
        float iconX0 = 0;
    };
    std::array<Shifted, efxtimer::kMaxId + 1> m_shifted{};
    std::uintptr_t m_shiftedRecords = 0;
    std::uint8_t m_iconField = 0;
    bool m_columnsReady = false;
    std::atomic<int> m_textWidth{0};
    bool m_loggedShift = false;
    bool m_loggedFrame = false;
    int m_failLogs = 0;
};

}
