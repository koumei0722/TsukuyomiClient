#include "modules/FreeCamera.h"
#include "input/GameButtons.h"

#include "config/Config.h"
#include "core/Logger.h"
#include "game/GameData.h"
#include "hooks/Detours.h"
#include "input/Foreground.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"

#include <Windows.h>

#include <string>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstring>
#include <numbers>
#include <vector>

namespace tsukuyomi {

namespace {

int accessViolationFilter(unsigned long code)
{
    return (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR)
               ? EXCEPTION_EXECUTE_HANDLER
               : EXCEPTION_CONTINUE_SEARCH;
}

constexpr std::uint32_t kUpDownEdge = (1u << 22) | (1u << 23) | (1u << 24) | (1u << 25);
constexpr std::uint32_t kUpDownHeld = (1u << 26) | (1u << 21);

bool dropUpDownBitsGuarded(std::byte* input)
{
    __try {
        constexpr std::uint32_t drop = kUpDownEdge | kUpDownHeld;
        *reinterpret_cast<std::uint32_t*>(input + 0x00) &= ~drop;
        *reinterpret_cast<std::uint32_t*>(input + 0x10) &= ~drop;
        return true;
    } __except (accessViolationFilter(GetExceptionCode())) {
        return false;
    }
}

bool takeMoveIntentGuarded(std::byte* out, float* took)
{
    constexpr float put = 0.0f;
    __try {
        took[0] = *reinterpret_cast<float*>(out + 0x0);
        took[1] = *reinterpret_cast<float*>(out + 0x4);
        *reinterpret_cast<float*>(out + 0x0) = put;
        *reinterpret_cast<float*>(out + 0x4) = put;
        return true;
    } __except (accessViolationFilter(GetExceptionCode())) {
        return false;
    }
}

}

FreeCamera& FreeCamera::instance()
{
    static FreeCamera module;
    return module;
}

bool FreeCamera::available() const
{
    return Scanner::instance().found(Target::CameraUpdate);
}

bool FreeCamera::held(MoveKey key) const
{
    const int index = static_cast<int>(key);
    if (index < 0 || index >= static_cast<int>(kMoveKeyCount)) return false;
    const bool watched = GameButtons::instance().buttonHeld(m_moveButtons[index]);
    if (key == MoveKey::Up || key == MoveKey::Down) {
        const std::uint64_t seen = m_rawSeenMs[static_cast<size_t>(key)].load(std::memory_order_relaxed);
        return watched || (seen != 0 && GetTickCount64() - seen <= kRawHoldGraceMs);
    }
    return watched;
}

void FreeCamera::onMoveInput(void* input)
{
    if (input == nullptr) {
        return;
    }

    if (!enabled() || !input::isInGameplay()) {
        return;
    }
    dropUpDownBitsGuarded(static_cast<std::byte*>(input));
}

void FreeCamera::onMoveIntent(void* out, void* input)
{
    if (out == nullptr || !enabled() || !input::isInGameplay()) {
        return;
    }
    float took[2]{};
    if (!takeMoveIntentGuarded(static_cast<std::byte*>(out), took)) {
        return;
    }

    if (std::isfinite(took[0]) && std::isfinite(took[1])
        && (took[0] != 0.0f || took[1] != 0.0f)) {
        m_intentStrafe.store(took[0], std::memory_order_release);
        m_intentForward.store(took[1], std::memory_order_release);
        m_intentAt.store(GetTickCount64(), std::memory_order_release);
    }

}

bool takeRawUpDownGuarded(std::byte* out, std::uint32_t* bits, float* move)
{
    __try {
        const std::uint32_t raw = *reinterpret_cast<const std::uint32_t*>(out + 0x00);
        *bits = raw;
        move[0] = *reinterpret_cast<const float*>(out + 0x04);
        move[1] = *reinterpret_cast<const float*>(out + 0x08);
        constexpr std::uint32_t drop = (1u << 7) | (1u << 0);
        if ((raw & drop) != 0) {
            *reinterpret_cast<std::uint32_t*>(out + 0x00) = raw & ~drop;
        }
        return true;
    } __except (accessViolationFilter(GetExceptionCode())) {
        return false;
    }
}

bool takeInputGatherGuarded(std::byte* out, std::uint32_t* bits, float* move)
{
    __try {
        *bits = *reinterpret_cast<std::uint32_t*>(out + 0x00);
        move[0] = *reinterpret_cast<float*>(out + 0x04);
        move[1] = *reinterpret_cast<float*>(out + 0x08);
        constexpr std::uint32_t drop = (1u << 13) | (1u << 14) | (1u << 15) | (1u << 16)
                                       | (1u << 21) | (1u << 22) | (1u << 23) | (1u << 24)
                                       | (1u << 25) | (1u << 26);
        *reinterpret_cast<std::uint32_t*>(out + 0x00) &= ~drop;
        *reinterpret_cast<float*>(out + 0x04) = 0.0f;
        *reinterpret_cast<float*>(out + 0x08) = 0.0f;
        return true;
    } __except (accessViolationFilter(GetExceptionCode())) {
        return false;
    }
}

void FreeCamera::onInputGatherBefore(void* out)
{
    if (out == nullptr || !enabled() || !input::isInGameplay()) {
        return;
    }
    std::uint32_t bits = 0;
    float move[2]{};
    if (!takeRawUpDownGuarded(static_cast<std::byte*>(out), &bits, move)) {
        return;
    }
    const std::uint64_t now = GetTickCount64();
    if ((bits & kRawJumpBit) != 0) {
        m_rawSeenMs[static_cast<size_t>(MoveKey::Up)].store(now, std::memory_order_relaxed);
    }
    if ((bits & kRawSneakBit) != 0) {
        m_rawSeenMs[static_cast<size_t>(MoveKey::Down)].store(now, std::memory_order_relaxed);
    }
}

void FreeCamera::onInputGather(void* out, void* src)
{
    if (out == nullptr || !enabled() || !input::isInGameplay()) {
        return;
    }
    (void)src;
    std::uint32_t bits = 0;
    float move[2]{};
    if (!takeInputGatherGuarded(static_cast<std::byte*>(out), &bits, move)) {
        return;
    }
    if (std::isfinite(move[0]) && std::isfinite(move[1])
        && (move[0] != 0.0f || move[1] != 0.0f)) {
        m_intentStrafe.store(move[0], std::memory_order_release);
        m_intentForward.store(move[1], std::memory_order_release);
        m_intentAt.store(GetTickCount64(), std::memory_order_release);
    }
}

bool FreeCamera::intentFresh() const
{
    const unsigned long long at = m_intentAt.load(std::memory_order_acquire);
    return at != 0 && (GetTickCount64() - at) <= kIntentFreshMs;
}

float FreeCamera::cameraYaw(const float* q)
{
    const float qw = q[1];
    const float qy = q[3];
    return 2.0f * std::atan2(qy, qw) * (180.0f / std::numbers::pi_v<float>);
}

float FreeCamera::movementOffset() const
{
    const bool forward = held(MoveKey::Forward);
    const bool back = held(MoveKey::Back);
    const bool left = held(MoveKey::Left);
    const bool right = held(MoveKey::Right);

    if (forward && left)  return -45.0f;
    if (forward && right) return 45.0f;
    if (back && left)     return -135.0f;
    if (back && right)    return 135.0f;
    if (forward)          return 0.0f;
    if (back)             return 180.0f;
    if (left)             return -90.0f;
    if (right)            return 90.0f;

    return kNoMovement;
}

void FreeCamera::onScansReady()
{
    const char* names[kMoveKeyCount] = {
        gamebuttonlogic::button::up, gamebuttonlogic::button::down,
        gamebuttonlogic::button::left, gamebuttonlogic::button::right,
        gamebuttonlogic::button::jump, gamebuttonlogic::button::sneak,
        gamebuttonlogic::button::sprint};
    for (size_t i = 0; i < kMoveKeyCount; ++i) {
        m_moveButtons[i] = GameButtons::instance().watchButton(names[i]);
    }
    if (std::byte* const base = Scanner::instance().address(Target::CameraUpdate);
        base != nullptr) {
        m_patchX = makeNopPatch(base + kWriteX, kWriteSize, "FreeCamera.CameraPosition");
        m_patchY = makeNopPatch(base + kWriteY, kWriteSize, "FreeCamera.CameraPosition");
        m_patchZ = makeNopPatch(base + kWriteZ, kWriteSize, "FreeCamera.CameraPosition");
    }

    if (std::byte* const rot = Scanner::instance().address(Target::PlayerRotation);
        rot != nullptr) {
        m_patchYaw = makeNopPatch(rot + kWriteYaw, kWriteYawSize, "FreeCamera.BodyRotation");
        m_patchPitch = makeNopPatch(rot + kWritePitch, kWritePitchSize, "FreeCamera.BodyRotation");
        m_patchYawFollow = makeNopPatch(rot + kWriteYawFollow, kWriteYawFollowSize, "FreeCamera.BodyRotation");
    } else {
        log().warn(L"FreeCamera: the player rotation site was not found; "
                   L"the body will turn with the camera");
    }

    const auto looksMovss = [](const std::byte* at) {
        if (at == nullptr || !memory::isReadable(at, 4)) {
            return false;
        }
        const auto* const b = reinterpret_cast<const unsigned char*>(at);
        return (b[0] == 0xF3 && b[1] == 0x0F && b[2] == 0x11)
               || (b[0] == 0xF3 && b[1] == 0x44 && b[2] == 0x0F && b[3] == 0x11);
    };
    const auto nopIfMovss = [&](std::byte* at, size_t size, const wchar_t* what) {
        if (!looksMovss(at)) {
            log().warn(L"FreeCamera: {} does not look like a movss; leaving it alone", what);
            return Patch{};
        }
        return makeNopPatch(at, size, "FreeCamera.HeadRotation");
    };

    if (std::byte* const head = Scanner::instance().address(Target::PlayerHeadRotation);
        head != nullptr) {
        m_patchHead = nopIfMovss(head + kWriteHead, kWriteHeadSize, L"the head write");
        m_patchHeadPair = nopIfMovss(head + kWriteHeadPair, kWriteHeadPairSize,
                                     L"the head write (second half)");
        m_patchHeadAlt = nopIfMovss(head + kWriteHeadAlt, kWriteHeadAltSize,
                                    L"the other head write");
        m_patchHeadAltPair = nopIfMovss(head + kWriteHeadAltPair, kWriteHeadAltPairSize,
                                        L"the other head write (second half)");
    } else {
        log().warn(L"FreeCamera: the player head rotation site was not found; "
                   L"the face will turn with the camera");
    }
    if (std::byte* const head = Scanner::instance().address(Target::PlayerHeadRotationInput);
        head != nullptr) {
        m_patchHeadInput = nopIfMovss(head + kWriteHead, kWriteHeadSize, L"the input head write");
        m_patchHeadInputPair = nopIfMovss(head + kWriteHeadPair, kWriteHeadInputPairSize,
                                          L"the input head write (second half)");
    } else {
        log().warn(L"FreeCamera: the other player head rotation site was not found; "
                   L"the face will turn with the camera");
    }

    if (std::byte* const view = Scanner::instance().address(Target::ViewPerspective);
        view != nullptr) {
        m_patchPerspective = Patch(view,
                                   {std::byte{0xB8}, kThirdPersonBack, std::byte{0x00}, std::byte{0x00},
                                    std::byte{0x00}, std::byte{0xC3}},
                                   "FreeCamera.ThirdPerson");
    } else {
        log().warn(L"FreeCamera: the view perspective getter was not found; "
                   L"the view will stay in first person");
    }
}

MenuItem FreeCamera::buildMenu()
{
    std::vector<MenuItem> children;
    children.push_back(menu::back());
    children.push_back(enabledItem());
    children.push_back(toggleKeyItem());
    children.push_back(menu::number(
        L"Speed", [this] { return m_speed; },
        [this](float value) { m_speed = std::clamp(value, kMinSpeed, kMaxSpeed); }, false,
        kMinSpeed, kMaxSpeed));

    MenuItem item = menu::submenu(name(), std::move(children));
    item.available = [this] { return available(); };
    item.isOn = [this] { return enabled(); };
    return item;
}

void FreeCamera::loadConfig(const nlohmann::json& section)
{
    Module::loadConfig(section);

    m_speed = std::clamp(Config::getFloat(section, "speed", kDefaultSpeed), kMinSpeed, kMaxSpeed);
}

void FreeCamera::saveConfig(nlohmann::json& section) const
{
    Module::saveConfig(section);
    section["speed"] = m_speed;
}

void FreeCamera::onEnabledChanged(bool enabled)
{
    if (enabled) {
        if (!GameData::instance().hasLivePlayer()) {
            GameData::instance().findPlayerFromClient(hooks::gameClientInstance());
        }
        m_synced = false;
        m_hasFrozenView = false;
        m_patchX.apply();
        m_patchY.apply();
        m_patchZ.apply();

        m_patchYaw.apply();
        m_patchPitch.apply();
        m_patchYawFollow.apply();

        m_patchHead.apply();
        m_patchHeadPair.apply();
        m_patchHeadAlt.apply();
        m_patchHeadAltPair.apply();
        m_patchHeadInput.apply();
        m_patchHeadInputPair.apply();

        m_patchPerspective.apply();

    } else {

        m_patchX.restore();
        m_patchY.restore();
        m_patchZ.restore();

        m_patchYaw.restore();
        m_patchPitch.restore();
        m_patchYawFollow.restore();
        m_patchHead.restore();
        m_patchHeadPair.restore();
        m_patchHeadAlt.restore();
        m_patchHeadAltPair.restore();
        m_patchHeadInput.restore();
        m_patchHeadInputPair.restore();

        m_patchPerspective.restore();

    }
}

bool FreeCamera::freezeViewVector(float* out)
{
    if (!enabled()) {
        m_hasFrozenView = false;
        return false;
    }
    if (out == nullptr) {
        return false;
    }
    if (!m_hasFrozenView) {
        if (!memory::copyGuarded(out, m_frozenView, sizeof(m_frozenView))) {
            return false;
        }
        m_hasFrozenView = true;
        log().info(L"FreeCamera: froze the aim direction at ({:.3f}, {:.3f}, {:.3f})",
                   m_frozenView[0], m_frozenView[1], m_frozenView[2]);
        return false;
    }
    return memory::writeGuarded(out, m_frozenView, sizeof(m_frozenView));
}

bool FreeCamera::borrowForChunkReload()
{
    if (enabled()) {
        return false;
    }
    if (!m_patchX.valid() || !m_patchY.valid() || !m_patchZ.valid()) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true, std::memory_order_relaxed)) {
            log().warn(L"Schematica: the camera write site was not found, so chunk rebuilds "
                       L"cannot be requested");
        }
        return false;
    }
    if (m_borrowUntil != 0) {
        return false;
    }
    m_patchX.apply();
    m_patchY.apply();
    m_patchZ.apply();
    m_borrowSynced = false;
    m_borrowUntil = GetTickCount64() + kBorrowMs;
    m_borrowActive.store(true, std::memory_order_release);
    return true;
}

