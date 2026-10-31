#include "render/BoxRenderer.h"

#include "core/Logger.h"
#include "core/Notice.h"
#include "memory/Memory.h"

#include <Windows.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <unordered_map>

namespace tsukuyomi::boxes {
namespace {

constexpr std::ptrdiff_t kCameraX = 0x40;
constexpr std::ptrdiff_t kCameraY = 0x44;
constexpr std::ptrdiff_t kCameraZ = 0x48;

constexpr std::ptrdiff_t kSourceProj = 0x9c;
constexpr std::ptrdiff_t kSourceView = kSourceProj + 0x40;
constexpr unsigned long long kCameraBadNoticeMs = 10000;

bool finiteFloat(float v)
{
    return std::isfinite(v) && std::fabs(v) < 1.0e7F;
}

bool nearly(float v, float want, float slack)
{
    return std::fabs(v - want) <= slack;
}

bool looksLikeProjection(const float* m)
{
    for (int i = 0; i < 16; ++i) {
        if (!finiteFloat(m[i])) {
            return false;
        }
    }
    if (!(m[0] > 0.05F && m[0] < 200.0F) || !(m[5] > 0.05F && m[5] < 200.0F)) {
        return false;
    }
    const int zeros[] = {1, 2, 3, 4, 6, 7, 8, 9, 12, 13, 15};
    for (const int at : zeros) {
        if (m[at] != 0.0F) {
            return false;
        }
    }
    if (!nearly(m[10], -1.0F, 0.01F) || !nearly(m[11], -1.0F, 0.001F)) {
        return false;
    }
    return m[14] < 0.0F && m[14] > -10.0F;
}

bool looksLikeView(const float* m)
{
    for (int i = 0; i < 16; ++i) {
        if (!finiteFloat(m[i])) {
            return false;
        }
    }
    if (m[3] != 0.0F || m[7] != 0.0F || m[11] != 0.0F) {
        return false;
    }
    if (m[12] != 0.0F || m[13] != 0.0F || m[14] != 0.0F || !nearly(m[15], 1.0F, 0.001F)) {
        return false;
    }
    for (int r = 0; r < 3; ++r) {
        const float* row = m + r * 4;
        const float len = std::sqrt(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]);
        if (len < 0.99F || len > 1.01F) {
            return false;
        }
    }
    for (int a = 0; a < 3; ++a) {
        for (int b = a + 1; b < 3; ++b) {
            const float* p = m + a * 4;
            const float* q = m + b * 4;
            if (std::fabs(p[0] * q[0] + p[1] * q[1] + p[2] * q[2]) > 0.01F) {
                return false;
            }
        }
    }
    return true;
}

struct Snapshot {
    float eye[3] = {0.0F, 0.0F, 0.0F};
    float vp[16] = {};
    bool valid = false;
};
std::mutex g_camLock;
Snapshot g_cam;
std::atomic<unsigned long long> g_cameraBadSince{0};
std::atomic<std::uintptr_t> g_cameraBadSource{0};

std::mutex g_boxLock;
std::unordered_map<schematic::SectionKey, bool, schematic::SectionKeyHash> g_requests;
bool g_requestPending = false, g_requestReset = false, g_requestXray = false;

std::atomic<bool> g_on{false};
std::atomic<float> g_faceAlpha{0.35F};
std::atomic<bool> g_xray{false};
std::atomic<unsigned long long> g_requestVersion{0};

constexpr unsigned long long kStyleStaleMs = 1000;
std::atomic<unsigned long long> g_styleAt{0};

bool styleFresh()
{
    const unsigned long long at = g_styleAt.load(std::memory_order_relaxed);
    if (at == 0) {
        return false;
    }
    return GetTickCount64() - at < kStyleStaleMs;
}

void multiply(const float* a, const float* b, float* out)
{
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            float sum = 0.0F;
            for (int k = 0; k < 4; ++k) {
                sum += a[r * 4 + k] * b[k * 4 + c];
            }
            out[r * 4 + c] = sum;
        }
    }
}

}

