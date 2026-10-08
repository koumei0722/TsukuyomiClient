#include "modules/DeathLogger.h"

#include "config/Config.h"
#include "core/Logger.h"
#include "core/Paths.h"
#include "core/Strings.h"
#include "game/ClientChat.h"
#include "game/GameData.h"
#include "game/PlayerListMemory.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"
#include "memory/Signatures.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>

namespace tsukuyomi {
namespace {
constexpr std::uint32_t kAttributesComponent = 0xFD3B0613u;
constexpr std::size_t kAttributesSize = 0x50;
constexpr std::size_t kAttributeId = 4;
constexpr std::size_t kActorDimension = 0x1c8;
constexpr std::size_t kDimensionName = 0x20;

bool readPointer(const void* base, std::size_t offset, const void*& out)
{
    out = nullptr;
    return base != nullptr && memory::copyGuarded(static_cast<const char*>(base) + offset, &out, sizeof(out))
        && memory::plausiblePointer(out);
}

bool readShortString(const void* at, std::string& out)
{
    std::uint8_t raw[0x20]{};
    if (!memory::copyGuarded(at, raw, sizeof(raw))) return false;
    std::uint64_t length = 0;
    std::uint64_t capacity = 0;
    std::memcpy(&length, raw + 0x10, sizeof(length));
    std::memcpy(&capacity, raw + 0x18, sizeof(capacity));
    if (length == 0 || length > 48 || capacity < length || capacity < 15) return false;
    char text[49]{};
    if (capacity < 16) {
        std::memcpy(text, raw, static_cast<std::size_t>(length));
    } else {
        const void* pointer = nullptr;
        std::memcpy(&pointer, raw, sizeof(pointer));
        if (!memory::plausiblePointer(pointer) || !memory::copyGuarded(pointer, text, static_cast<std::size_t>(length))) {
            return false;
        }
    }
    for (std::uint64_t i = 0; i < length; ++i) {
        if (text[i] < 0x20 || text[i] > 0x7e) return false;
    }
    out.assign(text, static_cast<std::size_t>(length));
    return true;
}

const char* displayDimension(const std::string& internal)
{
    if (internal == "Overworld") return "Overworld";
    if (internal == "Nether") return "Nether";
    if (internal == "TheEnd") return "The End";
    return nullptr;
}

std::string timestamp()
{
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    char text[32]{};
    std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local);
    return text;
}
}

DeathLogger& DeathLogger::instance()
{
    static DeathLogger module;
    return module;
}

MenuItem DeathLogger::buildMenu()
{
    std::vector<MenuItem> children;
    children.push_back(enabledItem());
    children.push_back(toggleKeyItem());
    children.push_back(menu::toggle(L"Write to file", [this] { return m_writeToFile.load(std::memory_order_relaxed); },
                                    [this] { m_writeToFile.store(!m_writeToFile.load(std::memory_order_relaxed),
                                                                 std::memory_order_relaxed); }));
    MenuItem item = menu::submenu(name(), std::move(children));
    item.available = [this] { return available(); };
    item.isOn = [this] { return enabled(); };
    return item;
}

void DeathLogger::loadConfig(const nlohmann::json& section)
{
    Module::loadConfig(section);
    m_writeToFile.store(Config::getBool(section, "writeToFile", false), std::memory_order_relaxed);
}

void DeathLogger::saveConfig(nlohmann::json& section) const
{
    Module::saveConfig(section);
    section["writeToFile"] = m_writeToFile.load(std::memory_order_relaxed);
}

void DeathLogger::onScansReady()
{
    if (std::byte* const site = Scanner::instance().address(Target::HealthAttributeLoad)) {
        m_healthAttribute.store(memory::ripTarget(site, 8), std::memory_order_relaxed);
    }
    if (m_healthAttribute.load(std::memory_order_relaxed) == nullptr) {
        log().warn(L"DeathLogger: NOT usable (the health attribute was not found)");
        return;
    }
    log().info(L"DeathLogger: ready (file: {})", paths::dataDir().wstring() + L"\\deaths.txt");
}

