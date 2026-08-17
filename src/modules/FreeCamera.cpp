#include "modules/FreeCamera.h"

#include "config/Config.h"
#include "core/Logger.h"
#include "game/GameData.h"
#include "game/UiSound.h"
#include "input/Foreground.h"
#include "input/Keys.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstring>
#include <numbers>

namespace tsukuyomi {

namespace {

bool keyDown(int virtualKey)
{
    return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
}

HMODULE currentModule()
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                           | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&currentModule), &module);
    return module;
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
    if (m_keyHook != nullptr) {
        return m_held[static_cast<size_t>(key)].load(std::memory_order_relaxed);
    }

    return keyDown(virtualKeyFor(key));
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

        m_patchX = makeNopPatch(base + kWriteX, kWriteSize);
        m_patchY = makeNopPatch(base + kWriteY, kWriteSize);
        m_patchZ = makeNopPatch(base + kWriteZ, kWriteSize);
    }

    if (std::byte* const rot = Scanner::instance().address(Target::PlayerRotation);
        rot != nullptr) {
        m_patchYaw = makeNopPatch(rot + kWriteYaw, kWriteYawSize);
        m_patchPitch = makeNopPatch(rot + kWritePitch, kWritePitchSize);
        m_patchYawFollow = makeNopPatch(rot + kWriteYawFollow, kWriteYawFollowSize);
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
        return makeNopPatch(at, size);
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

        m_patchPerspective = Patch(view, {std::byte{0xB8}, kThirdPersonBack, std::byte{0x00},
                                          std::byte{0x00}, std::byte{0x00}, std::byte{0xC3}});
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
    if (m_keyHook != nullptr) {
        return;
    }

    clearHeldKeys();
    s_hookOwner = this;
    m_keyHook = SetWindowsHookExW(WH_KEYBOARD_LL, &FreeCamera::keyboardHookProc, currentModule(),
                                  0);
    if (m_keyHook == nullptr) {
        s_hookOwner = nullptr;
        log().warn(L"FreeCamera: could not hook the keyboard (error {}). "
                   L"The camera still moves, but the player will move with it",
                   GetLastError());
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

        if ((down || up) && s_hookOwner->enabled() && input::isInGameplay()) {
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
    if (enabled) {

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

void FreeCamera::onCameraWrite(void* cameraBase)
{
    if (!enabled() || cameraBase == nullptr) {
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

    const float offset = movementOffset();
    if (offset < kNoMovement) {

        const float angle = (cameraYaw(base) + offset + 90.0f)
                            * (std::numbers::pi_v<float> / 180.0f);
        m_x += std::cos(angle) * speed;
        m_z += std::sin(angle) * speed;
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
