#include "render/BoxRenderer.h"
#include "render/WorldMesh.h"

#include "core/Logger.h"
#include "core/Paths.h"
#include "memory/Memory.h"

#include <Windows.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace tsukuyomi::boxes {
namespace {

constexpr std::ptrdiff_t kCameraX = 0x40;
constexpr std::ptrdiff_t kCameraY = 0x44;
constexpr std::ptrdiff_t kCameraZ = 0x48;

constexpr std::ptrdiff_t kSearchFrom = -0x1000;
constexpr std::ptrdiff_t kSearchTo = 0x400;

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
    if (!(m[0] > 0.05F && m[0] < 10.0F) || !(m[5] > 0.05F && m[5] < 10.0F)) {
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

std::atomic<std::ptrdiff_t> g_projAt{0};
std::atomic<bool> g_projFound{false};

struct Snapshot {
    float eye[3] = {0.0F, 0.0F, 0.0F};
    float vp[16] = {};
    bool valid = false;
};
std::mutex g_camLock;
Snapshot g_cam;
constexpr std::ptrdiff_t kPerspectiveStride = 0x120;
std::atomic<int> g_perspective{0};
std::atomic<unsigned long long> g_perspectiveAt{0};
constexpr unsigned long long kPerspectiveStaleMs = 1000;
std::atomic<unsigned> g_perspectiveViewMissing{0};

std::mutex g_boxLock;
std::shared_ptr<const std::vector<blocks::DiffBox>> g_boxes;

std::atomic<bool> g_on{false};
std::atomic<float> g_faceAlpha{0.35F};
std::atomic<bool> g_xray{false};
std::atomic<unsigned long long> g_boxVersion{0};

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

void noteCamera(void* cameraBase)
{
    if (cameraBase == nullptr) {
        return;
    }
    auto* const base = static_cast<std::byte*>(cameraBase);

    std::ptrdiff_t at = g_projAt.load(std::memory_order_relaxed);
    float proj[16];
    float view[16];
    bool ok = false;
    std::byte savedMatrices[0x80];
    if (g_projFound.load(std::memory_order_acquire)
        && memory::copyGuarded(base + at, savedMatrices, sizeof(savedMatrices))) {
        std::memcpy(proj, savedMatrices, sizeof(proj));
        std::memcpy(view, savedMatrices + 0x40, sizeof(view));
        ok = looksLikeProjection(proj) && looksLikeView(view);
    }
    if (!ok) {
        g_projFound.store(false, std::memory_order_release);
        constexpr std::ptrdiff_t kStep = 0x200;
        for (std::ptrdiff_t block = kSearchFrom; block < kSearchTo && !ok; block += kStep) {
            const auto span = static_cast<std::size_t>(kStep + 0x80);
            if (!memory::isReadable(base + block, span)) {
                continue;
            }
            for (std::ptrdiff_t off = block; off < block + kStep; off += 4) {
                if (off + 0x80 > kSearchTo) {
                    break;
                }
                std::memcpy(proj, base + off, sizeof(proj));
                if (!looksLikeProjection(proj)) {
                    continue;
                }
                std::memcpy(view, base + off + 0x40, sizeof(view));
                if (!looksLikeView(view)) {
                    continue;
                }
                at = off;
                ok = true;
                g_projAt.store(off, std::memory_order_relaxed);
                g_projFound.store(true, std::memory_order_release);
                break;
            }
        }
        if (!ok) {
            static std::atomic<unsigned> told{0};
            if (told.fetch_add(1, std::memory_order_relaxed) < 3) {
                log().warn(L"BoxRenderer: the projection matrix was not found (camera {:#x})",
                           reinterpret_cast<std::uintptr_t>(base));
            }
        }
    }
    if (!ok) {
        std::lock_guard<std::mutex> guard(g_camLock);
        g_cam.valid = false;
        return;
    }

    float eye[3];
    static_assert(kCameraY == kCameraX + sizeof(float) && kCameraZ == kCameraY + sizeof(float));
    if (!memory::copyGuarded(base + kCameraX, eye, sizeof(eye))
        || !finiteFloat(eye[0]) || !finiteFloat(eye[1]) || !finiteFloat(eye[2])) {
        std::lock_guard<std::mutex> guard(g_camLock);
        g_cam.valid = false;
        return;
    }

    const unsigned long long perspectiveAt = g_perspectiveAt.load(std::memory_order_acquire);
    const int perspective = g_perspective.load(std::memory_order_relaxed);
    if (perspectiveAt != 0 && GetTickCount64() - perspectiveAt < kPerspectiveStaleMs
        && (perspective == 1 || perspective == 2)) {
        float other[16];
        if (memory::copyGuarded(base + at + 0x40 + perspective * kPerspectiveStride, other, sizeof(other))
            && looksLikeView(other)) {
            std::memcpy(view, other, sizeof(view));
        } else if (g_perspectiveViewMissing.fetch_add(1, std::memory_order_relaxed) == 0) {
            log().warn(L"BoxRenderer: the view matrix for perspective {} was not found (camera {:#x}, +{:#x}); "
                       L"using the first person one",
                       perspective, reinterpret_cast<std::uintptr_t>(base),
                       static_cast<std::uintptr_t>(at + 0x40 + perspective * kPerspectiveStride));
        }
    }
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

void setBoxes(std::vector<blocks::DiffBox> list)
{
    auto shared = std::make_shared<const std::vector<blocks::DiffBox>>(std::move(list));
    std::lock_guard<std::mutex> guard(g_boxLock);
    g_boxes = std::move(shared);
    g_boxVersion.fetch_add(1, std::memory_order_relaxed);
}

void setStyle(bool on, float faceAlpha, bool xray)
{
    g_styleAt.store(GetTickCount64(), std::memory_order_relaxed);
    g_on.store(on, std::memory_order_relaxed);
    g_faceAlpha.store(faceAlpha, std::memory_order_relaxed);
    if (g_xray.exchange(xray, std::memory_order_relaxed) != xray) {
        g_boxVersion.fetch_add(1, std::memory_order_relaxed);
    }
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

void noteViewPerspective(int perspective)
{
    g_perspective.store(perspective, std::memory_order_relaxed);
    g_perspectiveAt.store(GetTickCount64(), std::memory_order_release);
}

std::shared_ptr<const std::vector<blocks::DiffBox>> boxSnapshot()
{
    std::lock_guard<std::mutex> guard(g_boxLock);
    return g_boxes;
}

bool boxesOn() { return g_on.load(std::memory_order_relaxed) && styleFresh(); }
float boxFaceAlpha() { return g_faceAlpha.load(std::memory_order_relaxed); }
bool boxXray() { return g_xray.load(std::memory_order_relaxed); }
unsigned long long boxVersion() { return g_boxVersion.load(std::memory_order_relaxed); }

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

constexpr int kFaces[6][4] = {
    {0, 1, 3, 2},
    {4, 6, 7, 5},
    {0, 4, 5, 1},
    {2, 3, 7, 6},
    {0, 2, 6, 4},
    {1, 5, 7, 3},
};

}

bool boxStyle(blocks::DiffColor color, float rgb[3], float* faceAlpha)
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
