#pragma once

#include <Windows.h>

#include <atomic>
#include <vector>

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

protected:

    void onUpdate() override;

public:

    void onCameraWrite(void* cameraBase);

    bool freezeViewVector(float* out);

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

    static constexpr ptrdiff_t kWriteHead = 0x09;
    static constexpr size_t kWriteHeadSize = 5;

    static constexpr ptrdiff_t kWriteHeadPair = 0x0E;
    static constexpr size_t kWriteHeadPairSize = 7;
    static constexpr size_t kWriteHeadInputPairSize = 6;

    static constexpr ptrdiff_t kWriteHeadAlt = -0x14F;
    static constexpr size_t kWriteHeadAltSize = 5;
    static constexpr ptrdiff_t kWriteHeadAltPair = -0x14A;
    static constexpr size_t kWriteHeadAltPairSize = 6;

    static constexpr std::byte kThirdPersonBack{1};

    static constexpr float kDefaultSpeed = 0.0625f;

    static constexpr float kMinSpeed = 0.03125f;
    static constexpr float kMaxSpeed = 1.0f;

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

    bool consumeToggle();
    static bool comboKeyOf(const std::vector<int>& combo, DWORD virtualKey);

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

    Patch m_patchHead;
    Patch m_patchHeadPair;
    Patch m_patchHeadAlt;
    Patch m_patchHeadAltPair;
    Patch m_patchHeadInput;
    Patch m_patchHeadInputPair;

    Patch m_patchPerspective;

    HHOOK m_keyHook = nullptr;

    std::atomic<bool> m_held[kMoveKeyCount]{};

    std::atomic<bool> m_toggleDown{false};
    std::atomic<bool> m_togglePressed{false};

    float m_frozenView[3]{};
    bool m_hasFrozenView = false;

    float m_x = 0.0f;
    float m_y = 0.0f;
    float m_z = 0.0f;
    bool m_synced = false;
};

}
