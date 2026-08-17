#include "modules/Scaffold.h"

#include "config/Config.h"
#include "core/Logger.h"
#include "game/GameData.h"
#include "hooks/Detours.h"
#include "input/Foreground.h"
#include "memory/Scanner.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace tsukuyomi {

Scaffold& Scaffold::instance()
{
    static Scaffold module;
    return module;
}

bool Scaffold::available() const
{

    const Scanner& scanner = Scanner::instance();
    return scanner.found(Target::BuildBlock) && scanner.found(Target::PlayerView);
}

const wchar_t* Scaffold::patternName() const
{
    switch (m_pattern) {
    case Pattern::Square3: return L"Square 3x3";
    case Pattern::Square5: return L"Square 5x5";
    case Pattern::Square7: return L"Square 7x7";
    case Pattern::Cross:
    default:               return L"Cross (5)";
    }
}

int Scaffold::squareRadius() const
{
    switch (m_pattern) {
    case Pattern::Square3: return 1;
    case Pattern::Square5: return 2;
    case Pattern::Square7: return 3;
    case Pattern::Cross:
    default:               return 0;
    }
}

const wchar_t* Scaffold::heightName() const
{
    switch (m_height) {
    case Height::Manual:   return L"Manual";
    case Height::OnEnable: return L"On enable";
    case Height::Follow:
    default:               return L"Follow";
    }
}

void Scaffold::cyclePattern()
{
    switch (m_pattern) {
    case Pattern::Cross:   m_pattern = Pattern::Square3; break;
    case Pattern::Square3: m_pattern = Pattern::Square5; break;
    case Pattern::Square5: m_pattern = Pattern::Square7; break;
    case Pattern::Square7:
    default:               m_pattern = Pattern::Cross; break;
    }
}

void Scaffold::cycleHeight()
{
    switch (m_height) {
    case Height::Follow:   m_height = Height::Manual; break;
    case Height::Manual:   m_height = Height::OnEnable; break;
    case Height::OnEnable:
    default:               m_height = Height::Follow; break;
    }

    if (enabled()) {
        captureHeight();
    } else {
        m_hasCapturedY = false;
    }
}

void Scaffold::captureHeight()
{
    m_hasCapturedY = false;

    if (m_height != Height::OnEnable) {
        return;
    }

    const GameData& data = GameData::instance();
    if (!data.hasPlayerView()) {
        return;
    }

    float footY = 0.0f;
    if (!data.playerFeetY(footY)) {
        footY = data.playerView().y - GameData::kEyeHeight;
    }
    m_capturedY = static_cast<int>(std::floor(footY)) - 1;
    m_hasCapturedY = true;
    log().info(L"Scaffold: height locked to Y {}", m_capturedY);
}

bool Scaffold::resolveY(float footY, int& outY) const
{
    switch (m_height) {
    case Height::Manual:
        outY = m_manualY;
        return true;

    case Height::OnEnable:
        if (!m_hasCapturedY) {
            return false;
        }
        outY = m_capturedY;
        return true;

    case Height::Follow:
    default:

        outY = static_cast<int>(std::floor(footY)) - 1;
        return true;
    }
}

