#pragma once

#include <Windows.h>

#include <atomic>

#include "memory/Patch.h"
#include "modules/Module.h"

namespace tsukuyomi {

class FreeCamera : public Module {
public:
    static FreeCamera& instance();

    const wchar_t* name() const override { return L"FreeCamera"; }
    bool available() const override;

    MenuItem buildMenu() override;
    void loadConfig(const nlohmann::json& section) override;
    void saveConfig(nlohmann::json& section) const override;

    void onScansReady() override;
    void shutdown() override;

    void onCameraWrite(void* cameraBase);

    static constexpr ptrdiff_t kTrampolineOffset = 0x0C;

protected:
    void onEnabledChanged(bool enabled) override;

    bool persistEnabled() const override { return false; }

private:
    FreeCamera() = default;

    static constexpr ptrdiff_t kCameraX = 0x40;
    static constexpr ptrdiff_t kCameraY = 0x44;
    static constexpr ptrdiff_t kCameraZ = 0x48;

    static constexpr ptrdiff_t kCameraQuat = 0x30;

    static constexpr ptrdiff_t kWriteX = 0x11;
    static constexpr ptrdiff_t kWriteY = 0x1B;
    static constexpr ptrdiff_t kWriteZ = 0x25;
    static constexpr size_t kWriteSize = 5;

    static constexpr ptrdiff_t kWriteYaw = 0x0D;
    static constexpr ptrdiff_t kWritePitch = 0x18;
    static constexpr ptrdiff_t kWriteYawFollow = 0xA5;
    static constexpr size_t kWriteYawSize = 5;
    static constexpr size_t kWritePitchSize = 4;
    static constexpr size_t kWriteYawFollowSize = 6;

    static constexpr std::byte kThirdPersonBack{1};

    static constexpr float kDefaultSpeed = 0.0625f;

    static constexpr float kNoMovement = 360.0f;
    float movementOffset() const;

    static float cameraYaw(const std::byte* cameraBase);

    enum class MoveKey {
        Forward,
        Back,
        Left,
        Right,
        Up,
        Down,
        Fast,
        Count,
    };

    static constexpr size_t kMoveKeyCount = static_cast<size_t>(MoveKey::Count);

    static MoveKey moveKeyFor(DWORD virtualKey);
    static int virtualKeyFor(MoveKey key);

    bool held(MoveKey key) const;

    void installKeyHook();
    void removeKeyHook();
    void clearHeldKeys();

    static LRESULT CALLBACK keyboardHookProc(int code, WPARAM wParam, LPARAM lParam);

    static FreeCamera* s_hookOwner;

    float m_speed = kDefaultSpeed;

    Patch m_patchX;
    Patch m_patchY;
    Patch m_patchZ;

    Patch m_patchYaw;
    Patch m_patchPitch;
    Patch m_patchYawFollow;

    Patch m_patchPerspective;

    HHOOK m_keyHook = nullptr;

    std::atomic<bool> m_held[kMoveKeyCount]{};

    float m_x = 0.0f;
    float m_y = 0.0f;
    float m_z = 0.0f;
    bool m_synced = false;
};

}
