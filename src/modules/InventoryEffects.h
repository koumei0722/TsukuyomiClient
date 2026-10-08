#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "game/ContainerUi.h"
#include "modules/Module.h"

namespace tsukuyomi {

class InventoryEffects : public Module, public containerui::Listener {
public:
    static InventoryEffects& instance();

    const wchar_t* name() const override { return L"InventoryEffects"; }
    bool available() const override;

    void onScansReady() override;
    void shutdown() override;

    bool onSlotButton(std::uint32_t, int, const std::string&, int) override { return false; }
    void onScreenTick() override;
    void onScreenLost() override;
    void onScreenCreated(void* ctrl) override;

private:
    InventoryEffects() = default;

    struct Row {
        int id = 0;
        int duration = 0;
        int amplifier = 0;
        bool ambient = false;
        std::uint32_t color = 0;
        std::string name;
        std::string time;
        std::string icon;
        std::string tip;
    };

    void refresh();
    void clearRows();
    const std::string& nameOf(int id, int amplifier);
    std::string timeOf(int duration);
    const std::string& iconOf(int id, const void* effect);

    static bool listOn(std::uintptr_t);
    static bool rowOn(std::uintptr_t row);
    static float rowY(std::uintptr_t row);
    static void rowName(std::uintptr_t row, std::string& out);
    static void rowTime(std::uintptr_t row, std::string& out);
    static void rowIcon(std::uintptr_t row, std::string& out);
    static void rowBg(std::uintptr_t row, std::string& out);
    static void rowTip(std::uintptr_t row, std::string& out);

    bool m_ready = false;
    std::vector<Row> m_rows;
    std::vector<int> m_shownIds;
    bool m_shownOn = false;
    std::map<int, std::string> m_names;
    std::map<int, std::string> m_icons;
    std::string m_infinite;
    bool m_failLogged = false;
    bool m_listLogged = false;
};

}