bool FreeCamera::borrowing() const
{
    return m_borrowActive.load(std::memory_order_acquire)
           || GetTickCount64() < m_borrowQuietUntil.load(std::memory_order_relaxed);
}

bool FreeCamera::applyBorrow(std::byte* cameraBase)
{
    static_assert(kCameraY == kCameraX + sizeof(float)
                  && kCameraZ == kCameraY + sizeof(float));
    float position[3]{};
    if (!memory::copyGuarded(cameraBase + kCameraX, position, sizeof(position))) {
        endBorrow(nullptr);
        return false;
    }
    if (!m_borrowSynced) {
        m_borrowX = position[0];
        m_borrowY = position[1];
        m_borrowZ = position[2];
        m_borrowSynced = true;
    }
    if (GetTickCount64() >= m_borrowUntil) {
        endBorrow(cameraBase);
        return false;
    }
    const float moved[3] = {m_borrowX + kBorrowJump, m_borrowY, m_borrowZ + kBorrowJump};
    return memory::writeGuarded(cameraBase + kCameraX, moved, sizeof(moved));
}

void FreeCamera::endBorrow(std::byte* cameraBase)
{
    if (cameraBase != nullptr && m_borrowSynced) {
        const float position[3] = {m_borrowX, m_borrowY, m_borrowZ};
        memory::writeGuarded(cameraBase + kCameraX, position, sizeof(position));
    }
    m_patchX.restore();
    m_patchY.restore();
    m_patchZ.restore();
    m_borrowUntil = 0;
    m_borrowSynced = false;
    m_borrowQuietUntil.store(GetTickCount64() + kBorrowQuietMs, std::memory_order_relaxed);
    m_borrowActive.store(false, std::memory_order_release);
}

