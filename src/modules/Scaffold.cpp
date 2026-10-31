#include "modules/Scaffold.h"

#include "core/Logger.h"
#include "core/Notice.h"
#include "game/BlockRegistry.h"
#include "game/DebugScreenWorld.h"
#include "game/GameData.h"
#include "hooks/Detours.h"
#include "input/Foreground.h"
#include "memory/Scanner.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace tsukuyomi {

namespace {
constexpr float kClickAtCorner[3]{0.0f, 0.0f, 0.0f};
}

Scaffold& Scaffold::instance()
{
    static Scaffold module;
    return module;
}

bool Scaffold::available() const
{
    const Scanner& scanner = Scanner::instance();
    return scanner.found(Target::BuildBlock) && scanner.found(Target::PlayerView)
        && scanner.found(Target::ServerLevelTick) && scanner.found(Target::HitResultAssign);
}

bool Scaffold::readFeet(float& outX, float& outY, float& outZ) const
{
    if (GameData::instance().playerBoxFeet(outX, outY, outZ)) {
        m_feetMisses = 0;
        return true;
    }
    if (++m_feetMisses == kFeetMissNotice) {
        notice::failOnce("Scaffold.hitbox",
                         L"Scaffold: the hitbox of the player could not be read for " + std::to_wstring(kFeetMissNotice)
                             + L" ticks in a row; no blocks are placed",
                         "Scaffold is not placing blocks: the hitbox of the player could not be read");
    }
    return false;
}

int Scaffold::buildTargets(const BlockPos& center, BlockPos (&targets)[kMaxTargets]) const
{
    int count = 0;
    targets[count++] = center;

    static constexpr int kDx[] = {1, -1, 0, 0};
    static constexpr int kDz[] = {0, 0, 1, -1};
    for (int i = 0; i < 4; ++i) {
        targets[count++] = BlockPos{center.x + kDx[i], center.y, center.z + kDz[i]};
    }
    return count;
}

unsigned char Scaffold::faceTowardCenter(const BlockPos& pos, const BlockPos& center)
{
    if (pos.x > center.x) { return 4; }
    if (pos.x < center.x) { return 5; }
    if (pos.z > center.z) { return 2; }
    if (pos.z < center.z) { return 3; }

    return 4;
}

bool Scaffold::alreadyFilled(const BlockPos& pos)
{
    const std::int32_t at[3]{pos.x, pos.y, pos.z};
    const void* const block = debugworld::worldBlockAt(at);
    bool blocksMotion = false;
    bool liquid = false;
    return block != nullptr && blocks::materialFlags(block, blocksMotion, liquid) && blocksMotion;
}

void Scaffold::placeAll()
{
    const GameData& data = GameData::instance();

    void* const gameMode = data.gameMode();
    if (gameMode == nullptr) {
        if (!m_warnedNoGameMode) {
            m_warnedNoGameMode = true;
            log().warn(L"Scaffold: place one block by hand first (game mode not captured yet)");
        }
        return;
    }

    if (!data.hasPlayerView() || !input::isInGameplay()) {
        return;
    }

    float footX = 0.0f;
    float footY = 0.0f;
    float footZ = 0.0f;
    if (!readFeet(footX, footY, footZ)) {
        return;
    }

    const int planeY = static_cast<int>(std::floor(footY)) - 1;
    if (planeY < kMinY || planeY > kMaxY) {
        return;
    }

    const BlockPos center{static_cast<int>(std::floor(footX)), planeY,
                          static_cast<int>(std::floor(footZ))};

    const bool moved = !m_hasLastCenter || center.x != m_lastCenter.x
                       || center.y != m_lastCenter.y || center.z != m_lastCenter.z;

    m_lastCenter = center;
    m_hasLastCenter = true;

    BlockPos targets[kMaxTargets];
    const int count = buildTargets(center, targets);

    int placed = 0;
    int filled = 0;
    constexpr bool simTick = false;
    m_placing = true;
    for (int i = 0; i < count; ++i) {
        const BlockPos& target = targets[i];
        if (alreadyFilled(target)) {
            ++filled;
            continue;
        }
        std::int32_t at[3]{target.x, target.y, target.z};
        if (hooks::callBuildBlockAimedWithoutSwing(gameMode, at, faceTowardCenter(target, center), kClickAtCorner,
                                                   0, simTick)) {
            ++placed;
        }
    }
    m_placing = false;

    if (placed == 0 && !moved) {
        return;
    }

    const Clock::time_point now = Clock::now();
    if (now >= m_nextLog) {
        m_nextLog = now + std::chrono::milliseconds(kLogIntervalMs);
        log().info(L"Scaffold: Y {} at ({}, {}) {}/{} placed, {} already filled (feet {:.3f} from the hitbox)",
                   planeY, center.x, center.z, placed, count, filled, footY);
    }
}

void Scaffold::onClientTick()
{
    if (m_placing) return;
    if (m_resetRequested.exchange(false, std::memory_order_acquire)) {
        m_hasLastCenter = false;
        m_warnedNoGameMode = false;
        m_feetMisses = 0;
        m_nextLog = Clock::time_point{};
    }
    if (!enabled()) return;

    placeAll();
}

MenuItem Scaffold::buildMenu()
{
    std::vector<MenuItem> children;
    children.push_back(enabledItem());
    children.push_back(toggleKeyItem());
    MenuItem item = menu::submenu(name(), std::move(children));
    item.available = [this] { return available(); };
    item.isOn = [this] { return enabled(); };
    return item;
}

void Scaffold::saveConfig(nlohmann::json& section) const
{
    Module::saveConfig(section);
    section.erase("pattern");
    section.erase("height");
    section.erase("fixedY");
}

void Scaffold::onEnabledChanged(bool )
{
    m_resetRequested.store(true, std::memory_order_release);
}

}
