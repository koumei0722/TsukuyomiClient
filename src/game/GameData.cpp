#include "game/GameData.h"

#include <format>

#include <Windows.h>

#include "core/Logger.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"

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

bool GameData::rawPlayerPos(float& outX, float& outY, float& outZ) const
{
    return rawPosOf(m_player.load(std::memory_order_relaxed), outX, outY, outZ);
}

namespace {

int gdAccessViolationFilter(unsigned long code)
{
    return (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR)
               ? EXCEPTION_EXECUTE_HANDLER
               : EXCEPTION_CONTINUE_SEARCH;
}

bool gdReadPointer(const void* at, void*& out)
{
    __try {
        out = *reinterpret_cast<void* const*>(at);
        return true;
    } __except (gdAccessViolationFilter(GetExceptionCode())) {
        return false;
    }
}

}

bool GameData::hasLivePlayer() const
{
    void* const p = m_player.load(std::memory_order_relaxed);
    if (p == nullptr) {
        return false;
    }
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    if (!rawPosOf(p, x, y, z)) {
        return false;
    }
    std::byte* const ref = Scanner::instance().address(Target::PlayerVtableRef);
    if (ref != nullptr) {
        const void* const vtable = memory::ripTarget(ref, 3);
        void* head = nullptr;
        if (vtable != nullptr && (!gdReadPointer(p, head) || head != vtable)) {
            return false;
        }
    }
    return true;
}

bool GameData::findPlayerFromClient(void* clientInstance)
{
    if (clientInstance == nullptr) {
        return false;
    }
    std::byte* const ref = Scanner::instance().address(Target::PlayerVtableRef);
    if (ref == nullptr) {
        return false;
    }
    const void* const vtable = memory::ripTarget(ref, 3);
    if (vtable == nullptr) {
        return false;
    }

    constexpr std::size_t kSpan = 0x8000;
    auto* const base = static_cast<std::byte*>(clientInstance);
    for (std::size_t at = 0; at + sizeof(void*) <= kSpan; at += sizeof(void*)) {
        void* candidate = nullptr;
        if (!gdReadPointer(base + at, candidate) || candidate == nullptr) {
            continue;
        }
        void* head = nullptr;
        if (!gdReadPointer(candidate, head) || head != vtable) {
            continue;
        }
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        if (!rawPosOf(candidate, x, y, z)) {
            continue;
        }
        constexpr float kWorldLimit = 3.0e7f;
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)
            || std::fabs(x) > kWorldLimit || std::fabs(y) > kWorldLimit
            || std::fabs(z) > kWorldLimit) {
            continue;
        }
        m_player.store(candidate, std::memory_order_relaxed);
        return true;
    }
    log().info(L"GameData: no player object inside ClientInstance {:#x} (span {:#x})",
               reinterpret_cast<std::uintptr_t>(clientInstance),
               kSpan);
    return false;
}

bool GameData::adoptPlayerFromEntity(void* entityContext)
{
    if (entityContext == nullptr) {
        return false;
    }
    const unsigned long long now = GetTickCount64();
    const unsigned long long last = m_adoptAt.load(std::memory_order_relaxed);
    if (last != 0 && now - last < 1000) {
        return false;
    }
    m_adoptAt.store(now, std::memory_order_relaxed);

    PlayerView view;
    {
        std::lock_guard lock(m_mutex);
        if (!m_valid) {
            return false;
        }
        view = m_view;
    }

    std::byte* const ref = Scanner::instance().address(Target::PlayerVtableRef);
    if (ref == nullptr) {
        return false;
    }
    const void* const vtable = memory::ripTarget(ref, 3);
    if (vtable == nullptr) {
        return false;
    }

    std::byte* candidate = nullptr;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    for (const std::ptrdiff_t back : {std::ptrdiff_t{0x08}, std::ptrdiff_t{0}}) {
        auto* const guess = static_cast<std::byte*>(entityContext) - back;
        void* head = nullptr;
        if (!gdReadPointer(guess, head) || head != vtable) {
            continue;
        }
        if (!rawPosOf(guess, x, y, z)) {
            continue;
        }
        candidate = guess;
        break;
    }
    if (candidate == nullptr) {
        return false;
    }
    const float dx = x - view.x;
    const float dy = y - view.y;
    const float dz = z - view.z;
    if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(dz)
        || (dx * dx + dy * dy + dz * dz) > 1.0f) {
        return false;
    }
    m_player.store(candidate, std::memory_order_relaxed);
    return true;
}

int GameData::players(void* out[2]) const
{
    int n = 0;
    void* const a = m_player.load(std::memory_order_relaxed);
    void* const b = m_playerAlt.load(std::memory_order_relaxed);
    if (a != nullptr) {
        out[n++] = a;
    }
    if (b != nullptr && b != a) {
        out[n++] = b;
    }
    return n;
}

bool GameData::rawPosOf(const void* player, float& outX, float& outY, float& outZ)
{
    if (player == nullptr) {
        return false;
    }
    const auto* const at = static_cast<const char*>(player) + kPlayerPositionOffset;
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
    outY = position[1];
    outZ = position[2];
    return true;
}

bool GameData::targetPosOf(const void* player, float& outX, float& outY, float& outZ)
{
    if (player == nullptr) {
        return false;
    }
    const auto* const at = static_cast<const char*>(player) + kPlayerTargetPosOffset;
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
    outY = position[1];
    outZ = position[2];
    return true;
}

bool GameData::writeRawPosOf(void* player, float x, float y, float z)
{
    if (player == nullptr) {
        return false;
    }
    const float position[3] = {x, y, z};
    auto* const at = static_cast<char*>(player) + kPlayerPositionOffset;
    if (!memory::isWritable(at, sizeof(float) * 3)) {
        return false;
    }
    std::memcpy(at, position, sizeof(position));

    auto* const target = static_cast<char*>(player) + kPlayerTargetPosOffset;
    if (memory::isWritable(target, kPlayerTargetPosSize)) {
        std::memcpy(target, position, sizeof(position));
        std::memcpy(target + sizeof(position), position, sizeof(position));
    }
    return true;
}

void GameData::setPlayerAlt(void* player)
{
    if (player == nullptr) {
        return;
    }
    const auto* const at = static_cast<const char*>(player) + kPlayerPositionOffset;
    if (!memory::isReadable(at, sizeof(float) * 3)) {
        return;
    }
    float position[3]{};
    std::memcpy(position, at, sizeof(position));
    constexpr float kWorldLimit = 3.0e7f;
    for (const float value : position) {
        if (!std::isfinite(value) || std::fabs(value) > kWorldLimit) {
            return;
        }
    }
    m_playerAlt.store(player, std::memory_order_relaxed);
}

void* GameData::playerAlt() const
{
    return m_playerAlt.load(std::memory_order_relaxed);
}

bool GameData::writeRawPlayerPos(float x, float y, float z)
{
    void* const targets[2] = {m_player.load(std::memory_order_relaxed),
                              m_playerAlt.load(std::memory_order_relaxed)};
    const float position[3] = {x, y, z};
    bool wrote = false;
    for (int i = 0; i < 2; ++i) {
        void* const self = targets[i];
        if (self == nullptr || (i > 0 && self == targets[0])) {
            continue;
        }
        auto* const at = static_cast<char*>(self) + kPlayerPositionOffset;
        if (!memory::isWritable(at, sizeof(float) * 3)) {
            continue;
        }
        std::memcpy(at, position, sizeof(position));
        wrote = true;
    }
    return wrote;
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
