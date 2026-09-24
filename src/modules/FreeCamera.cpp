#include "modules/FreeCamera.h"

#include "config/Config.h"
#include "core/Logger.h"
#include "game/GameData.h"
#include "game/UiSound.h"
#include "hooks/Detours.h"
#include "input/Foreground.h"
#include "input/Keys.h"
#include "input/LowLevelHook.h"
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

bool keyDown(int virtualKey)
{
    return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
}

int accessViolationFilter(unsigned long code)
{
    return (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR)
               ? EXCEPTION_EXECUTE_HANDLER
               : EXCEPTION_CONTINUE_SEARCH;
}

struct MoveInputPeek {
    std::uint32_t bits = 0;
    std::uint32_t bits2 = 0;
    std::uint16_t flags = 0;
};

constexpr std::uint32_t kUpDownEdge = (1u << 22) | (1u << 23) | (1u << 24) | (1u << 25);
constexpr std::uint32_t kUpDownHeld = (1u << 26) | (1u << 21);

constexpr std::uint32_t kMoveBits = (1u << 13) | (1u << 14) | (1u << 15) | (1u << 16);

bool peekMoveInputGuarded(std::byte* input, bool clearJump, MoveInputPeek* out)
{
    __try {
        out->bits = *reinterpret_cast<const std::uint32_t*>(input + 0x00);
        out->bits2 = *reinterpret_cast<const std::uint32_t*>(input + 0x10);
        out->flags = *reinterpret_cast<const std::uint16_t*>(input + 0x60);
        if (clearJump) {
            constexpr std::uint32_t drop = kUpDownEdge | kUpDownHeld;
            *reinterpret_cast<std::uint32_t*>(input + 0x00) &= ~drop;
            *reinterpret_cast<std::uint32_t*>(input + 0x10) &= ~drop;
        }
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

FreeCamera* FreeCamera::s_hookOwner = nullptr;

FreeCamera& FreeCamera::instance()
{
    static FreeCamera module;
    return module;
}

bool FreeCamera::available() const
{
    return Scanner::instance().found(Target::CameraUpdate);
}

FreeCamera::MoveKey FreeCamera::moveKeyFor(DWORD virtualKey)
{
    switch (virtualKey) {
    case 'W':
        return MoveKey::Forward;
    case 'S':
        return MoveKey::Back;
    case 'A':
        return MoveKey::Left;
    case 'D':
        return MoveKey::Right;
    case VK_SPACE:
        return MoveKey::Up;
    case VK_LSHIFT:
    case VK_SHIFT:
        return MoveKey::Down;
    case VK_LCONTROL:
    case VK_CONTROL:
        return MoveKey::Fast;
    default:
        return MoveKey::Count;
    }
}

int FreeCamera::virtualKeyFor(MoveKey key)
{
    switch (key) {
    case MoveKey::Forward:
        return 'W';
    case MoveKey::Back:
        return 'S';
    case MoveKey::Left:
        return 'A';
    case MoveKey::Right:
        return 'D';
    case MoveKey::Up:
        return VK_SPACE;
    case MoveKey::Down:
        return VK_LSHIFT;
    case MoveKey::Fast:
        return VK_LCONTROL;
    default:
        return 0;
    }
}

bool FreeCamera::held(MoveKey key) const
{
    if (movementSuppressed()) {
        if (key == MoveKey::Up || key == MoveKey::Down) {
            const std::uint64_t seen =
                m_rawSeenMs[static_cast<size_t>(key)].load(std::memory_order_relaxed);
            return seen != 0 && (GetTickCount64() - seen) <= kRawHoldGraceMs;
        }
        return keyDown(virtualKeyFor(key));
    }

    if (m_keyHook != nullptr) {
        return m_held[static_cast<size_t>(key)].load(std::memory_order_relaxed);
    }

    return keyDown(virtualKeyFor(key));
}

bool FreeCamera::movementSuppressed() const
{
    const unsigned long long at = m_inputSeenAt.load(std::memory_order_acquire);
    return at != 0 && (GetTickCount64() - at) <= kInputFreshMs;
}

void FreeCamera::onMoveInput(void* input)
{
    if (input == nullptr) {
        return;
    }

    const bool clearJump = enabled() && input::isInGameplay();

    MoveInputPeek peek;
    if (!peekMoveInputGuarded(static_cast<std::byte*>(input), clearJump, &peek)) {
        return;
    }

    m_inputSeenAt.store(GetTickCount64(), std::memory_order_release);

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

float FreeCamera::cameraYaw(const std::byte* cameraBase)
{
    const auto* const q = reinterpret_cast<const float*>(cameraBase + kCameraQuat);
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

void FreeCamera::clearHeldKeys()
{
    for (std::atomic<bool>& flag : m_held) {
        flag.store(false, std::memory_order_relaxed);
    }
}

bool FreeCamera::comboKeyOf(const std::vector<int>& combo, DWORD virtualKey)
{
    if (combo.empty()) {
        return false;
    }
    const int vk = keys::normalize(static_cast<int>(virtualKey));
    if (keys::isModifier(vk)) {
        return false;
    }
    bool isMain = false;
    for (const int key : combo) {
        if (!keys::isModifier(key) && key == vk) {
            isMain = true;
            break;
        }
    }
    if (!isMain) {
        return false;
    }
    for (const int key : combo) {
        if (keys::isModifier(key) && (GetAsyncKeyState(key) & 0x8000) == 0) {
            return false;
        }
    }
    return true;
}

bool FreeCamera::consumeToggle()
{
    return m_togglePressed.exchange(false, std::memory_order_relaxed);
}

void FreeCamera::onUpdate()
{
    if (m_keyHook == nullptr && available()) {
        installKeyHook();
    }
    if (m_keyHook == nullptr) {
        return;
    }
    if (consumeToggle() && input::isInGameplay()) {
        toggle();
        UiSound::instance().request();
    }
}

void FreeCamera::installKeyHook()
{
    if (m_keyHook != nullptr || m_keyHookFailed.load(std::memory_order_relaxed)) {
        return;
    }

    clearHeldKeys();
    s_hookOwner = this;
    const input::LowLevelHook hook =
        input::installLowLevelHook(WH_KEYBOARD_LL, &FreeCamera::keyboardHookProc);
    m_keyHook = hook.hook;
    if (m_keyHook == nullptr) {
        s_hookOwner = nullptr;
        m_keyHookFailed.store(true, std::memory_order_relaxed);
        if (!m_keyHookWarned) {
            m_keyHookWarned = true;
            log().warn(L"FreeCamera: could not hook the keyboard (error {}, {} with the module). "
                       L"The camera still moves, but the player will move with it. "
                       L"Retrying only when FreeCamera is switched",
                       hook.errorWithoutModule, hook.errorWithModule);
        }
        return;
    }
    if (hook.withModule) {
        log().info(L"FreeCamera: hooked the keyboard with the module (error {} without it)",
                   hook.errorWithoutModule);
    } else {
        log().info(L"FreeCamera: hooked the keyboard");
    }
}

void FreeCamera::removeKeyHook()
{
    if (m_keyHook != nullptr) {
        UnhookWindowsHookEx(m_keyHook);
        m_keyHook = nullptr;
    }
    s_hookOwner = nullptr;

    clearHeldKeys();
}

LRESULT CALLBACK FreeCamera::keyboardHookProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && s_hookOwner != nullptr) {
        const bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        const bool up = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);

        const DWORD vkCode = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam)->vkCode;

        if ((down || up) && input::isInGameplay()
            && comboKeyOf(s_hookOwner->toggleKey().combo(), vkCode)) {
            if (down) {
                if (!s_hookOwner->m_toggleDown.exchange(true, std::memory_order_relaxed)) {
                    s_hookOwner->m_togglePressed.store(true, std::memory_order_relaxed);
                }
            } else {
                s_hookOwner->m_toggleDown.store(false, std::memory_order_relaxed);
            }
            return 1;
        }

        if ((down || up) && s_hookOwner->enabled() && input::isInGameplay()
            && !s_hookOwner->movementSuppressed()) {
            const MoveKey key = moveKeyFor(vkCode);
            if (key != MoveKey::Count) {
                s_hookOwner->m_held[static_cast<size_t>(key)].store(down,
                                                                    std::memory_order_relaxed);
                return 1;
            }
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

void FreeCamera::onEnabledChanged(bool enabled)
{
    m_keyHookFailed.store(false, std::memory_order_relaxed);

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

        clearHeldKeys();
    } else {
        clearHeldKeys();

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
    if (out == nullptr || !memory::isReadable(out, sizeof(m_frozenView))) {
        return false;
    }
    if (!m_hasFrozenView) {
        std::memcpy(m_frozenView, out, sizeof(m_frozenView));
        m_hasFrozenView = true;
        log().info(L"FreeCamera: froze the aim direction at ({:.3f}, {:.3f}, {:.3f})",
                   m_frozenView[0], m_frozenView[1], m_frozenView[2]);
        return false;
    }
    std::memcpy(out, m_frozenView, sizeof(m_frozenView));
    return true;
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
    if (!memory::isWritable(cameraBase + kCameraX,
                            static_cast<size_t>(kCameraZ + sizeof(float) - kCameraX))) {
        endBorrow(nullptr);
        return false;
    }
    auto* const x = reinterpret_cast<float*>(cameraBase + kCameraX);
    auto* const y = reinterpret_cast<float*>(cameraBase + kCameraY);
    auto* const z = reinterpret_cast<float*>(cameraBase + kCameraZ);
    if (!m_borrowSynced) {
        m_borrowX = *x;
        m_borrowY = *y;
        m_borrowZ = *z;
        m_borrowSynced = true;
    }
    if (GetTickCount64() >= m_borrowUntil) {
        endBorrow(cameraBase);
        return false;
    }
    *x = m_borrowX + kBorrowJump;
    *y = m_borrowY;
    *z = m_borrowZ + kBorrowJump;
    return true;
}

void FreeCamera::endBorrow(std::byte* cameraBase)
{
    if (cameraBase != nullptr && m_borrowSynced
        && memory::isWritable(cameraBase + kCameraX,
                              static_cast<size_t>(kCameraZ + sizeof(float) - kCameraX))) {
        *reinterpret_cast<float*>(cameraBase + kCameraX) = m_borrowX;
        *reinterpret_cast<float*>(cameraBase + kCameraY) = m_borrowY;
        *reinterpret_cast<float*>(cameraBase + kCameraZ) = m_borrowZ;
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
        clearHeldKeys();
        return;
    }

    auto* const base = static_cast<std::byte*>(cameraBase);
    if (!memory::isWritable(base + kCameraQuat,
                            static_cast<size_t>(kCameraZ + sizeof(float) - kCameraQuat))) {
        return;
    }

    auto* const x = reinterpret_cast<float*>(base + kCameraX);
    auto* const y = reinterpret_cast<float*>(base + kCameraY);
    auto* const z = reinterpret_cast<float*>(base + kCameraZ);

    if (!m_synced) {
        m_x = *x;
        m_y = *y;
        m_z = *z;
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
        const float angle = (cameraYaw(base) + offset + 90.0f)
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

    *x = m_x;
    *y = m_y;
    *z = m_z;
}

void FreeCamera::shutdown()
{
    removeKeyHook();

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