int Scaffold::buildTargets(const BlockPos& center, BlockPos (&targets)[kMaxTargets]) const
{

    int count = 0;
    targets[count++] = center;

    if (const int radius = squareRadius(); radius > 0) {
        for (int dx = -radius; dx <= radius; ++dx) {
            for (int dz = -radius; dz <= radius; ++dz) {
                if (dx == 0 && dz == 0) {
                    continue;
                }
                targets[count++] = BlockPos{center.x + dx, center.y, center.z + dz};
            }
        }
        return count;
    }

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

    const PlayerView view = data.playerView();

    float footX = view.x;
    float footY = view.y - GameData::kEyeHeight;
    float footZ = view.z;
    if (!data.playerFeet(footX, footY, footZ)) {
        footX = view.x;
        footY = view.y - GameData::kEyeHeight;
        footZ = view.z;
    }

    int planeY = 0;
    if (!resolveY(footY, planeY)) {
        return;
    }
    if (planeY < kMinY || planeY > kMaxY) {
        return;
    }

    const BlockPos center{static_cast<int>(std::floor(footX)), planeY,
                          static_cast<int>(std::floor(footZ))};

    const bool moved = !m_hasLastCenter || center.x != m_lastCenter.x
                       || center.y != m_lastCenter.y || center.z != m_lastCenter.z;

    const Clock::time_point now = Clock::now();
    if (!moved && now < m_nextResend) {
        return;
    }
    m_nextResend = now + std::chrono::milliseconds(kResendIntervalMs);
    m_lastCenter = center;
    m_hasLastCenter = true;

    BlockPos targets[kMaxTargets];
    const int count = buildTargets(center, targets);

    int placed = 0;
    m_placing = true;
    for (int i = 0; i < count; ++i) {

        BlockPos target = targets[i];
        if (hooks::callBuildBlock(gameMode, &target, faceTowardCenter(target, center), 0)) {
            ++placed;
        }
    }
    m_placing = false;

    if (placed == 0 && !moved) {
        return;
    }

    if (now >= m_nextLog) {
        m_nextLog = now + std::chrono::milliseconds(kLogIntervalMs);
        log().info(L"Scaffold: {} Y {} ({}) at ({}, {}) {}/{} placed", patternName(), planeY,
                   heightName(), center.x, center.z, placed, count);
    }
}

void Scaffold::onPlayerViewUpdate()
{

    if (!enabled() || m_placing) {
        return;
    }

    if (m_height == Height::OnEnable && !m_hasCapturedY) {
        captureHeight();
    }

    placeAll();
}

MenuItem Scaffold::buildMenu()
{
    std::vector<MenuItem> children;
    children.push_back(menu::back());
    children.push_back(enabledItem());
    children.push_back(toggleKeyItem());

    children.push_back(menu::choice(
        L"Pattern", {L"Cross (5)", L"Square 3x3", L"Square 5x5", L"Square 7x7"},
        [this] { return static_cast<int>(m_pattern); },
        [this](int at) { m_pattern = static_cast<Pattern>(std::clamp(at, 0, 3)); }));
    children.push_back(menu::choice(
        L"Height", {L"Follow", L"Manual", L"On enable"},
        [this] { return static_cast<int>(m_height); }, [this](int at) {
            m_height = static_cast<Height>(std::clamp(at, 0, 2));

            if (enabled()) {
                captureHeight();
            } else {
                m_hasCapturedY = false;
            }
        }));

    MenuItem fixedY = menu::number(
        L"Fixed Y", [this] { return static_cast<float>(m_manualY); },
        [this](float value) {
            m_manualY = std::clamp(static_cast<int>(value), kMinY, kMaxY);
            log().info(L"Scaffold: fixed Y set to {}", m_manualY);
        },
        true, static_cast<float>(kMinY), static_cast<float>(kMaxY));

    fixedY.available = [this] { return m_height == Height::Manual; };
    children.push_back(std::move(fixedY));

    MenuItem item = menu::submenu(name(), std::move(children));
    item.available = [this] { return available(); };
    item.isOn = [this] { return enabled(); };
    return item;
}

void Scaffold::loadConfig(const nlohmann::json& section)
{
    Module::loadConfig(section);

    const int pattern =
        std::clamp(Config::getInt(section, "pattern", static_cast<int>(Pattern::Cross)), 0, 3);
    m_pattern = static_cast<Pattern>(pattern);

    const int height =
        std::clamp(Config::getInt(section, "height", static_cast<int>(Height::Follow)), 0, 2);
    m_height = static_cast<Height>(height);

    m_manualY = std::clamp(Config::getInt(section, "fixedY", 64), kMinY, kMaxY);
}

void Scaffold::saveConfig(nlohmann::json& section) const
{
    Module::saveConfig(section);
    section["pattern"] = static_cast<int>(m_pattern);
    section["height"] = static_cast<int>(m_height);
    section["fixedY"] = m_manualY;
}

void Scaffold::onEnabledChanged(bool enabled)
{
    m_hasLastCenter = false;
    m_warnedNoGameMode = false;

    m_nextLog = Clock::time_point{};

    if (!enabled) {
        m_hasCapturedY = false;
        return;
    }

    captureHeight();
}

}
