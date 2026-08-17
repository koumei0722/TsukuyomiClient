#include "modules/FastBlockPlacement.h"

#include "config/Config.h"
#include "core/Logger.h"
#include "game/GameData.h"
#include "hooks/Detours.h"
#include "input/Foreground.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace tsukuyomi {

FastBlockPlacement& FastBlockPlacement::instance()
{
    static FastBlockPlacement module;
    return module;
}

bool FastBlockPlacement::available() const
{

    const Scanner& scanner = Scanner::instance();
    return scanner.found(Target::BuildBlock) && scanner.found(Target::PlayerView);
}

const wchar_t* FastBlockPlacement::axisName() const
{
    switch (m_axis) {
    case Axis::X: return L"X";
    case Axis::Z: return L"Z";
    case Axis::Y:
    default:      return L"Y";
    }
}

FastBlockPlacement::BlockPos FastBlockPlacement::shiftByFace(BlockPos pos, unsigned char face)
{
    switch (face) {
    case 0: --pos.y; break;
    case 1: ++pos.y; break;
    case 2: --pos.z; break;
    case 3: ++pos.z; break;
    case 4: --pos.x; break;
    case 5: ++pos.x; break;
    default: break;
    }
    return pos;
}

void FastBlockPlacement::faceCandidates(float stepX, float stepY, float stepZ,
                                        unsigned char (&out)[4]) const
{

    float primary = 0.0f;
    float secondary = 0.0f;
    unsigned char primaryPlus = 0;
    unsigned char primaryMinus = 0;
    unsigned char secondaryPlus = 0;
    unsigned char secondaryMinus = 0;

    switch (m_axis) {
    case Axis::X:
        primary = stepY;   primaryPlus = 1;   primaryMinus = 0;
        secondary = stepZ; secondaryPlus = 3; secondaryMinus = 2;
        break;
    case Axis::Z:
        primary = stepX;   primaryPlus = 5;   primaryMinus = 4;
        secondary = stepY; secondaryPlus = 1; secondaryMinus = 0;
        break;
    case Axis::Y:
    default:
        primary = stepX;   primaryPlus = 5;   primaryMinus = 4;
        secondary = stepZ; secondaryPlus = 3; secondaryMinus = 2;
        break;
    }

    if (std::fabs(secondary) > std::fabs(primary)) {
        std::swap(primary, secondary);
        std::swap(primaryPlus, secondaryPlus);
        std::swap(primaryMinus, secondaryMinus);
    }

    const unsigned char primaryForward = (primary >= 0.0f) ? primaryPlus : primaryMinus;
    const unsigned char primaryBack = (primary >= 0.0f) ? primaryMinus : primaryPlus;
    const unsigned char secondaryForward = (secondary >= 0.0f) ? secondaryPlus : secondaryMinus;
    const unsigned char secondaryBack = (secondary >= 0.0f) ? secondaryMinus : secondaryPlus;

    out[0] = primaryForward;
    out[1] = primaryBack;
    out[2] = secondaryForward;
    out[3] = secondaryBack;
}

unsigned char FastBlockPlacement::faceForCell(const BlockPos& pos,
                                              const unsigned char (&candidates)[4],
                                              const BlockPos* targets, int count)
{

    for (const unsigned char face : candidates) {
        const BlockPos shifted = shiftByFace(pos, face);
        for (int i = 0; i < count; ++i) {
            if (targets[i].x == shifted.x && targets[i].y == shifted.y
                && targets[i].z == shifted.z) {
                return face;
            }
        }
    }

    return candidates[0];
}

bool FastBlockPlacement::alreadyPlaced(const BlockPos& pos) const
{
    for (const BlockPos& done : m_placed) {
        if (done.x == pos.x && done.y == pos.y && done.z == pos.z) {
            return true;
        }
    }
    return false;
}

