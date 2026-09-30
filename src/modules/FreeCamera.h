#pragma once

#include <Windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
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

public:

    void onCameraWrite(void* cameraBase);

    void onMoveInput(void* input);

    void onMoveIntent(void* out, void* input);

    void onInputGatherBefore(void* out);
    void onInputGather(void* out, void* src);

    bool intentFresh() const;

    bool borrowForChunkReload();

    bool borrowing() const;

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

    static float cameraYaw(const float* quat);

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

    bool held(MoveKey key) const;

    static constexpr std::uint32_t kRawJumpBit = 1u << 7;
    static constexpr std::uint32_t kRawSneakBit = 1u << 0;
    static constexpr std::uint64_t kRawHoldGraceMs = 150;

    std::atomic<float> m_intentStrafe{0.0f};
    std::atomic<float> m_intentForward{0.0f};
    std::atomic<unsigned long long> m_intentAt{0};
    static constexpr unsigned long long kIntentFreshMs = 120;

    float m_speed = kDefaultSpeed;

    Patch m_patchX;
    Patch m_patchY;
    Patch m_patchZ;

    unsigned long long m_borrowUntil = 0;
    bool m_borrowSynced = false;
    float m_borrowX = 0.0f;
    float m_borrowY = 0.0f;
    float m_borrowZ = 0.0f;

    static constexpr float kBorrowJump = 4096.0f;
    static constexpr unsigned long long kBorrowMs = 600;

    bool applyBorrow(std::byte* cameraBase);
    void endBorrow(std::byte* cameraBase);

    std::atomic<bool> m_borrowActive{false};
    std::atomic<unsigned long long> m_borrowQuietUntil{0};
    static constexpr unsigned long long kBorrowQuietMs = 250;

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

    int m_moveButtons[kMoveKeyCount]{-1, -1, -1, -1, -1, -1, -1};
    std::atomic<std::uint64_t> m_rawSeenMs[kMoveKeyCount]{};

    float m_frozenView[3]{};
    bool m_hasFrozenView = false;

    float m_x = 0.0f;
    float m_y = 0.0f;
    float m_z = 0.0f;
    bool m_synced = false;
};

}
