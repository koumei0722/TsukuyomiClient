#include "modules/FreeCamera.h"
#include "input/GameButtons.h"

#include "config/Config.h"
#include "core/Logger.h"
#include "core/Notice.h"
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

constexpr std::uint32_t kDirectLookTypeId = 0xCEB578F1u;
constexpr std::size_t kDirectLookStride = 0x14;
constexpr std::uint32_t kOrbitTypeId = 0x2F0FC33Fu;
constexpr std::size_t kOrbitStride = 0x4c;
constexpr std::size_t kOrbitAngles = 0x24;

bool readCameraContext(const void* frame, int offset, std::uintptr_t& registry, std::uint32_t& id)
{
    __try {
        const auto* const ctx = static_cast<const char*>(frame) + offset;
        if (*reinterpret_cast<const std::uint8_t*>(ctx + 0x18) != 1) {
            return false;
        }
        registry = *reinterpret_cast<const std::uintptr_t*>(ctx + 0x08);
        id = *reinterpret_cast<const std::uint32_t*>(ctx + 0x10);
        return registry != 0;
    } __except (accessViolationFilter(GetExceptionCode())) {
        return false;
    }
}

bool cameraComponentAt(std::uintptr_t registry, std::uint32_t id, std::uint32_t typeId, std::size_t stride,
                       std::uintptr_t& at)
{
    __try {
        const std::uintptr_t first = *reinterpret_cast<const std::uintptr_t*>(registry + 0x68);
        const std::uintptr_t last = *reinterpret_cast<const std::uintptr_t*>(registry + 0x70);
        if (first == 0 || last <= first || (last - first) % 0x20 != 0) {
            return false;
        }
        const std::uintptr_t entries = (last - first) / 0x20;
        std::uintptr_t store = 0;
        for (std::uintptr_t i = 0; i < entries && i < 4096; ++i) {
            const std::uintptr_t entry = first + i * 0x20;
            if (*reinterpret_cast<const std::uint32_t*>(entry + 0x08) == typeId) {
                store = *reinterpret_cast<const std::uintptr_t*>(entry + 0x10);
                break;
            }
        }
        if (store == 0) {
            return false;
        }
        const std::uint32_t low = id & 0x3ffffu;
        const std::uint32_t page = low >> 11;
        const std::uintptr_t pagesBegin = *reinterpret_cast<const std::uintptr_t*>(store + 0x08);
        const std::uintptr_t pagesEnd = *reinterpret_cast<const std::uintptr_t*>(store + 0x10);
        if (pagesBegin == 0 || pagesEnd <= pagesBegin || page >= (pagesEnd - pagesBegin) / 8) {
            return false;
        }
        const std::uintptr_t sparse = *reinterpret_cast<const std::uintptr_t*>(pagesBegin + page * 8);
        if (sparse == 0) {
            return false;
        }
        const std::uint32_t packed = *reinterpret_cast<const std::uint32_t*>(sparse + (low & 0x7ffu) * 4);
        if (((id & 0xfffc0000u) ^ packed) > 0x3fffeu) {
            return false;
        }
        const std::uintptr_t table = *reinterpret_cast<const std::uintptr_t*>(store + 0x50);
        if (table == 0) {
            return false;
        }
        const std::uintptr_t dense = *reinterpret_cast<const std::uintptr_t*>(table + ((packed >> 4) & 0x3ff8u));
        if (dense == 0) {
            return false;
        }
        at = dense + (packed & 0x7fu) * stride;
        return true;
    } __except (accessViolationFilter(GetExceptionCode())) {
        return false;
    }
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

    if (!active() || !input::isInGameplay()) {
        return;
    }
    dropUpDownBitsGuarded(static_cast<std::byte*>(input));
}