void FastBlockPlacement::rememberPlaced(const BlockPos& pos)
{
    if (alreadyPlaced(pos)) {
        return;
    }

    if (m_placed.size() >= kMaxPlacedMemory) {
        const auto drop = static_cast<std::ptrdiff_t>(kMaxPlacedMemory / 4);
        m_placed.erase(m_placed.begin(), m_placed.begin() + drop);
    }
    m_placed.push_back(pos);
}

void FastBlockPlacement::cycleAxis()
{
    switch (m_axis) {
    case Axis::X: m_axis = Axis::Y; break;
    case Axis::Y: m_axis = Axis::Z; break;
    case Axis::Z:
    default:      m_axis = Axis::X; break;
    }
}

MenuItem FastBlockPlacement::buildMenu()
{
    std::vector<MenuItem> children;
    children.push_back(menu::back());
    children.push_back(enabledItem());
    children.push_back(toggleKeyItem());

    children.push_back(menu::choice(
        L"Locked axis", {L"X", L"Y", L"Z"}, [this] { return static_cast<int>(m_axis); },
        [this](int at) { m_axis = static_cast<Axis>(std::clamp(at, 0, 2)); }));
    children.push_back(menu::number(
        L"Range", [this] { return static_cast<float>(m_range); },
        [this](float value) {
            m_range = std::clamp(static_cast<int>(value), kMinRange, kMaxRange);
        },
        true, static_cast<float>(kMinRange), static_cast<float>(kMaxRange)));

    MenuItem item = menu::submenu(name(), std::move(children));
    item.available = [this] { return available(); };
    item.isOn = [this] { return enabled(); };
    return item;
}

void FastBlockPlacement::loadConfig(const nlohmann::json& section)
{
    Module::loadConfig(section);

    const int mode = std::clamp(Config::getInt(section, "mode", static_cast<int>(Axis::Y)), 0, 2);
    m_axis = static_cast<Axis>(mode);

    m_range = std::clamp(Config::getInt(section, "distance", kDefaultRange), kMinRange, kMaxRange);
}

void FastBlockPlacement::saveConfig(nlohmann::json& section) const
{
    Module::saveConfig(section);
    section["mode"] = static_cast<int>(m_axis);
    section["distance"] = m_range;

    section.erase("feet");
}

void FastBlockPlacement::onEnabledChanged(bool enabled)
{
    if (!enabled) {
        m_hasBase = false;
        m_manualDone = false;
        m_placed.clear();
    }
}

bool FastBlockPlacement::onBuildBlock(void* gameMode, void* blockPos, unsigned char face,
                                      unsigned char extra)
{

    if (m_placing) {
        return hooks::callBuildBlock(gameMode, blockPos, face, extra);
    }

    m_gameMode = gameMode;

    if (!enabled() || !memory::isReadable(blockPos, sizeof(int) * 3)) {
        return hooks::callBuildBlock(gameMode, blockPos, face, extra);
    }

    m_holdUntil = Clock::now() + std::chrono::milliseconds(kHoldMs);

    if (m_manualDone) {

        BlockPos redirect = m_base;
        const bool result = hooks::callBuildBlock(gameMode, &redirect, face, extra);
        placeRange();
        return result;
    }

    const auto* const pos = reinterpret_cast<const int*>(blockPos);

    m_base = shiftByFace(BlockPos{pos[0], pos[1], pos[2]}, face);
    m_hasBase = true;
    m_manualDone = true;

    rememberPlaced(m_base);

    log().info(L"FastBlockPlacement: base ({}, {}, {}) face {}", m_base.x, m_base.y, m_base.z,
               static_cast<int>(face));

    const bool result = hooks::callBuildBlock(gameMode, blockPos, face, extra);
    placeRange();
    return result;
}

void FastBlockPlacement::onPlayerViewUpdate()
{

    if (!m_hasBase || m_placing) {
        return;
    }

    if ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0) {
        m_holdUntil = Clock::now() + std::chrono::milliseconds(kHoldMs);

        placeRange();
        return;
    }

    if (Clock::now() > m_holdUntil) {
        m_hasBase = false;
        m_manualDone = false;
        m_placed.clear();
    }
}

