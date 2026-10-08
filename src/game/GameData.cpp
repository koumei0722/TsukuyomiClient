#include "game/GameData.h"

#include <format>

#include <Windows.h>

#include "core/Logger.h"
#include "core/Notice.h"
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
    {
        std::lock_guard lock(m_mutex);
        m_view = view;
        m_valid = true;
    }
    m_viewAt.store(GetTickCount64(), std::memory_order_relaxed);
}

unsigned long long GameData::msSinceView() const
{
    const unsigned long long at = m_viewAt.load(std::memory_order_relaxed);
    if (at == 0) {
        return ~0ULL;
    }
    const unsigned long long now = GetTickCount64();
    return now > at ? now - at : 0;
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

bool GameData::onLevelTick(const void* level, unsigned long thread, unsigned long clientThread)
{
    if (level == nullptr) {
        return false;
    }
    const bool onClientThread = clientThread == 0 || thread == clientThread;
    const unsigned long long now = GetTickCount64();
    bool known = false;
    for (int i = 0; i < 2 && !known; ++i) {
        if (m_tickLevels[i].load(std::memory_order_relaxed) == level) {
            m_tickLevelAt[i].store(now, std::memory_order_relaxed);
            known = true;
        }
    }
    if (!known) {
        const int slot = m_tickLevelAt[0].load(std::memory_order_relaxed) <= m_tickLevelAt[1].load(std::memory_order_relaxed)
            ? 0 : 1;
        m_tickLevels[slot].store(level, std::memory_order_relaxed);
        m_tickLevelAt[slot].store(now, std::memory_order_relaxed);
        if (m_tickLevelLogs.fetch_add(1, std::memory_order_relaxed) < 8) {
            log().info(L"GameData: a level ticks at {:#x} on thread {} (client thread {})",
                       reinterpret_cast<std::uintptr_t>(level), thread, clientThread);
        }
    }
    return !onClientThread && integratedServer();
}

bool GameData::integratedServer() const
{
    const unsigned long long now = GetTickCount64();
    for (int i = 0; i < 2; ++i) {
        const unsigned long long at = m_tickLevelAt[i].load(std::memory_order_relaxed);
        if (at == 0 || (now > at && now - at > 2000) || m_tickLevels[i].load(std::memory_order_relaxed) == nullptr) {
            return false;
        }
    }
    return m_tickLevels[0].load(std::memory_order_relaxed) != m_tickLevels[1].load(std::memory_order_relaxed);
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
    m_playerSerial.fetch_add(1, std::memory_order_acq_rel);
}

unsigned long long GameData::playerSerial() const
{
    return m_playerSerial.load(std::memory_order_acquire);
}

void* GameData::player() const
{
    return m_player.load(std::memory_order_relaxed);
}

namespace {

bool gdCopyGuarded(const void* address, void* out, std::size_t size)
{
    return memory::copyGuarded(address, out, size);
}

}

bool GameData::isPlayerEntity(const void* entityContext) const
{
    const void* const self = m_player.load(std::memory_order_relaxed);
    if (entityContext == nullptr || self == nullptr) {
        return false;
    }
    char own[0x1c]{};
    char ctx[0x1c]{};
    if (!gdCopyGuarded(self, own, sizeof(own)) || !gdCopyGuarded(entityContext, ctx, sizeof(ctx))) {
        return false;
    }
    void* ownRegistry = nullptr;
    unsigned int ownId = 0;
    std::memcpy(&ownRegistry, own + 0x10, sizeof(ownRegistry));
    std::memcpy(&ownId, own + 0x18, sizeof(ownId));
    if (ownRegistry == nullptr) {
        return false;
    }

    const auto matches = [&](std::ptrdiff_t registryAt, std::ptrdiff_t idAt) {
        void* registry = nullptr;
        unsigned int id = 0;
        std::memcpy(&registry, ctx + registryAt, sizeof(registry));
        std::memcpy(&id, ctx + idAt, sizeof(id));
        return registry == ownRegistry && id == ownId;
    };
    return matches(0x10, 0x18) || matches(0x08, 0x10);
}

bool GameData::playerFeet(float& outX, float& outY, float& outZ) const
{
    void* const self = m_player.load(std::memory_order_relaxed);
    if (self == nullptr) {
        return false;
    }
    const auto* const at = static_cast<const char*>(self) + kPlayerPositionOffset;
    float position[3]{};
    if (!gdCopyGuarded(at, position, sizeof(position))) {
        return false;
    }
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

bool gdCopyComponent(const void* self, std::uint32_t typeId, std::size_t stride, void* out)
{
    __try {
        const auto* const base = static_cast<const char*>(self);
        const std::uintptr_t registry = *reinterpret_cast<const std::uintptr_t*>(base + 0x10);
        const std::uint32_t id = *reinterpret_cast<const std::uint32_t*>(base + 0x18);
        if (registry == 0) {
            return false;
        }

        const std::uintptr_t first = *reinterpret_cast<const std::uintptr_t*>(registry + 0x68);
        const std::uintptr_t last = *reinterpret_cast<const std::uintptr_t*>(registry + 0x70);
        if (first == 0 || last <= first || (last - first) % 0x20 != 0) {
            return false;
        }
        constexpr std::uintptr_t kMaxEntries = 4096;
        const std::uintptr_t entries = (last - first) / 0x20;
        const std::uintptr_t count = entries < kMaxEntries ? entries : kMaxEntries;
        std::uintptr_t store = 0;
        for (std::uintptr_t i = 0; i < count; ++i) {
            const std::uintptr_t entry = first + i * 0x20;
            if (*reinterpret_cast<const std::uint32_t*>(entry + 0x08) == typeId) {
                store = *reinterpret_cast<const std::uintptr_t*>(entry + 0x10);
                break;
            }
        }
        if (store == 0) {
            return false;
        }

        const std::uint32_t low = id & 0x3ffffu;
        const std::uint32_t page = low >> 11;
        const std::uintptr_t pagesBegin = *reinterpret_cast<const std::uintptr_t*>(store + 0x08);
        const std::uintptr_t pagesEnd = *reinterpret_cast<const std::uintptr_t*>(store + 0x10);
        if (pagesBegin == 0 || pagesEnd <= pagesBegin
            || page >= (pagesEnd - pagesBegin) / sizeof(std::uintptr_t)) {
            return false;
        }
        const std::uintptr_t sparse =
            *reinterpret_cast<const std::uintptr_t*>(pagesBegin + page * sizeof(std::uintptr_t));
        if (sparse == 0) {
            return false;
        }
        const std::uint32_t packed =
            *reinterpret_cast<const std::uint32_t*>(sparse + (low & 0x7ffu) * 4);
        if (((id & 0xfffc0000u) ^ packed) > 0x3fffeu) {
            return false;
        }

        const std::uintptr_t table = *reinterpret_cast<const std::uintptr_t*>(store + 0x50);
        if (table == 0) {
            return false;
        }
        const std::uintptr_t dense =
            *reinterpret_cast<const std::uintptr_t*>(table + ((packed >> 4) & 0x3ff8u));
        if (dense == 0) {
            return false;
        }
        std::memcpy(out, reinterpret_cast<const void*>(dense + (packed & 0x7fu) * stride), stride);
        return true;
    } __except (gdAccessViolationFilter(GetExceptionCode())) {
        return false;
    }
}

}

bool GameData::copyComponent(const void* actor, std::uint32_t typeId, std::size_t stride, void* out)
{
    return gdCopyComponent(actor, typeId, stride, out);
}

bool GameData::playerBoxFeet(float& outX, float& outY, float& outZ) const
{
    void* const self = m_player.load(std::memory_order_relaxed);
    if (self == nullptr) {
        return false;
    }
    float box[kAabbShapeStride / sizeof(float)]{};
    if (!gdCopyComponent(self, kAabbShapeTypeId, kAabbShapeStride, box)) {
        return false;
    }
    constexpr float kWorldLimit = 3.0e7f;
    for (const float value : box) {
        if (!std::isfinite(value) || std::fabs(value) > kWorldLimit) {
            return false;
        }
    }

    const float width = box[3] - box[0];
    const float height = box[4] - box[1];
    const float depth = box[5] - box[2];
    constexpr float kTolerance = 0.01f;
    if (width <= 0.0f || height <= 0.0f || std::fabs(width - depth) > kTolerance
        || std::fabs(width - box[6]) > kTolerance || std::fabs(height - box[7]) > kTolerance) {
        return false;
    }

    outX = (box[0] + box[3]) * 0.5f;
    outY = box[1];
    outZ = (box[2] + box[5]) * 0.5f;
    return true;
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
    const void* const vtable = playerVtable();
    void* head = nullptr;
    if (vtable != nullptr && (!gdReadPointer(p, head) || head != vtable)) {
        return false;
    }
    return true;
}

bool GameData::findPlayerFromClient(void* clientInstance)
{
    if (clientInstance == nullptr) {
        return false;
    }
    const void* const vtable = playerVtable();
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
        m_playerSerial.fetch_add(1, std::memory_order_acq_rel);
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

    const void* const vtable = playerVtable();
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
    m_playerSerial.fetch_add(1, std::memory_order_acq_rel);
    return true;
}

namespace {

bool readPlayerVec3(const void* player, std::ptrdiff_t offset, float& outX, float& outY, float& outZ)
{
    if (player == nullptr) {
        return false;
    }
    const auto* const at = static_cast<const char*>(player) + offset;
    float position[3]{};
    if (!gdCopyGuarded(at, position, sizeof(position))) {
        return false;
    }
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

}

bool GameData::rawPosOf(const void* player, float& outX, float& outY, float& outZ)
{
    return readPlayerVec3(player, kPlayerPositionOffset, outX, outY, outZ);
}

bool GameData::previousPosOf(const void* player, float& outX, float& outY, float& outZ)
{
    return readPlayerVec3(player, kPlayerPreviousPositionOffset, outX, outY, outZ);
}

bool GameData::setPlayerAlt(void* player)
{
    if (!isServerPlayer(player)) {
        return false;
    }
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    if (!rawPosOf(player, x, y, z)) {
        return false;
    }
    constexpr double kAltMaxDistance = 8.0;
    float px = 0.0f;
    float py = 0.0f;
    float pz = 0.0f;
    if (rawPlayerPos(px, py, pz)) {
        const double distance = std::hypot(x - px, y - py, z - pz);
        if (distance > kAltMaxDistance) {
            return false;
        }
        void* const current = m_playerAlt.load(std::memory_order_acquire);
        float cx = 0.0f;
        float cy = 0.0f;
        float cz = 0.0f;
        if (current != nullptr && current != player && isServerPlayer(current) && rawPosOf(current, cx, cy, cz)
            && std::hypot(cx - px, cy - py, cz - pz) < distance) {
            return false;
        }
    }
    if (m_playerAlt.exchange(player, std::memory_order_acq_rel) != player
        && m_playerAltLogs.fetch_add(1, std::memory_order_relaxed) < 6) {
        log().info(L"GameData: the server-side player of this local world is at {:#x}",
                   reinterpret_cast<std::uintptr_t>(player));
    }
    return true;
}

void* GameData::playerAlt() const
{
    void* current = m_playerAlt.load(std::memory_order_acquire);
    if (current == nullptr) {
        return nullptr;
    }
    if (!isServerPlayer(current)) {
        m_playerAlt.compare_exchange_strong(current, nullptr, std::memory_order_acq_rel);
        return nullptr;
    }
    return current;
}

void GameData::onScansReady()
{
    constexpr std::size_t kServerPlayerVtableDisp = 3;
    const void* vtable = nullptr;
    if (std::byte* const ref = Scanner::instance().address(Target::ServerPlayerVtableRef); ref != nullptr) {
        vtable = memory::ripTarget(ref, kServerPlayerVtableDisp);
    }
    m_serverPlayerVtable.store(vtable, std::memory_order_release);
    const auto* const exe = reinterpret_cast<const std::byte*>(GetModuleHandleW(nullptr));

    constexpr std::size_t kPlayerVtableDisp = 3;
    const void* playerVt = nullptr;
    if (std::byte* const ref = Scanner::instance().address(Target::PlayerVtableRef); ref != nullptr) {
        playerVt = memory::ripTarget(ref, kPlayerVtableDisp);
    }
    m_playerVtable.store(playerVt, std::memory_order_release);
    if (playerVt == nullptr) {
        notice::failOnce("GameData.playerVtable",
                         L"GameData: the Player vtable was not found; the local player is only picked up through /gamemode",
                         "Most features stay idle until you run /gamemode once: the player type could not be found");
    }
    if (vtable != nullptr) {
        log().info(L"GameData: telling the server-side player apart by the ServerPlayer type (vtable rva {:#x})",
                   static_cast<std::uintptr_t>(static_cast<const std::byte*>(vtable) - exe));
    } else {
        notice::failOnce("GameData.serverPlayerVtable",
                         L"GameData: the ServerPlayer vtable was not found; the server-side player of a local world is "
                         L"not used (AutoTool is off, and HandRestock does not refill the server-side copy)",
                         "AutoTool is off: the server player type could not be found");
    }
}

bool GameData::knowsServerPlayer() const
{
    return m_serverPlayerVtable.load(std::memory_order_acquire) != nullptr;
}

bool GameData::isServerPlayer(const void* player) const
{
    const void* const vtable = m_serverPlayerVtable.load(std::memory_order_acquire);
    void* head = nullptr;
    return vtable != nullptr && memory::plausiblePointer(player) && gdReadPointer(player, head) && head == vtable;
}

}
