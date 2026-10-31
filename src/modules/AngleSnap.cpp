#include "modules/AngleSnap.h"

#include "config/Config.h"
#include "core/Logger.h"
#include "game/GameData.h"
#include "input/Foreground.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace tsukuyomi {

namespace {

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kDegToRad = kPi / 180.0f;

constexpr std::size_t kBaseYaw = 0;
constexpr std::size_t kPitch = 1;
constexpr std::size_t kYawOffset = 2;
constexpr std::size_t kPitchLimits = 3;

constexpr std::size_t kOrbitPitchLimits = 0xc / 4;
constexpr std::size_t kOrbitYawLimits = 0x14 / 4;
constexpr std::size_t kOrbitYaw = 0x34 / 4;
constexpr std::size_t kOrbitPitch = 0x38 / 4;

float wrapAngle(float angle)
{
    float folded = std::fmod(angle + kPi, 2.0f * kPi);
    if (folded < 0.0f) {
        folded += 2.0f * kPi;
    }
    return folded - kPi;
}

}

AngleSnap& AngleSnap::instance()
{
    static AngleSnap module;
    return module;
}

bool AngleSnap::available() const
{
    return Scanner::instance().found(Target::CameraDirectLookTurn);
}

void AngleSnap::onUpdate()
{
    const bool live = enabled() && available() && input::isInGameplay();
    const auto held = [live](const Hotkey& key) { return live && !key.empty() && key.isDown(); };
    const bool snapDown = held(m_snapKey);
    const std::uint8_t yaw = held(m_lockYawKey) ? kLock : (snapDown ? kSnap : kFree);
    const std::uint8_t pitch = held(m_lockPitchKey) ? kLock : (snapDown ? kSnap : kFree);
    const std::uint8_t mode = static_cast<std::uint8_t>(yaw | (pitch << 2));
    if (mode != m_mode.load(std::memory_order_acquire)) {
        m_pressSerial.fetch_add(1, std::memory_order_acq_rel);
        m_mode.store(mode, std::memory_order_release);
        static constexpr const wchar_t* kNames[] = {L"free", L"snap", L"lock"};
        const PlayerView view = GameData::instance().playerView();
        log().info(L"AngleSnap: yaw {} / pitch {} (step {:.0f}; player yaw {:.2f} pitch {:.2f})", kNames[yaw],
                   kNames[pitch], m_step.load(std::memory_order_acquire), view.yaw, view.pitch);
    }
}

AngleSnap::Track& AngleSnap::trackFor(const float* component)
{
    for (Track& slot : m_tracks) {
        if (slot.component == component) {
            return slot;
        }
    }
    Track& slot = m_tracks[m_nextTrack];
    m_nextTrack = (m_nextTrack + 1) % std::size(m_tracks);
    slot = Track{};
    slot.component = component;
    return slot;
}

AngleSnap::Angles AngleSnap::snap(Track& track, Angles before, Angles now, float low, float high)
{
    const std::uint32_t serial = m_pressSerial.load(std::memory_order_acquire);
    constexpr float kSame = 1.0e-3f;
    const bool continued = track.seeded && track.serial == serial
                           && std::fabs(wrapAngle(before.yaw - track.wrote.yaw)) < kSame
                           && std::fabs(before.pitch - track.wrote.pitch) < kSame;
    if (continued) {
        track.raw.yaw = wrapAngle(track.raw.yaw + wrapAngle(now.yaw - before.yaw));
        track.raw.pitch += now.pitch - before.pitch;
    } else {
        if (track.seeded && track.serial == serial) {
            static std::atomic<int> logged{0};
            if (logged.fetch_add(1, std::memory_order_relaxed) < 10) {
                log().info(L"AngleSnap: the direction was changed by someone else ({:.2f}, {:.2f} -> {:.2f}, {:.2f})",
                           track.wrote.yaw / kDegToRad, track.wrote.pitch / kDegToRad, before.yaw / kDegToRad,
                           before.pitch / kDegToRad);
            }
        }
        track.raw = {wrapAngle(now.yaw), now.pitch};
        track.lock = {wrapAngle(before.yaw), std::clamp(before.pitch, low, high)};
        track.serial = serial;
        track.seeded = true;
    }
    track.raw.pitch = std::clamp(track.raw.pitch, low, high);

    const std::uint8_t mode = m_mode.load(std::memory_order_acquire);
    const std::uint8_t yawMode = mode & 3;
    const std::uint8_t pitchMode = (mode >> 2) & 3;
    const float step = std::clamp(m_step.load(std::memory_order_acquire), kMinStep, kMaxStep) * kDegToRad;
    const auto pick = [step](std::uint8_t how, float raw, float lock) {
        return how == kLock ? lock : how == kSnap ? std::round(raw / step) * step : raw;
    };
    Angles snapped{pick(yawMode, track.raw.yaw, track.lock.yaw), pick(pitchMode, track.raw.pitch, track.lock.pitch)};
    if (yawMode == kSnap && snapped.yaw > kPi - 1.0e-3f) {
        snapped.yaw -= 2.0f * kPi;
    }
    snapped.pitch = std::clamp(snapped.pitch, low, high);
    if (!continued || (yawMode != kFree && snapped.yaw != track.wrote.yaw)
        || (pitchMode != kFree && snapped.pitch != track.wrote.pitch)) {
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 200) {
            log().info(L"AngleSnap: yaw {:.2f} pitch {:.2f}", snapped.yaw / kDegToRad, snapped.pitch / kDegToRad);
        }
    }
    track.wrote = snapped;
    return snapped;
}