void FreeCamera::onMoveIntent(void* out, void* input)
{
    if (out == nullptr || !active() || !input::isInGameplay()) {
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

bool takeRawUpDownGuarded(std::byte* out, std::uint32_t* bits)
{
    __try {
        const std::uint32_t raw = *reinterpret_cast<const std::uint32_t*>(out + 0x00);
        *bits = raw;
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
    if (out == nullptr || !active() || !input::isInGameplay()) {
        return;
    }
    std::uint32_t bits = 0;
    if (!takeRawUpDownGuarded(static_cast<std::byte*>(out), &bits)) {
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
    if (out == nullptr || !active() || !input::isInGameplay()) {
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

bool FreeCamera::directionHeld() const
{
    return held(MoveKey::Forward) || held(MoveKey::Back) || held(MoveKey::Left) || held(MoveKey::Right);
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
    const auto skipIfMovss = [](std::byte* at, size_t size, const char* name, const wchar_t* what) {
        if (at == nullptr || movssStoreLength(at) != size) {
            log().warn(L"FreeCamera: {} is not a {}-byte movss; leaving it alone", what, size);
            return Patch{};
        }
        return makeSkipPatch(at, size, name);
    };

    if (std::byte* const base = Scanner::instance().address(Target::CameraUpdate);
        base != nullptr) {
        const auto positionPatch = [&](std::byte* store, const wchar_t* what) -> Patch {
            if ((reinterpret_cast<std::uintptr_t>(store) & 63) != 63) {
                return skipIfMovss(store, kWriteSize, "FreeCamera.CameraPosition", what);
            }
            const auto* const s = reinterpret_cast<const unsigned char*>(store);
            const auto* const l = s - kWriteSize;
            const bool shaped = memory::isReadable(l, 2 * kWriteSize) && movssStoreLength(store) == kWriteSize
                                && l[0] == 0xF3 && l[1] == 0x0F && l[2] == 0x10 && (l[3] >> 6) == 1 && (s[3] >> 6) == 1
                                && ((l[3] >> 3) & 7) == ((s[3] >> 3) & 7) && l[4] == s[4] && (l[3] & 7) != 4
                                && (s[3] & 7) != 4;
            if (!shaped) {
                log().warn(L"FreeCamera: {} is not a movss load/store pair; leaving it alone", what);
                return Patch{};
            }
            const auto modrm = static_cast<std::byte>((l[3] & 0xF8) | (s[3] & 7));
            return makeAtomicPatch(store - kWriteSize + 3, {modrm}, "FreeCamera.CameraPosition");
        };
        m_patchX = positionPatch(base + kWriteX, L"the camera x write");
        m_patchY = positionPatch(base + kWriteY, L"the camera y write");
        m_patchZ = positionPatch(base + kWriteZ, L"the camera z write");

        const std::byte* const ctx = Scanner::instance().address(Target::CameraUpdateContext);
        if (ctx != nullptr && ctx < base && base - ctx < 0x400) {
            const int offset = static_cast<int>(std::to_integer<unsigned>(ctx[0x11]));
            const int flag = static_cast<int>(std::to_integer<unsigned>(ctx[0x1A]));
            if (flag == offset + 0x18) {
                m_ctxOffset = offset;
            }
        }
        if (m_ctxOffset < 0) {
            log().warn(L"FreeCamera: the camera context was not found; FreeLook is unavailable");
        }
    }

    if (std::byte* const rot = Scanner::instance().address(Target::PlayerRotation);
        rot != nullptr) {
        m_patchYaw = skipIfMovss(rot + kWriteYaw, kWriteYawSize, "FreeCamera.BodyRotation", L"the body yaw write");
        m_patchPitch = skipIfMovss(rot + kWritePitch, kWritePitchSize, "FreeCamera.BodyRotation", L"the body pitch write");
        m_patchYawFollow = skipIfMovss(rot + kWriteYawFollow, kWriteYawFollowSize, "FreeCamera.BodyRotation",
                                       L"the body yaw-follow write");
    } else {
        log().warn(L"FreeCamera: the player rotation site was not found; "
                   L"the body will turn with the camera");
    }

    const auto nopIfMovss = [&](std::byte* at, size_t size, const wchar_t* what) {
        return skipIfMovss(at, size, "FreeCamera.HeadRotation", what);
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
        static constexpr std::byte kStub[] = {std::byte{0xB8}, kThirdPersonBack, std::byte{0x00}, std::byte{0x00},
                                              std::byte{0x00}, std::byte{0xC3}};
        m_patchPerspective = makeStubPatch(view, kStub, "FreeCamera.ThirdPerson");
        if (!m_patchPerspective.valid()) {
            log().warn(L"FreeCamera: the third-person stub was not placed (no room after the view perspective getter, "
                       L"or turned off in hooks.json); the view will stay in first person");
        }
    } else {
        log().warn(L"FreeCamera: the view perspective getter was not found; "
                   L"the view will stay in first person");
    }
}

MenuItem FreeCamera::buildMenu()
{
    std::vector<MenuItem> children;
    children.push_back(enabledItem());
    children.push_back(toggleKeyItem());
    children.push_back(menu::keybind(
        L"FreeCamera key", [this] { return m_cameraKey.combo(); },
        [this](std::vector<int> combo) {
            m_cameraKey.set(std::move(combo));
            log().info(L"FreeCamera: camera key set to {}", m_cameraKey.name());
        },
        {}));
    bindPad(children.back(), m_cameraKey);
    children.push_back(menu::keybind(
        L"FreeLook key", [this] { return m_lookKey.combo(); },
        [this](std::vector<int> combo) {
            m_lookKey.set(std::move(combo));
            log().info(L"FreeCamera: FreeLook key set to {}", m_lookKey.name());
        },
        {}));
    bindPad(children.back(), m_lookKey);
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

    std::vector<int> combo;
    if (const auto it = section.find("cameraKeys"); it != section.end() && it->is_array()) {
        for (const auto& value : *it) {
            if (value.is_number_integer()) {
                combo.push_back(value.get<int>());
            }
        }
    }
    m_cameraKey.set(std::move(combo));

    std::vector<int> lookCombo;
    if (const auto it = section.find("lookKeys"); it != section.end() && it->is_array()) {
        for (const auto& value : *it) {
            if (value.is_number_integer()) {
                lookCombo.push_back(value.get<int>());
            }
        }
    }
    m_lookKey.set(std::move(lookCombo));

    m_speed = std::clamp(Config::getFloat(section, "speed", kDefaultSpeed), kMinSpeed, kMaxSpeed);
}

void FreeCamera::saveConfig(nlohmann::json& section) const
{
    Module::saveConfig(section);
    section["cameraKeys"] = m_cameraKey.combo();
    section["lookKeys"] = m_lookKey.combo();
    section["speed"] = m_speed;
}

void FreeCamera::onEnabledChanged(bool enabled)
{
    if (!enabled) {
        setActive(false);
    }
}

void FreeCamera::onUpdate()
{
    const bool pressed = m_cameraKey.triggered();
    if (pressed && enabled() && input::isInGameplay() && m_look.load(std::memory_order_acquire) == kLookIdle) {
        setActive(!active());
    }
    updateLook();
}

void FreeCamera::freezeBody(bool on)
{
    Patch* const patches[] = {&m_patchYaw,        &m_patchPitch,     &m_patchYawFollow,
                              &m_patchHead,       &m_patchHeadPair,  &m_patchHeadAlt,
                              &m_patchHeadAltPair, &m_patchHeadInput, &m_patchHeadInputPair};
    Patch::setAll(patches, on);
}

void FreeCamera::updateLook()
{
    const bool want = enabled() && !active() && m_ctxOffset >= 0 && !m_lookKey.empty() && m_lookKey.isDown()
                      && input::isInGameplay();
    switch (m_look.load(std::memory_order_acquire)) {
    case kLookIdle:
        if (want) {
            if (!GameData::instance().hasLivePlayer()) {
                GameData::instance().findPlayerFromClient(hooks::gameClientInstance());
            }
            m_hasFrozenView = false;
            m_lookRestored.store(false, std::memory_order_release);
            m_lookSnapped.store(false, std::memory_order_release);
            m_lookSnapWanted.store(true, std::memory_order_release);
            m_look.store(kLookHolding, std::memory_order_release);
            freezeBody(true);
        }
        break;
    case kLookHolding:
        if (!want) {
            if (m_lookPerspective) {
                m_patchPerspective.restore();
                m_lookPerspective = false;
            }
            m_lookReleasedAt = GetTickCount64();
            m_look.store(kLookReleasing, std::memory_order_release);
        } else if (!m_lookPerspective && m_lookSnapped.load(std::memory_order_acquire)) {
            m_patchPerspective.apply();
            m_lookPerspective = true;
        }
        break;
    case kLookReleasing: {
        const bool restored = m_lookRestored.load(std::memory_order_acquire);
        if (restored || GetTickCount64() - m_lookReleasedAt >= kLookRestoreTimeoutMs) {
            if (!restored) {
                static std::atomic<int> warned{0};
                if (warned.fetch_add(1, std::memory_order_relaxed) < 3) {
                    log().warn(L"FreeLook: the original camera did not come back in time; released without "
                               L"turning the camera back");
                }
            }
            if (!active()) {
                freezeBody(false);
            }
            m_look.store(kLookIdle, std::memory_order_release);
        }
        break;
    }
    default:
        break;
    }
}

void FreeCamera::takeLookSnapshot(void* frame)
{
    m_lookSnapValid = false;
    LookSnapshot snap{};
    if (readCameraContext(frame, m_ctxOffset, snap.registry, snap.id)) {
        std::uintptr_t at = 0;
        if (cameraComponentAt(snap.registry, snap.id, kDirectLookTypeId, kDirectLookStride, at)) {
            snap.hasDirect = memory::copyGuarded(reinterpret_cast<const void*>(at), snap.direct, sizeof(snap.direct));
        }
        if (cameraComponentAt(snap.registry, snap.id, kOrbitTypeId, kOrbitStride, at)) {
            snap.hasOrbit = memory::copyGuarded(reinterpret_cast<const void*>(at + kOrbitAngles), snap.orbit,
                                                sizeof(snap.orbit));
        }
        m_lookSnap = snap;
        m_lookSnapValid = snap.hasDirect || snap.hasOrbit;
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 20) {
            log().info(L"FreeLook: held the camera direction (camera {:#x}:{:#x}, direct look {}, orbit {})",
                       snap.registry, snap.id, snap.hasDirect ? L"yes" : L"no", snap.hasOrbit ? L"yes" : L"no");
        }
    }
    m_lookSnapped.store(true, std::memory_order_release);
}

bool FreeCamera::restoreLookSnapshot(void* frame)
{
    if (!m_lookSnapValid) {
        return true;
    }
    std::uintptr_t registry = 0;
    std::uint32_t id = 0;
    if (!readCameraContext(frame, m_ctxOffset, registry, id) || registry != m_lookSnap.registry
        || id != m_lookSnap.id) {
        return false;
    }
    m_lookSnapValid = false;
    bool wroteDirect = false;
    bool wroteOrbit = false;
    std::uintptr_t at = 0;
    if (m_lookSnap.hasDirect && cameraComponentAt(registry, id, kDirectLookTypeId, kDirectLookStride, at)) {
        wroteDirect = memory::writeGuarded(reinterpret_cast<void*>(at), m_lookSnap.direct, sizeof(m_lookSnap.direct));
    }
    if (m_lookSnap.hasOrbit && cameraComponentAt(registry, id, kOrbitTypeId, kOrbitStride, at)) {
        wroteOrbit = memory::writeGuarded(reinterpret_cast<void*>(at + kOrbitAngles), m_lookSnap.orbit,
                                          sizeof(m_lookSnap.orbit));
    }
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 20) {
        log().info(L"FreeLook: turned the camera back (camera {:#x}:{:#x}; direct look {}, orbit {})", registry, id,
                   wroteDirect ? L"yes" : L"no", wroteOrbit ? L"yes" : L"no");
    }
    return true;
}

void FreeCamera::setActive(bool value)
{
    if (active() == value) {
        return;
    }
    if (value && !enabled()) {
        return;
    }
    if (value) {
        if (!GameData::instance().hasLivePlayer()) {
            GameData::instance().findPlayerFromClient(hooks::gameClientInstance());
        }
        m_synced = false;
        m_hasFrozenView = false;
        m_active.store(true, std::memory_order_release);
        log().info(L"FreeCamera: camera detached");

        Patch* const patches[] = {&m_patchX,          &m_patchY,           &m_patchZ,
                                  &m_patchYaw,        &m_patchPitch,       &m_patchYawFollow,
                                  &m_patchHead,       &m_patchHeadPair,    &m_patchHeadAlt,
                                  &m_patchHeadAltPair, &m_patchHeadInput,  &m_patchHeadInputPair,
                                  &m_patchPerspective};
        Patch::setAll(patches, true);

    } else {
        m_active.store(false, std::memory_order_release);
        log().info(L"FreeCamera: camera returned");

        Patch* const patches[] = {&m_patchX,          &m_patchY,           &m_patchZ,
                                  &m_patchYaw,        &m_patchPitch,       &m_patchYawFollow,
                                  &m_patchHead,       &m_patchHeadPair,    &m_patchHeadAlt,
                                  &m_patchHeadAltPair, &m_patchHeadInput,  &m_patchHeadInputPair,
                                  &m_patchPerspective};
        Patch::setAll(patches, false);

    }
}

bool FreeCamera::freezeViewVector(float* out)
{
    if (!freezesAim()) {
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
    if (active()) {
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
    Patch* const patches[] = {&m_patchX, &m_patchY, &m_patchZ};
    Patch::setAll(patches, true);
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
    Patch* const patches[] = {&m_patchX, &m_patchY, &m_patchZ};
    Patch::setAll(patches, false);
    m_borrowUntil = 0;
    m_borrowSynced = false;
    m_borrowQuietUntil.store(GetTickCount64() + kBorrowQuietMs, std::memory_order_relaxed);
    m_borrowActive.store(false, std::memory_order_release);
}

void FreeCamera::onCameraWrite(void* cameraBase, void* frame)
{
    if (cameraBase == nullptr) {
        return;
    }
    if (frame != nullptr && m_ctxOffset >= 0) {
        const int look = m_look.load(std::memory_order_acquire);
        if (look == kLookHolding && m_lookSnapWanted.exchange(false, std::memory_order_acq_rel)) {
            takeLookSnapshot(frame);
        } else if (look == kLookReleasing && !m_lookRestored.load(std::memory_order_acquire)
                   && restoreLookSnapshot(frame)) {
            m_lookRestored.store(true, std::memory_order_release);
        }
    }
    if (m_borrowUntil != 0) {
        if (applyBorrow(static_cast<std::byte*>(cameraBase))) {
            return;
        }
    }
    if (!active()) {
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
        m_staleIntentFrames = 0;
    } else if (directionHeld()) {
        if (++m_staleIntentFrames == kStaleIntentNotice) {
            notice::failOnce("FreeCamera.intent",
                             L"FreeCamera: the movement intent did not arrive while a direction was held for "
                                 + std::to_wstring(kStaleIntentNotice) + L" frames; the camera does not move sideways",
                             "FreeCamera cannot move: the game's movement input did not reach it");
        }
    } else {
        m_staleIntentFrames = 0;
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

    setActive(false);
    if (m_look.exchange(kLookIdle, std::memory_order_acq_rel) != kLookIdle) {
        freezeBody(false);
    }
    m_lookPerspective = false;
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
