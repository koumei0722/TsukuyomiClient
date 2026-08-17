#include "game/GameData.h"

#include <Windows.h>

#include "core/Logger.h"
#include "memory/Memory.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace tsukuyomi {

GameData& GameData::instance()
{
    static GameData data;
    return data;
}

void GameData::setPlayerView(const PlayerView& view)
{
    std::lock_guard lock(m_mutex);
    m_view = view;
    m_valid = true;
}

PlayerView GameData::playerView() const
{
    std::lock_guard lock(m_mutex);
    return m_view;
}

bool GameData::hasPlayerView() const
{
    std::lock_guard lock(m_mutex);
    return m_valid;
}

float GameData::yaw() const
{
    std::lock_guard lock(m_mutex);
    return m_view.yaw;
}

void GameData::setGameMode(void* gameMode)
{
    m_gameMode.store(gameMode, std::memory_order_relaxed);
}

void* GameData::gameMode() const
{
    return m_gameMode.load(std::memory_order_relaxed);
}

void GameData::setPlayer(void* player)
{
    m_player.store(player, std::memory_order_relaxed);
}

void* GameData::player() const
{
    return m_player.load(std::memory_order_relaxed);
}

namespace {

template <typename T>
bool readAt(const void* address, T& out)
{
    if (address == nullptr || !memory::isReadable(address, sizeof(T))) {
        return false;
    }
    std::memcpy(&out, address, sizeof(T));
    return true;
}

}

bool GameData::isPlayerEntity(const void* entityContext) const
{
    const void* const self = m_player.load(std::memory_order_relaxed);
    if (entityContext == nullptr || self == nullptr || !memory::isReadable(entityContext, 0x1c)
        || !memory::isReadable(self, 0x1c)) {
        return false;
    }
    const auto* const own = static_cast<const char*>(self);
    void* ownRegistry = nullptr;
    unsigned int ownId = 0;
    std::memcpy(&ownRegistry, own + 0x10, sizeof(ownRegistry));
    std::memcpy(&ownId, own + 0x18, sizeof(ownId));
    if (ownRegistry == nullptr) {
        return false;
    }

    const auto* const ctx = static_cast<const char*>(entityContext);
    const auto matches = [&](std::ptrdiff_t registryAt, std::ptrdiff_t idAt) {
        void* registry = nullptr;
        unsigned int id = 0;
        std::memcpy(&registry, ctx + registryAt, sizeof(registry));
        std::memcpy(&id, ctx + idAt, sizeof(id));
        return registry == ownRegistry && id == ownId;
    };
    return matches(0x10, 0x18) || matches(0x08, 0x10);
}

void* GameData::playerComponent(unsigned int typeId, std::size_t stride) const
{
    void* const self = m_player.load(std::memory_order_relaxed);
    if (self == nullptr || stride == 0) {
        return nullptr;
    }
    const auto* const base = static_cast<const char*>(self);
    std::uintptr_t registry = 0;
    std::uint32_t id = 0;
    if (!readAt(base + 0x10, registry) || !readAt(base + 0x18, id) || registry == 0) {
        return nullptr;
    }

    std::uintptr_t first = 0;
    std::uintptr_t last = 0;
    if (!readAt(reinterpret_cast<const void*>(registry + 0x68), first)
        || !readAt(reinterpret_cast<const void*>(registry + 0x70), last)) {
        return nullptr;
    }
    if (first == 0 || last <= first || (last - first) % 0x20 != 0) {
        return nullptr;
    }
    constexpr std::uintptr_t kMaxEntries = 4096;
    const std::uintptr_t count = (std::min)((last - first) / 0x20, kMaxEntries);
    std::uintptr_t store = 0;
    for (std::uintptr_t i = 0; i < count; ++i) {
        const auto entry = first + i * 0x20;
        std::uint32_t kind = 0;
        if (!readAt(reinterpret_cast<const void*>(entry + 0x08), kind)) {
            return nullptr;
        }
        if (kind != typeId) {
            continue;
        }
        if (!readAt(reinterpret_cast<const void*>(entry + 0x10), store)) {
            return nullptr;
        }
        break;
    }
    if (store == 0) {
        return nullptr;
    }

    const std::uint32_t low = id & 0x3ffffu;
    const std::uint32_t page = low >> 11;
    std::uintptr_t pagesBegin = 0;
    std::uintptr_t pagesEnd = 0;
    if (!readAt(reinterpret_cast<const void*>(store + 0x08), pagesBegin)
        || !readAt(reinterpret_cast<const void*>(store + 0x10), pagesEnd)) {
        return nullptr;
    }
    if (pagesBegin == 0 || pagesEnd <= pagesBegin
        || page >= (pagesEnd - pagesBegin) / sizeof(std::uintptr_t)) {
        return nullptr;
    }
    std::uintptr_t sparse = 0;
    if (!readAt(reinterpret_cast<const void*>(pagesBegin + page * sizeof(std::uintptr_t)), sparse)
        || sparse == 0) {
        return nullptr;
    }
    std::uint32_t packed = 0;
    if (!readAt(reinterpret_cast<const void*>(sparse + (low & 0x7ffu) * 4), packed)) {
        return nullptr;
    }
    if (((id & 0xfffc0000u) ^ packed) > 0x3fffeu) {
        return nullptr;
    }

    std::uintptr_t table = 0;
    if (!readAt(reinterpret_cast<const void*>(store + 0x50), table) || table == 0) {
        return nullptr;
    }
    std::uintptr_t dense = 0;
    if (!readAt(reinterpret_cast<const void*>(table + ((packed >> 4) & 0x3ff8u)), dense)
        || dense == 0) {
        return nullptr;
    }
    auto* const at = reinterpret_cast<void*>(dense + (packed & 0x7fu) * stride);
    if (!memory::isReadable(at, stride)) {
        return nullptr;
    }
    return at;
}

bool GameData::playerFeet(float& outX, float& outY, float& outZ) const
{
    void* const self = m_player.load(std::memory_order_relaxed);
    if (self == nullptr) {
        return false;
    }
    const auto* const at = static_cast<const char*>(self) + kPlayerPositionOffset;
    if (!memory::isReadable(at, sizeof(float) * 3)) {
        return false;
    }
    float position[3]{};
    std::memcpy(position, at, sizeof(position));
    constexpr float kWorldLimit = 3.0e7f;
    for (const float value : position) {
        if (!std::isfinite(value) || std::fabs(value) > kWorldLimit) {
            return false;
        }
    }
    outX = position[0];
    outY = position[1] - kEyeHeight;
    outZ = position[2];
    return true;
}

bool GameData::playerFeetY(float& outY) const
{
    void* const self = m_player.load(std::memory_order_relaxed);
    if (self == nullptr) {
        return false;
    }
    const auto* const at = static_cast<const char*>(self) + kPlayerPositionOffset;
    if (!memory::isReadable(at, sizeof(float) * 3)) {
        return false;
    }
    float position[3]{};
    std::memcpy(position, at, sizeof(position));

    constexpr float kWorldLimit = 3.0e7f;
    for (const float value : position) {
        if (!std::isfinite(value) || std::fabs(value) > kWorldLimit) {
            return false;
        }
    }
    outY = position[1] - kEyeHeight;
    return true;
}

}