void noteCamera(void* cameraBase, void* source)
{
    if (cameraBase == nullptr || source == nullptr) {
        return;
    }
    auto* const base = static_cast<std::byte*>(cameraBase);

    float proj[16]{};
    float view[16]{};
    std::byte matrices[0x80];
    static_assert(kSourceView == kSourceProj + static_cast<std::ptrdiff_t>(sizeof(proj)));
    bool ok = memory::copyGuarded(static_cast<std::byte*>(source) + kSourceProj, matrices, sizeof(matrices));
    if (ok) {
        std::memcpy(proj, matrices, sizeof(proj));
        std::memcpy(view, matrices + sizeof(proj), sizeof(view));
        ok = looksLikeProjection(proj) && looksLikeView(view);
    }

    float eye[3];
    static_assert(kCameraY == kCameraX + sizeof(float) && kCameraZ == kCameraY + sizeof(float));
    if (ok
        && (!memory::copyGuarded(base + kCameraX, eye, sizeof(eye)) || !finiteFloat(eye[0]) || !finiteFloat(eye[1])
            || !finiteFloat(eye[2]))) {
        ok = false;
    }

    if (!ok) {
        const unsigned long long now = GetTickCount64();
        const unsigned long long since = g_cameraBadSince.load(std::memory_order_relaxed);
        const auto sourceAt = reinterpret_cast<std::uintptr_t>(source);
        const bool sameSource = g_cameraBadSource.exchange(sourceAt, std::memory_order_relaxed) == sourceAt;
        if (since == 0 || !sameSource) {
            g_cameraBadSince.store(now, std::memory_order_relaxed);
        } else if (now - since >= kCameraBadNoticeMs) {
            notice::failOnce("BoxRenderer.camera",
                             std::format(L"BoxRenderer: the camera matrices could not be read for {} ms (camera {:#x}, "
                                         L"source {:#x} +{:#x}); boxes are not drawn",
                                         now - since, reinterpret_cast<std::uintptr_t>(base),
                                         reinterpret_cast<std::uintptr_t>(source),
                                         static_cast<std::uintptr_t>(kSourceProj)),
                             "Boxes are not drawn: the camera could not be read");
        }
        std::lock_guard<std::mutex> guard(g_camLock);
        g_cam.valid = false;
        return;
    }
    g_cameraBadSince.store(0, std::memory_order_relaxed);

    Snapshot snap;
    snap.eye[0] = eye[0];
    snap.eye[1] = eye[1];
    snap.eye[2] = eye[2];
    multiply(view, proj, snap.vp);
    snap.valid = true;
    {
        std::lock_guard<std::mutex> guard(g_camLock);
        g_cam = snap;
    }
}

void pushSectionRequest(SectionRequest request)
{
    std::lock_guard lock(g_boxLock);
    if (request.reset) { g_requests.clear(); g_requestReset = true; }
    for (const auto& key : request.collect) g_requests[key] = true;
    for (const auto& key : request.remove) g_requests[key] = false;
    g_requestXray = request.xray;
    g_requestPending = true;
    g_requestVersion.fetch_add(1, std::memory_order_release);
}

bool takeSectionRequests(SectionRequest& merged)
{
    std::lock_guard lock(g_boxLock);
    if (!g_requestPending) return false;
    merged = {};
    merged.reset = g_requestReset; merged.xray = g_requestXray;
    for (const auto& [key, collect] : g_requests) {
        if (collect) merged.collect.push_back(key);
        else merged.remove.push_back(key);
    }
    g_requests.clear(); g_requestReset = false; g_requestPending = false;
    return true;
}

unsigned long long sectionRequestVersion() { return g_requestVersion.load(std::memory_order_acquire); }

void setStyle(bool on, float faceAlpha, bool xray)
{
    g_styleAt.store(GetTickCount64(), std::memory_order_relaxed);
    g_on.store(on, std::memory_order_relaxed);
    g_faceAlpha.store(faceAlpha, std::memory_order_relaxed);
    g_xray.store(xray, std::memory_order_relaxed);
}

bool cameraSnapshot(float eye[3], float vp[16])
{
    std::lock_guard<std::mutex> guard(g_camLock);
    if (!g_cam.valid) {
        return false;
    }
    std::memcpy(eye, g_cam.eye, sizeof(g_cam.eye));
    std::memcpy(vp, g_cam.vp, sizeof(g_cam.vp));
    return true;
}

bool boxesOn() { return g_on.load(std::memory_order_relaxed) && styleFresh(); }
float boxFaceAlpha() { return g_faceAlpha.load(std::memory_order_relaxed); }
bool boxXray() { return g_xray.load(std::memory_order_relaxed); }

namespace {

struct Rgb {
    float r;
    float g;
    float b;
};

constexpr Rgb kColors[4] = {
    {0.20F, 0.80F, 1.00F},
    {1.00F, 0.25F, 0.25F},
    {1.00F, 0.60F, 0.10F},
    {1.00F, 0.40F, 0.80F},
};

}

bool boxStyle(schematic::DiffColor color, float rgb[3], float* faceAlpha)
{
    const auto index = static_cast<std::size_t>(color);
    if (index == 0 || index > std::size(kColors)) {
        return false;
    }
    const Rgb& one = kColors[index - 1];
    rgb[0] = one.r;
    rgb[1] = one.g;
    rgb[2] = one.b;
    if (faceAlpha != nullptr) {
        *faceAlpha = g_faceAlpha.load(std::memory_order_relaxed);
    }
    return true;
}

}