std::string DeathLogger::resolveDimension()
{
    std::string internal;
    const void* dimension = nullptr;
    if (readPointer(GameData::instance().player(), kActorDimension, dimension)
        && readShortString(static_cast<const char*>(dimension) + kDimensionName, internal)) {
        if (const char* shown = displayDimension(internal)) return shown;
        return internal;
    }
    if (!m_dimensionFailLogged) {
        m_dimensionFailLogged = true;
        log().warn(L"DeathLogger: could not read the dimension name (Actor +0x{:x} -> +0x{:x}); shown as Unknown",
                   kActorDimension, kDimensionName);
    }
    return {};
}

void DeathLogger::onPlayerViewUpdate()
{
    if (!enabled()) {
        m_lastHealth = -1.0f;
        return;
    }
    const void* const healthAttribute = m_healthAttribute.load(std::memory_order_relaxed);
    void* const player = GameData::instance().player();
    if (healthAttribute == nullptr || player == nullptr) {
        m_lastHealth = -1.0f;
        return;
    }
    const unsigned long long serial = GameData::instance().playerSerial();
    if (serial != m_serial) {
        m_serial = serial;
        m_lastHealth = -1.0f;
    }
    std::uint32_t healthId = 0;
    std::uint8_t attributes[kAttributesSize]{};
    float current = 0.0f;
    float maximum = 0.0f;
    if (!memory::copyGuarded(static_cast<const char*>(healthAttribute) + kAttributeId, &healthId, sizeof(healthId))
        || !GameData::copyComponent(player, kAttributesComponent, sizeof(attributes), attributes)
        || !playerlist::mem::readAttribute(attributes, healthId, current, maximum)) {
        if (!m_readFailLogged) {
            m_readFailLogged = true;
            log().warn(L"DeathLogger: could not read the player's health (deaths are not detected while unreadable)");
        }
        m_lastHealth = -1.0f;
        return;
    }
    const float previous = m_lastHealth;
    m_lastHealth = current;
    if (!(previous > 0.0f && current <= 0.0f)) return;

    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    char coords[64] = "unknown position";
    if (GameData::instance().playerBoxFeet(x, y, z) || GameData::instance().playerFeet(x, y, z)) {
        std::snprintf(coords, sizeof(coords), "%d %d %d", static_cast<int>(std::floor(x)),
                      static_cast<int>(std::floor(y + 0.001f)), static_cast<int>(std::floor(z)));
    } else {
        log().warn(L"DeathLogger: died but the position was unreadable");
    }
    std::string dimension = resolveDimension();
    if (dimension.empty()) dimension = "Unknown";

    const std::string chat = "\xC2\xA7" "c[DeathLogger]\xC2\xA7r You died in \xC2\xA7" "e" + dimension
        + "\xC2\xA7r at \xC2\xA7" "e" + coords;
    const bool shown = clientchat::printLocal(chat);
    log().info(L"DeathLogger: died in {} at {} (chat: {})", toUtf16(dimension), toUtf16(coords),
               shown ? L"shown" : L"failed");
    if (m_writeToFile.load(std::memory_order_relaxed)) {
        std::lock_guard lock(m_pendingMutex);
        if (m_pending.size() < 64) m_pending.push_back(timestamp() + "\t" + dimension + "\t" + coords);
    }
}

void DeathLogger::onUpdate()
{
    std::vector<std::string> lines;
    {
        std::lock_guard lock(m_pendingMutex);
        if (m_pending.empty()) return;
        lines.swap(m_pending);
    }
    const auto path = paths::dataDir() / L"deaths.txt";
    std::ofstream file(path, std::ios::binary | std::ios::app);
    for (const auto& line : lines) file << line << "\r\n";
    file.flush();
    if (!file && !m_writeFailLogged) {
        m_writeFailLogged = true;
        log().warn(L"DeathLogger: could not write {}", path.wstring());
    }
}

}
