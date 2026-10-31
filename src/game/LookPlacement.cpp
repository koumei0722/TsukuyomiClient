#include "game/LookPlacement.h"

#include "core/Logger.h"
#include "core/Notice.h"
#include "core/Strings.h"
#include "game/BlockWrite.h"
#include "game/DebugLines.h"
#include "game/GameData.h"
#include "game/ItemStackRequest.h"
#include "hooks/Detours.h"
#include "memory/Memory.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <mutex>
#include <optional>
#include <utility>

namespace tsukuyomi::lookplace {
namespace {
constexpr std::ptrdiff_t kRotationComponent = 0x250;
constexpr std::ptrdiff_t kPacketPitch = 0x30, kPacketYaw = 0x34, kPacketHeadYaw = 0x44;
constexpr std::ptrdiff_t kInteractPitch = 0x6c, kInteractYaw = 0x70, kViewVector = 0x74;
constexpr std::ptrdiff_t kAnalogMove = 0x5c, kMove = 0x64, kRawMove = 0x80, kInputBits = 0x88;
constexpr std::uint32_t kUp = 1U << 10, kDown = 1U << 11, kLeft = 1U << 12, kRight = 1U << 13, kUpLeft = 1U << 14, kUpRight = 1U << 15;
constexpr std::uint32_t kDirectionBits = kUp | kDown | kLeft | kRight | kUpLeft | kUpRight;
int g_moveMismatches = 0;
bool g_moveDisabled = false;
std::mutex g_mutex;
std::optional<Request> g_request;
bool g_sent = false, g_disabled = false, g_executing = false;
int g_mismatches = 0;
constexpr int kMismatchLimit = 3;
std::uint64_t g_at = 0, g_generation = 0;
unsigned g_lines = 0;
flexible::Look g_lastSent;
bool g_lastSentKnown = false;
std::atomic<std::uint64_t> g_inputs{0};
constexpr std::ptrdiff_t kPositionComponent = 0x240;
constexpr std::uint64_t kJumpDown = 1ULL << 3, kGroundHint = 1ULL << 50;
constexpr float kGroundFall = -0.0784F;
constexpr std::ptrdiff_t kBoxComponent = 0x248;
std::uint64_t g_lastBits = 0;
std::wstring g_motionFailure;
bool g_lastBitsKnown = false;
void record(const Request& request, const wchar_t* result)
{
    if (g_lines++ < 32) log().info(L"LookPlacement: {} {} at {}, {}, {} (pitch {}, yaw {})",
        toUtf16(request.owner), result, request.pos[0], request.pos[1], request.pos[2], request.look.pitch, request.look.yaw);
}
bool rotationAt(void* player, void*& rotation)
{
    return player && memory::copyGuarded(static_cast<const std::byte*>(player) + kRotationComponent,
        &rotation, sizeof(rotation)) && memory::plausiblePointer(rotation);
}
bool finite(flexible::Look look) { return std::isfinite(look.pitch) && std::isfinite(look.yaw); }
bool close(float a, float b) { return std::isfinite(a) && std::fabs(std::remainder(a - b, 360.0F)) <= 0.5F; }
void failPacket()
{
    g_disabled = true;
    if (g_request) record(*g_request, L"packet validation failed");
    g_request.reset(); g_sent = false;
    notice::failOnce("LookPlacement.packet", L"LookPlacement: PlayerAuthInput rotation layout could not be verified; placement rotation disabled",
        "LookPlacement: cannot verify PlayerAuthInput rotation; placement rotation disabled.");
}
void rotateMove(std::byte* packet, float delta)
{
    if (g_moveDisabled) return;
    float move[2]{}, raw[2]{}, analog[2]{};
    std::uint32_t bits = 0;
    if (!memory::copyGuarded(packet + kMove, move, sizeof(move)) || !memory::copyGuarded(packet + kRawMove, raw, sizeof(raw))
        || !memory::copyGuarded(packet + kAnalogMove, analog, sizeof(analog)) || !memory::copyGuarded(packet + kInputBits, &bits, sizeof(bits)))
        return;
    const auto plausible = [](const float v[2]) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::hypot(v[0], v[1]) <= 1.5F; };
    const bool agree = (!(bits & kUp) || raw[1] > 0.0F) && (!(bits & kDown) || raw[1] < 0.0F)
        && (!(bits & kLeft) || raw[0] > 0.0F) && (!(bits & kRight) || raw[0] < 0.0F);
    if (!plausible(move) || !plausible(raw) || !plausible(analog) || !agree) {
        static unsigned said = 0;
        if (said++ < 8) log().warn(L"LookPlacement: PlayerAuthInput move {}, {} / raw {}, {} / bits {:#x} did not verify ({} in a row)",
            move[0], move[1], raw[0], raw[1], bits, g_moveMismatches + 1);
        if (++g_moveMismatches >= kMismatchLimit) {
            g_moveDisabled = true;
            notice::failOnce("LookPlacement.move", L"LookPlacement: PlayerAuthInput move layout could not be verified; rotated placements may pull you back while walking",
                "LookPlacement: cannot verify the PlayerAuthInput move input; rotated placements may pull you back while walking.");
        }
        return;
    }
    g_moveMismatches = 0;
    if (move[0] == 0.0F && move[1] == 0.0F && raw[0] == 0.0F && raw[1] == 0.0F && analog[0] == 0.0F && analog[1] == 0.0F) return;
    constexpr float kPi = 3.14159265358979F;
    const float c = std::cos(delta * kPi / 180.0F), s = std::sin(delta * kPi / 180.0F);
    const auto turn = [&](float v[2]) { const float x = v[0] * c + v[1] * s, y = -v[0] * s + v[1] * c; v[0] = x; v[1] = y; };
    turn(move); turn(raw); turn(analog);
    constexpr float kPressed = 0.3F;
    bits &= ~kDirectionBits;
    if (raw[1] > kPressed) bits |= kUp;
    if (raw[1] < -kPressed) bits |= kDown;
    if (raw[0] > kPressed) bits |= kLeft;
    if (raw[0] < -kPressed) bits |= kRight;
    memory::writeGuarded(packet + kMove, move, sizeof(move));
    memory::writeGuarded(packet + kRawMove, raw, sizeof(raw));
    memory::writeGuarded(packet + kAnalogMove, analog, sizeof(analog));
    memory::writeGuarded(packet + kInputBits, &bits, sizeof(bits));
}
}
bool readLook(flexible::Look& out)
{
    void* rotation = nullptr;
    return rotationAt(GameData::instance().player(), rotation)
        && memory::copyGuarded(rotation, &out, sizeof(out)) && finite(out);
}
ScopedLook::ScopedLook(flexible::Look look)
{
    m_player = GameData::instance().player();
    if (!finite(look) || !rotationAt(m_player, m_rotation)
        || !memory::copyGuarded(m_rotation, &m_saved, sizeof(m_saved)) || !finite(m_saved)) return;
    m_ok = memory::writeGuarded(m_rotation, &look, sizeof(look));
    if (!m_ok) memory::writeGuarded(m_rotation, &m_saved, sizeof(m_saved));
}
ScopedLook::~ScopedLook()
{
    void* rotation = nullptr;
    if (m_ok && GameData::instance().player() == m_player && rotationAt(m_player, rotation) && rotation == m_rotation)
        memory::writeGuarded(m_rotation, &m_saved, sizeof(m_saved));
}
ScopedPosition::ScopedPosition(const float eye[3])
{
    m_player = GameData::instance().player();
    const auto* player = static_cast<const std::byte*>(m_player);
    if (!player || !eye || !std::isfinite(eye[0]) || !std::isfinite(eye[1]) || !std::isfinite(eye[2])
        || !memory::copyGuarded(player + kPositionComponent, &m_position, sizeof(m_position)) || !memory::plausiblePointer(m_position)
        || !memory::copyGuarded(player + kBoxComponent, &m_box, sizeof(m_box)) || !memory::plausiblePointer(m_box)
        || !memory::copyGuarded(m_position, m_savedPosition, sizeof(m_savedPosition))
        || !memory::copyGuarded(m_box, m_savedBox, sizeof(m_savedBox))) return;
    if (const int axis = easyplace::positionBoxMismatch(m_savedPosition, m_savedBox); axis >= 0) {
        static unsigned told = 0;
        if (told++ < 4) log().warn(L"LookPlacement: the player position/box layout did not verify on axis {} (position {}, box {}..{}); position query skipped",
            axis, m_savedPosition[axis], m_savedBox[axis], m_savedBox[axis + 3]);
        return;
    }
    float position[3]{}, box[6]{};
    for (int i = 0; i < 3; ++i) {
        const float delta = eye[i] - m_savedPosition[i];
        position[i] = eye[i]; box[i] = m_savedBox[i] + delta; box[i + 3] = m_savedBox[i + 3] + delta;
    }
    if (!memory::writeGuarded(m_position, position, sizeof(position))) return;
    if (!memory::writeGuarded(m_box, box, sizeof(box))) { memory::writeGuarded(m_position, m_savedPosition, sizeof(position)); return; }
    m_ok = true;
}
ScopedPosition::~ScopedPosition()
{
    if (!m_ok || GameData::instance().player() != m_player) return;
    memory::writeGuarded(m_box, m_savedBox, sizeof(m_savedBox));
    memory::writeGuarded(m_position, m_savedPosition, 3 * sizeof(float));
}
std::uint64_t inputsSent() { return g_inputs.load(std::memory_order_relaxed); }
bool readMotion(easyplace::Motion& out)
{
    const auto fail = [](std::wstring why) { std::lock_guard lock(g_mutex); g_motionFailure = std::move(why); return false; };
    const auto* player = static_cast<const std::byte*>(GameData::instance().player());
    const float* component = nullptr;
    const float* boxComponent = nullptr;
    float v[9]{}, box[6]{};
    if (!player) return fail(L"no player");
    if (!memory::copyGuarded(player + kPositionComponent, &component, sizeof(component)) || !memory::plausiblePointer(component)
        || !memory::copyGuarded(component, v, sizeof(v))
        || !memory::copyGuarded(player + kBoxComponent, &boxComponent, sizeof(boxComponent)) || !memory::plausiblePointer(boxComponent)
        || !memory::copyGuarded(boxComponent, box, sizeof(box))) return fail(L"the position or box component could not be read");
    if (const int axis = easyplace::positionBoxMismatch(v, box); axis >= 0)
        return fail(std::format(L"axis {}: position {} outside the box {}..{}", axis, v[axis], box[axis], box[axis + 3]));
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(v[6 + i]) || std::fabs(v[6 + i]) > 4.0F) return fail(std::format(L"axis {}: move {}", i, v[6 + i]));
    std::uint64_t bits = 0;
    bool known = false;
    { std::lock_guard lock(g_mutex); bits = g_lastBits; known = g_lastBitsKnown; }
    if (!known) return fail(L"no PlayerAuthInput has been seen yet");
    std::copy_n(v, 3, out.eye); std::copy_n(v + 6, 3, out.move);
    out.grounded = std::fabs(v[7] - kGroundFall) < 0.002F || (bits & kGroundHint) != 0;
    out.jumpHeld = (bits & kJumpDown) != 0;
    return true;
}
std::wstring motionFailure()
{
    std::lock_guard lock(g_mutex);
    return g_motionFailure;
}
bool lastSentLook(flexible::Look& out)
{
    std::lock_guard lock(g_mutex);
    if (g_lastSentKnown) out = g_lastSent;
    return g_lastSentKnown;
}
bool busy() { std::lock_guard lock(g_mutex); return g_request.has_value() || g_executing; }
bool schedule(Request request)
{
    std::lock_guard lock(g_mutex);
    if (g_disabled || g_request || g_executing || !request.gameMode || !finite(request.look)
        || request.gameMode != GameData::instance().gameMode()) {
        static unsigned said = 0;
        if (said++ < 8) log().warn(L"LookPlacement: {} was not scheduled (disabled {}, busy {}, game mode {})",
            toUtf16(request.owner), g_disabled, g_request.has_value() || g_executing,
            request.gameMode != nullptr && request.gameMode == GameData::instance().gameMode() ? L"ok" : L"mismatch");
        return false;
    }
    g_at = GetTickCount64(); g_generation = blockwrite::regionGeneration(); g_sent = false;
    record(request, L"scheduled");
    g_request = std::move(request);
    return true;
}
void onPacketSend(void* packet)
{
    if (ItemStackRequest::packetId(packet) != 0x90) return;
    g_inputs.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lock(g_mutex);
    g_lastBitsKnown = memory::copyGuarded(static_cast<const std::byte*>(packet) + kInputBits, &g_lastBits, sizeof(g_lastBits));
    if (!g_request) {
        flexible::Look sent;
        g_lastSentKnown = memory::copyGuarded(static_cast<const std::byte*>(packet) + kPacketPitch, &sent, sizeof(sent)) && finite(sent);
        if (g_lastSentKnown) g_lastSent = sent;
        return;
    }
    g_lastSentKnown = false;
    if (g_generation != blockwrite::regionGeneration() || g_request->gameMode != GameData::instance().gameMode()) {
        record(*g_request, L"world changed"); g_request.reset(); g_sent = false; return;
    }
    flexible::Look real, packetLook;
    auto* bytes = static_cast<std::byte*>(packet);
    if (!readLook(real) || !memory::copyGuarded(bytes + kPacketPitch, &packetLook, sizeof(packetLook))
        || !close(packetLook.pitch, real.pitch) || !close(packetLook.yaw, real.yaw)) {
        static unsigned said = 0;
        if (said++ < 8) log().warn(L"LookPlacement: PlayerAuthInput rotation {}, {} does not match the player {}, {} ({} in a row)",
            packetLook.pitch, packetLook.yaw, real.pitch, real.yaw, g_mismatches + 1);
        if (++g_mismatches >= kMismatchLimit) failPacket();
        return;
    }
    g_mismatches = 0;
    const auto look = g_request->look;
    float vector[3]{};
    debuglines::viewVector(look.pitch, look.yaw, vector);
    if (!memory::writeGuarded(bytes + kPacketPitch, &look.pitch, sizeof(float))
        || !memory::writeGuarded(bytes + kPacketYaw, &look.yaw, sizeof(float))
        || !memory::writeGuarded(bytes + kPacketHeadYaw, &look.yaw, sizeof(float))
        || !memory::writeGuarded(bytes + kInteractPitch, &look.pitch, sizeof(float))
        || !memory::writeGuarded(bytes + kInteractYaw, &look.yaw, sizeof(float))
        || !memory::writeGuarded(bytes + kViewVector, vector, sizeof(vector))) { failPacket(); return; }
    rotateMove(bytes, look.yaw - packetLook.yaw);
    if (!g_sent) record(*g_request, L"rotation sent");
    g_sent = true; g_lastSent = look; g_lastSentKnown = true;
}
void pump()
{
    std::optional<Request> request;
    {
        std::lock_guard lock(g_mutex);
        if (!g_request) return;
        if (g_generation != blockwrite::regionGeneration() || g_request->gameMode != GameData::instance().gameMode()) {
            record(*g_request, L"world changed"); g_request.reset(); g_sent = false; return;
        }
        if (!g_sent) {
            if (GetTickCount64() - g_at >= 500) { record(*g_request, L"timed out waiting for PlayerAuthInput"); g_request.reset(); }
            return;
        }
        request = std::move(g_request); g_request.reset(); g_sent = false; g_executing = true;
    }
    bool placed = false;
    if (!request->stillValid || request->stillValid()) {
        ScopedLook look(request->look);
        if (look.ok()) placed = hooks::callOriginalBuildBlock(request->gameMode, request->pos, request->face, request->click, 0, false);
    }
    std::lock_guard lock(g_mutex);
    record(*request, placed ? L"placed" : L"discarded or placement failed"); g_executing = false;
}
void cancel()
{
    std::lock_guard lock(g_mutex);
    if (g_request) record(*g_request, L"cancelled");
    g_request.reset(); g_sent = false;
}
}