void FreeCamera::onCameraWrite(void* cameraBase)
{
    if (cameraBase == nullptr) {
        return;
    }
    if (m_borrowUntil != 0) {
        if (applyBorrow(static_cast<std::byte*>(cameraBase))) {
            return;
        }
    }
    if (!enabled()) {
        return;
    }

    if (!input::isInGameplay()) {
        return;
    }

    auto* const base = static_cast<std::byte*>(cameraBase);
    static_assert(kCameraX == kCameraQuat + 4 * sizeof(float)
                  && kCameraY == kCameraX + sizeof(float)
                  && kCameraZ == kCameraY + sizeof(float));
    float cameraState[7]{};
    if (!memory::copyGuarded(base + kCameraQuat, cameraState, sizeof(cameraState))) {
        return;
    }

    if (!m_synced) {
        m_x = cameraState[4];
        m_y = cameraState[5];
        m_z = cameraState[6];
        m_synced = true;
    }

    const float speed = held(MoveKey::Fast) ? m_speed * 2.0f : m_speed;

    float offset = kNoMovement;
    float scale = 1.0f;
    const bool byIntent = intentFresh();
    if (byIntent) {
        const float s = m_intentStrafe.load(std::memory_order_acquire);
        const float f = m_intentForward.load(std::memory_order_acquire);
        const float len = std::sqrt(s * s + f * f);
        if (std::isfinite(len) && len > 0.001f) {
            offset = std::atan2(-s, f) * (180.0f / std::numbers::pi_v<float>);
            scale = (len < 1.0f) ? len : 1.0f;
        }
    } else {
        offset = movementOffset();
    }
    if (offset < kNoMovement) {
        const float angle = (cameraYaw(cameraState) + offset + 90.0f)
                            * (std::numbers::pi_v<float> / 180.0f);
        m_x += std::cos(angle) * speed * scale;
        m_z += std::sin(angle) * speed * scale;

    }

    if (held(MoveKey::Up)) {
        m_y += speed;
    }
    if (held(MoveKey::Down)) {
        m_y -= speed;
    }

    const float position[3] = {m_x, m_y, m_z};
    memory::writeGuarded(base + kCameraX, position, sizeof(position));
}

void FreeCamera::shutdown()
{

    m_patchX.restore();
    m_patchY.restore();
    m_patchZ.restore();

    m_patchYaw.restore();
    m_patchPitch.restore();
    m_patchYawFollow.restore();
    m_patchHead.restore();
    m_patchHeadInput.restore();
    m_patchPerspective.restore();
}

}