void AngleSnap::afterTurn(float* look, const float before[3])
{
    float now[5]{};
    if (look == nullptr || !memory::copyGuarded(look, now, sizeof(now)) || !std::isfinite(now[kBaseYaw])
        || !std::isfinite(now[kPitch]) || !std::isfinite(now[kYawOffset])) {
        return;
    }
    const auto toPlayer = [](const float* f) {
        return Angles{wrapAngle(kPi - (f[kBaseYaw] + f[kYawOffset])), -f[kPitch]};
    };
    float low = -kPi * 0.5f;
    float high = kPi * 0.5f;
    if (std::isfinite(now[kPitchLimits]) && std::isfinite(now[kPitchLimits + 1])
        && now[kPitchLimits] < now[kPitchLimits + 1]) {
        low = -now[kPitchLimits + 1];
        high = -now[kPitchLimits];
    }
    Track& track = trackFor(look);
    const Angles snapped = snap(track, toPlayer(before), toPlayer(now), low, high);
    const float eyeNow = now[kBaseYaw] + now[kYawOffset];
    const float wrote[2] = {-snapped.pitch, now[kYawOffset] + wrapAngle((kPi - snapped.yaw) - eyeNow)};
    if (!memory::writeGuarded(look + kPitch, wrote, sizeof(wrote))) {
        track.seeded = false;
    }
}

void AngleSnap::afterOrbitTurn(float* orbit, const float before[2])
{
    float now[kOrbitPitch + 1]{};
    if (orbit == nullptr || !memory::copyGuarded(orbit, now, sizeof(now)) || !std::isfinite(now[kOrbitYaw])
        || !std::isfinite(now[kOrbitPitch])) {
        return;
    }
    const auto toPlayer = [](float yaw, float polar) {
        return Angles{wrapAngle(yaw + kPi * 0.5f), kPi * 0.5f - polar};
    };
    float low = -kPi * 0.5f;
    float high = kPi * 0.5f;
    if (std::isfinite(now[kOrbitPitchLimits]) && std::isfinite(now[kOrbitPitchLimits + 1])
        && now[kOrbitPitchLimits] < now[kOrbitPitchLimits + 1]) {
        low = kPi * 0.5f - now[kOrbitPitchLimits + 1];
        high = kPi * 0.5f - now[kOrbitPitchLimits];
    }
    Track& track = trackFor(orbit);
    const Angles snapped =
        snap(track, toPlayer(before[0], before[1]), toPlayer(now[kOrbitYaw], now[kOrbitPitch]), low, high);
    float wrote[2] = {wrapAngle(snapped.yaw - kPi * 0.5f), kPi * 0.5f - snapped.pitch};
    if (std::isfinite(now[kOrbitYawLimits]) && std::isfinite(now[kOrbitYawLimits + 1])
        && now[kOrbitYawLimits] < now[kOrbitYawLimits + 1]) {
        wrote[0] = std::clamp(wrote[0], now[kOrbitYawLimits], now[kOrbitYawLimits + 1]);
    }
    if (!memory::writeGuarded(orbit + kOrbitYaw, wrote, sizeof(wrote))) {
        track.seeded = false;
    }
}

MenuItem AngleSnap::buildMenu()
{
    std::vector<MenuItem> children;
    children.push_back(enabledItem());
    children.push_back(toggleKeyItem());
    const auto keyRow = [this, &children](const wchar_t* title, Hotkey& key) {
        children.push_back(menu::keybind(
            title, [&key] { return key.combo(); },
            [title, &key](std::vector<int> combo) {
                key.set(std::move(combo));
                log().info(L"AngleSnap: {} set to {}", title, key.name());
            },
            {}));
        bindPad(children.back(), key);
    };
    keyRow(L"Snap key", m_snapKey);
    keyRow(L"Lock yaw key", m_lockYawKey);
    keyRow(L"Lock pitch key", m_lockPitchKey);
    children.push_back(menu::number(
        L"Step", [this] { return m_step.load(std::memory_order_acquire); },
        [this](float value) { m_step.store(std::clamp(std::round(value), kMinStep, kMaxStep), std::memory_order_release); },
        true, kMinStep, kMaxStep));

    MenuItem item = menu::submenu(name(), std::move(children));
    item.available = [this] { return available(); };
    item.isOn = [this] { return enabled(); };
    return item;
}

void AngleSnap::loadConfig(const nlohmann::json& section)
{
    Module::loadConfig(section);

    const auto loadKey = [&section](const char* field, Hotkey& key) {
        std::vector<int> combo;
        if (const auto it = section.find(field); it != section.end() && it->is_array()) {
            for (const auto& value : *it) {
                if (value.is_number_integer()) {
                    combo.push_back(value.get<int>());
                }
            }
        }
        key.set(std::move(combo));
    };
    loadKey("snapKeys", m_snapKey);
    loadKey("lockYawKeys", m_lockYawKey);
    loadKey("lockPitchKeys", m_lockPitchKey);

    m_step.store(std::clamp(std::round(Config::getFloat(section, "step", kDefaultStep)), kMinStep, kMaxStep),
                 std::memory_order_release);
}

void AngleSnap::saveConfig(nlohmann::json& section) const
{
    Module::saveConfig(section);
    section["snapKeys"] = m_snapKey.combo();
    section["lockYawKeys"] = m_lockYawKey.combo();
    section["lockPitchKeys"] = m_lockPitchKey.combo();
    section["step"] = m_step.load(std::memory_order_acquire);
}

}
