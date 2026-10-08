#include "modules/ToggleSneakSprint.h"

#include "config/Config.h"
#include "modules/FreeCamera.h"
#include "ui/Menu.h"

#include <Windows.h>

#include <cstdint>

namespace tsukuyomi {

namespace {

int accessFilter(unsigned long code)
{
    return code == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

bool readBits(const void* out, std::uint32_t& bits)
{
    __try {
        bits = *static_cast<const volatile std::uint32_t*>(out);
        return true;
    } __except (accessFilter(GetExceptionCode())) {
        return false;
    }
}

void writeBits(void* out, std::uint32_t bits)
{
    __try {
        *static_cast<volatile std::uint32_t*>(out) = bits;
    } __except (accessFilter(GetExceptionCode())) {
    }
}

}

ToggleSneakSprint& ToggleSneakSprint::instance()
{
    static ToggleSneakSprint module;
    return module;
}

MenuItem ToggleSneakSprint::buildMenu()
{
    std::vector<MenuItem> children;
    children.push_back(enabledItem());
    children.push_back(toggleKeyItem());
    children.push_back(menu::toggle(
        L"Toggle sneak", [this] { return m_sneak.load(std::memory_order_relaxed); },
        [this] { m_sneak.store(!m_sneak.load(std::memory_order_relaxed), std::memory_order_relaxed); }));
    children.push_back(menu::toggle(
        L"Toggle sprint", [this] { return m_sprint.load(std::memory_order_relaxed); },
        [this] { m_sprint.store(!m_sprint.load(std::memory_order_relaxed), std::memory_order_relaxed); }));
    MenuItem item = menu::submenu(name(), std::move(children));
    item.available = [this] { return available(); };
    item.isOn = [this] { return enabled(); };
    return item;
}

void ToggleSneakSprint::loadConfig(const nlohmann::json& section)
{
    Module::loadConfig(section);
    m_sneak.store(Config::getBool(section, "toggleSneak", true), std::memory_order_relaxed);
    m_sprint.store(Config::getBool(section, "toggleSprint", true), std::memory_order_relaxed);
}

void ToggleSneakSprint::saveConfig(nlohmann::json& section) const
{
    Module::saveConfig(section);
    section["toggleSneak"] = m_sneak.load(std::memory_order_relaxed);
    section["toggleSprint"] = m_sprint.load(std::memory_order_relaxed);
}

void ToggleSneakSprint::setKeyCallReturn(const void* returnAddress)
{
    m_keyCallReturn.store(returnAddress, std::memory_order_release);
}

void ToggleSneakSprint::onInputGatherBefore(void* out, const void* returnAddress)
{
    const void* const keyCall = m_keyCallReturn.load(std::memory_order_acquire);
    if (out == nullptr || keyCall == nullptr || returnAddress != keyCall) {
        return;
    }
    std::uint32_t raw = 0;
    if (!readBits(out, raw)) {
        return;
    }
    if (FreeCamera::instance().active()) {
        m_state.passThrough(raw);
        return;
    }
    const bool on = enabled();
    const bool sneak = on && m_sneak.load(std::memory_order_relaxed);
    const bool sprint = on && m_sprint.load(std::memory_order_relaxed);
    if (!sneak && !sprint && !m_state.sneakLatched() && !m_state.sprintLatched()) {
        m_state.passThrough(raw);
        return;
    }
    const std::uint32_t bits = m_state.apply(raw, sneak, sprint);
    if (bits != raw) {
        writeBits(out, bits);
    }
}

}