void FastBlockPlacement::placeRange()
{

    if (!enabled() || m_placing || m_gameMode == nullptr || !m_hasBase
        || !input::isInGameplay()) {
        return;
    }

    const Clock::time_point now = Clock::now();
    const PlayerView view = GameData::instance().playerView();

    constexpr float kDegToRad = std::numbers::pi_v<float> / 180.0f;
    const float yawRad = view.yaw * kDegToRad;
    const float pitchRad = view.pitch * kDegToRad;
    const float cosPitch = std::cos(pitchRad);
    const float dirX = -std::sin(yawRad) * cosPitch;
    const float dirY = -std::sin(pitchRad);
    const float dirZ = std::cos(yawRad) * cosPitch;

    float stepX = 0.0f;
    float stepY = 0.0f;
    float stepZ = 0.0f;
    switch (m_axis) {
    case Axis::X: stepY = dirY; stepZ = dirZ; break;
    case Axis::Z: stepX = dirX; stepY = dirY; break;
    case Axis::Y:
    default:      stepX = dirX; stepZ = dirZ; break;
    }

    const float length = std::sqrt(stepX * stepX + stepY * stepY + stepZ * stepZ);
    if (length < kMinDirection) {
        return;
    }
    stepX /= length;
    stepY /= length;
    stepZ /= length;

    const float footY = view.y - GameData::kEyeHeight;

    const bool resendDue = now >= m_nextResend;

    constexpr int firstStep = kFirstStep;

    BlockPos targets[kMaxRange + 1];
    int count = 0;
    for (int step = firstStep; step <= m_range; ++step) {
        const auto distance = static_cast<float>(step);

        BlockPos target;
        target.x = static_cast<int>(std::floor(view.x + stepX * distance));
        target.y = static_cast<int>(std::floor(footY + stepY * distance));
        target.z = static_cast<int>(std::floor(view.z + stepZ * distance));

        switch (m_axis) {
        case Axis::X: target.x = m_base.x; break;
        case Axis::Z: target.z = m_base.z; break;
        case Axis::Y:
        default:      target.y = m_base.y; break;
        }

        bool duplicate = false;
        for (int i = 0; i < count; ++i) {
            if (targets[i].x == target.x && targets[i].y == target.y
                && targets[i].z == target.z) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            targets[count++] = target;
        }
    }

    if (count == 0) {
        return;
    }

    unsigned char candidates[4] = {};
    faceCandidates(stepX, stepY, stepZ, candidates);

    int attempted = 0;
    int placed = 0;
    unsigned char lastFace = candidates[0];

    m_placing = true;
    for (int i = 0; i < count; ++i) {

        BlockPos target = targets[i];

        if (alreadyPlaced(target) && !resendDue) {
            continue;
        }

        const unsigned char face = faceForCell(target, candidates, targets, count);
        lastFace = face;

        ++attempted;
        if (hooks::callBuildBlock(m_gameMode, &target, face, 0)) {
            ++placed;
        }
        rememberPlaced(target);
    }
    m_placing = false;

    if (resendDue) {
        m_nextResend = now + std::chrono::milliseconds(kResendIntervalMs);
    }

    if (attempted == 0) {
        return;
    }

    if (now >= m_nextLog) {
        m_nextLog = now + std::chrono::milliseconds(kLogIntervalMs);

        log().info(L"FastBlockPlacement: {} yaw {:.1f} pitch {:.1f} face {} (end {}) "
                   L"({}, {}, {}) -> ({}, {}, {}) {}/{} placed",
                   axisName(), view.yaw, view.pitch, static_cast<int>(candidates[0]),
                   static_cast<int>(lastFace), targets[0].x, targets[0].y, targets[0].z,
                   targets[count - 1].x, targets[count - 1].y, targets[count - 1].z, placed,
                   attempted);
    }
}

}
