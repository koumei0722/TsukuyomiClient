#include "modules/LightOverlay.h"

#include "config/Config.h"
#include "core/Logger.h"
#include "core/Notice.h"
#include "game/BlockRegistry.h"
#include "game/BlockWrite.h"
#include "game/DebugScreenWorld.h"
#include "game/FlexibleLogic.h"
#include "game/GameData.h"
#include "memory/Scanner.h"
#include "render/WorldMesh.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <utility>

namespace tsukuyomi {

namespace {

using IsValidSpawnFn = bool(__fastcall*)(const void* type, void* source, const void* block, const std::int32_t* pos,
                                         void* entityType);

constexpr std::size_t kEntityTypeBytes = 0x80;
constexpr std::size_t kEntityTypeStringCapacity = 0x28;

void makeEntityType(std::byte* out)
{
    std::memset(out, 0, kEntityTypeBytes);
    const std::uint64_t capacity = 15;
    std::memcpy(out + kEntityTypeStringCapacity, &capacity, sizeof(capacity));
}

bool callIsValidSpawn(IsValidSpawnFn fn, const void* type, void* source, const void* block, const std::int32_t* pos,
                      void* entityType, bool& out, DWORD& code)
{
    __try {
        out = fn(type, source, block, pos, entityType);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        code = GetExceptionCode();
        return false;
    }
}

constexpr double kPixel = 0.075;
constexpr double kFaceLift = 0.01;
constexpr double kDigitLift = 0.02;
constexpr double kFaceInset = 0.03;
constexpr float kFaceAlpha = 0.3F;

void colorOf(lightlevel::Spawn spawn, float rgba[4])
{
    switch (spawn) {
    case lightlevel::Spawn::Never: rgba[0] = 0.25F; rgba[1] = 1.0F; rgba[2] = 0.25F; break;
    case lightlevel::Spawn::NightOnly: rgba[0] = 1.0F; rgba[1] = 1.0F; rgba[2] = 0.2F; break;
    case lightlevel::Spawn::Always: rgba[0] = 1.0F; rgba[1] = 0.25F; rgba[2] = 0.25F; break;
    }
    rgba[3] = 1.0F;
}

}

LightOverlay& LightOverlay::instance()
{
    static LightOverlay module;
    return module;
}

bool LightOverlay::available() const
{
    const auto& scanner = Scanner::instance();
    return scanner.found(Target::BlockTypeIsValidSpawn) && scanner.found(Target::PlayerView)
           && scanner.found(Target::BeRenderLoop) && scanner.found(Target::NameTagStage);
}

MenuItem LightOverlay::buildMenu()
{
    std::vector<MenuItem> children{enabledItem(), toggleKeyItem()};
    children.push_back(menu::number(
        L"Range", [this] { return static_cast<float>(m_range.load(std::memory_order_relaxed)); },
        [this](float value) {
            m_range.store(std::clamp(static_cast<int>(std::lround(value)), kMinRange, kMaxRange),
                          std::memory_order_relaxed);
        },
        true, static_cast<float>(kMinRange), static_cast<float>(kMaxRange)));
    children.push_back(menu::number(
        L"Vertical range", [this] { return static_cast<float>(m_vertical.load(std::memory_order_relaxed)); },
        [this](float value) {
            m_vertical.store(std::clamp(static_cast<int>(std::lround(value)), kMinVertical, kMaxVertical),
                             std::memory_order_relaxed);
        },
        true, static_cast<float>(kMinVertical), static_cast<float>(kMaxVertical)));
    auto item = menu::submenu(name(), std::move(children));
    item.available = [this] { return available(); };
    item.isOn = [this] { return enabled(); };
    return item;
}

void LightOverlay::loadConfig(const nlohmann::json& section)
{
    Module::loadConfig(section);
    m_range.store(std::clamp(Config::getInt(section, "range", kDefaultRange), kMinRange, kMaxRange),
                  std::memory_order_relaxed);
    m_vertical.store(std::clamp(Config::getInt(section, "vertical", kDefaultVertical), kMinVertical, kMaxVertical),
                     std::memory_order_relaxed);
}

void LightOverlay::saveConfig(nlohmann::json& section) const
{
    Module::saveConfig(section);
    section["range"] = m_range.load(std::memory_order_relaxed);
    section["vertical"] = m_vertical.load(std::memory_order_relaxed);
}

void LightOverlay::onEnabledChanged(bool enabled)
{
    if (!enabled) worldmesh::setDebugFills(worldmesh::LineSource::LightOverlay, {});
}

void LightOverlay::shutdown()
{
    worldmesh::setDebugFills(worldmesh::LineSource::LightOverlay, {});
}

void LightOverlay::clear()
{
    if (m_drawn) worldmesh::setDebugFills(worldmesh::LineSource::LightOverlay, {});
    m_drawn = false;
    m_cells.clear();
    m_lastScan = 0;
    m_facing = -1;
    m_lastFeet[0] = m_lastFeet[1] = m_lastFeet[2] = INT32_MIN;
}

int LightOverlay::materialOf(const void* block)
{
    if (const auto found = m_material.find(block); found != m_material.end()) return found->second;
    bool blocksMotion = false;
    bool liquid = false;
    int answer = -1;
    if (blocks::materialFlags(block, blocksMotion, liquid)) answer = (blocksMotion ? kBlocksMotion : 0) | (liquid ? kLiquid : 0);
    m_material.emplace(block, static_cast<std::int8_t>(answer));
    return answer;
}

int LightOverlay::passable(const void* block, const void* extra)
{
    const int main = materialOf(block);
    if (main < 0) return -1;
    if (main != 0) return 0;
    if (extra == nullptr) return 1;
    const int second = materialOf(extra);
    if (second < 0) return -1;
    return (second & kLiquid) != 0 ? 0 : 1;
}

int LightOverlay::spawnableFloor(const void* block, const std::int32_t pos[3])
{
    if (const auto found = m_spawnable.find(block); found != m_spawnable.end()) return found->second;
    const auto fn = Scanner::instance().addressAs<IsValidSpawnFn>(Target::BlockTypeIsValidSpawn);
    const void* const type = blocks::legacyOfFast(block);
    void* const source = blockwrite::renderRegion();
    if (fn == nullptr || type == nullptr || source == nullptr) return -1;
    alignas(16) std::byte entityType[kEntityTypeBytes];
    makeEntityType(entityType);
    bool answer = false;
    DWORD code = 0;
    if (!callIsValidSpawn(fn, type, source, block, pos, entityType, answer, code)) {
        m_broken = true;
        notice::failOnce("LightOverlay.isValidSpawn",
                         std::format(L"LightOverlay: BlockType::isValidSpawn faulted (exception {:08X}); "
                                     L"the overlay is stopped",
                                     code),
                         "LightOverlay stopped: asking the game whether mobs can spawn failed");
        return -1;
    }
    m_spawnable.emplace(block, static_cast<std::int8_t>(answer ? 1 : 0));
    if (!answer && m_rejectedNames.size() < kMaxRejectedNames) {
        char text[blocks::kStateTextBytes] = {};
        if (blocks::nameOfBlock(block, text, sizeof(text))
            && std::find(m_rejectedNames.begin(), m_rejectedNames.end(), text) == m_rejectedNames.end()) {
            m_rejectedNames.emplace_back(text);
        }
    }
    return answer ? 1 : 0;
}

void LightOverlay::scan(int feetX, int feetY, int feetZ)
{
    const int range = m_range.load(std::memory_order_relaxed);
    const int vertical = m_vertical.load(std::memory_order_relaxed);
    const int bottom = feetY - vertical - 1;
    const int count = 2 * vertical + 2;
    std::vector<debugworld::ColumnCell> column(static_cast<std::size_t>(count));
    std::vector<Cell> cells;
    for (int dx = -range; dx <= range && !m_broken; ++dx) {
        for (int dz = -range; dz <= range && !m_broken; ++dz) {
            const int x = feetX + dx;
            const int z = feetZ + dz;
            if (!debugworld::readColumn(x, z, bottom, count, column.data())) continue;
            for (int i = 1; i < count && !m_broken; ++i) {
                const debugworld::ColumnCell& space = column[static_cast<std::size_t>(i)];
                const debugworld::ColumnCell& floor = column[static_cast<std::size_t>(i - 1)];
                if (!space.ok || !floor.ok || floor.aboveTop || floor.block == nullptr) continue;
                if (!space.aboveTop && passable(space.block, space.extra) != 1) continue;
                if (materialOf(floor.block) != kBlocksMotion) continue;
                const std::int32_t pos[3] = {x, bottom + i - 1, z};
                if (spawnableFloor(floor.block, pos) != 1) continue;
                cells.push_back({x, pos[1], z, space.blockLight, lightlevel::classify(space.blockLight, space.skyLight)});
            }
        }
    }
    if (cells.size() > kMaxCells) {
        auto distance = [&](const Cell& c) {
            const int dx = c.x - feetX, dy = c.y - feetY, dz = c.z - feetZ;
            return dx * dx + dy * dy + dz * dz;
        };
        std::nth_element(cells.begin(), cells.begin() + kMaxCells, cells.end(),
                         [&](const Cell& a, const Cell& b) { return distance(a) < distance(b); });
        if (!m_loggedCap) {
            m_loggedCap = true;
            log().info(L"LightOverlay: {} cells found, drawing the nearest {} (logged once)", cells.size(), kMaxCells);
        }
        cells.resize(kMaxCells);
    }
    m_cells = std::move(cells);
    if (!m_summaryDone && !m_cells.empty() && m_summaryLogs < kMaxSummaryLogs) {
        m_summaryDone = true;
        int counts[3] = {};
        for (const Cell& cell : m_cells) ++counts[static_cast<int>(cell.spawn)];
        std::wstring rejected;
        for (const std::string& name : m_rejectedNames) {
            if (!rejected.empty()) rejected += L", ";
            rejected += std::wstring(name.begin(), name.end());
        }
        std::wstring summary = std::format(L"{} numbers (green {} / yellow {} / red {}); floors the game rejected: {}",
                                           m_cells.size(), counts[0], counts[1], counts[2],
                                           rejected.empty() ? L"-" : rejected);
        if (summary != m_lastSummary) {
            ++m_summaryLogs;
            log().info(L"LightOverlay: first scan in this world: {} (logged once per world, {} of {})", summary,
                       m_summaryLogs, kMaxSummaryLogs);
            m_lastSummary = std::move(summary);
        }
    }
}

void LightOverlay::rebuildFills(int facing)
{
    std::vector<worldmesh::DebugFill> fills;
    fills.reserve(m_cells.size() * 9);
    for (const Cell& cell : m_cells) {
        float rgba[4] = {};
        colorOf(cell.spawn, rgba);
        worldmesh::DebugFill face{};
        const lightlevel::Quad faceQuad = lightlevel::faceQuad(cell.x, cell.y, cell.z, kFaceLift, kFaceInset);
        std::memcpy(face.p, faceQuad.p, sizeof(face.p));
        std::copy_n(rgba, 3, face.rgba);
        face.rgba[3] = kFaceAlpha;
        face.blend = true;
        fills.push_back(face);
        for (const lightlevel::Quad& quad : lightlevel::numberQuads(cell.light, cell.x, cell.y, cell.z, facing, kPixel, kDigitLift)) {
            worldmesh::DebugFill fill{};
            std::memcpy(fill.p, quad.p, sizeof(fill.p));
            std::copy_n(rgba, 4, fill.rgba);
            fill.blend = true;
            fills.push_back(fill);
        }
    }
    worldmesh::setDebugFills(worldmesh::LineSource::LightOverlay, std::move(fills));
    m_drawn = true;
}

void LightOverlay::onPlayerViewUpdate()
{
    if (!enabled() || m_broken) {
        clear();
        return;
    }
    const std::uint64_t generation = blockwrite::regionGeneration();
    if (generation != m_generation) {
        m_generation = generation;
        m_material.clear();
        m_spawnable.clear();
        m_rejectedNames.clear();
        m_summaryDone = false;
        clear();
    }
    float x = 0.0F, y = 0.0F, z = 0.0F;
    if (!GameData::instance().playerBoxFeet(x, y, z)) {
        clear();
        return;
    }
    const int feet[3] = {static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)), static_cast<int>(std::floor(z))};
    const int facing = flexible::horizontalFromYaw(GameData::instance().playerView().yaw);
    const std::uint64_t now = GetTickCount64();
    const bool moved = feet[0] != m_lastFeet[0] || feet[1] != m_lastFeet[1] || feet[2] != m_lastFeet[2];
    bool changed = false;
    if (now - m_lastScan >= (moved ? 50 : kScanIntervalMs)) {
        m_lastScan = now;
        std::copy_n(feet, 3, m_lastFeet);
        scan(feet[0], feet[1], feet[2]);
        changed = true;
        if (m_broken) {
            clear();
            return;
        }
    }
    if (changed || facing != m_facing) {
        m_facing = facing;
        rebuildFills(facing);
    }
}

}
