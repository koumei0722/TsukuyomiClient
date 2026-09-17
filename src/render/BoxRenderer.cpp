#include "render/BoxRenderer.h"
#include "render/WorldMesh.h"

#include "core/Logger.h"
#include "core/Paths.h"
#include "memory/Memory.h"

#include <Windows.h>

#include <d2d1_1.h>
#include <wrl/client.h>

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

using Microsoft::WRL::ComPtr;

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

std::mutex g_boxLock;
std::shared_ptr<const std::vector<blocks::DiffBox>> g_boxes;

std::atomic<bool> g_on{false};
std::atomic<float> g_faceAlpha{0.35F};
std::atomic<bool> g_xray{false};
std::atomic<unsigned long long> g_boxVersion{0};

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
    if (g_projFound.load(std::memory_order_acquire)
        && memory::isReadable(base + at, 0x80)) {
        std::memcpy(proj, base + at, sizeof(proj));
        std::memcpy(view, base + at + 0x40, sizeof(view));
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
    std::memcpy(&eye[0], base + kCameraX, sizeof(float));
    std::memcpy(&eye[1], base + kCameraY, sizeof(float));
    std::memcpy(&eye[2], base + kCameraZ, sizeof(float));
    if (!finiteFloat(eye[0]) || !finiteFloat(eye[1]) || !finiteFloat(eye[2])) {
        std::lock_guard<std::mutex> guard(g_camLock);
        g_cam.valid = false;
        return;
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

std::shared_ptr<const std::vector<blocks::DiffBox>> boxSnapshot()
{
    std::lock_guard<std::mutex> guard(g_boxLock);
    return g_boxes;
}

bool boxesOn() { return g_on.load(std::memory_order_relaxed); }
float boxFaceAlpha() { return g_faceAlpha.load(std::memory_order_relaxed); }
bool boxXray() { return g_xray.load(std::memory_order_relaxed); }
unsigned long long boxVersion() { return g_boxVersion.load(std::memory_order_relaxed); }

namespace {

struct Rgb {
    float r;
    float g;
    float b;
};

constexpr Rgb kColors[3] = {
    {0.20F, 0.80F, 1.00F},
    {1.00F, 0.25F, 0.25F},
    {1.00F, 0.60F, 0.10F},
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

bool wantsDraw()
{
    if (!g_on.load(std::memory_order_relaxed)) {
        return false;
    }
    if (worldmesh::active(g_xray.load(std::memory_order_relaxed))) {
        return false;
    }
    {
        std::lock_guard<std::mutex> guard(g_camLock);
        if (!g_cam.valid) {
            return false;
        }
    }
    std::lock_guard<std::mutex> guard(g_boxLock);
    return g_boxes && !g_boxes->empty();
}

void draw(ID2D1DeviceContext* context, float width, float height)
{
    if (context == nullptr || width < 1.0F || height < 1.0F) {
        return;
    }
    if (!g_on.load(std::memory_order_relaxed)) {
        return;
    }
    if (depthReady()) {
        return;
    }
    if (worldmesh::active(g_xray.load(std::memory_order_relaxed))) {
        return;
    }
    Snapshot cam;
    {
        std::lock_guard<std::mutex> guard(g_camLock);
        cam = g_cam;
    }
    if (!cam.valid) {
        return;
    }
    std::shared_ptr<const std::vector<blocks::DiffBox>> boxes;
    {
        std::lock_guard<std::mutex> guard(g_boxLock);
        boxes = g_boxes;
    }
    if (!boxes || boxes->empty()) {
        return;
    }

    ComPtr<ID2D1Factory> factory;
    context->GetFactory(&factory);
    if (!factory) {
        return;
    }

    const float faceAlpha = g_faceAlpha.load(std::memory_order_relaxed);
    const float edgeAlpha = faceAlpha * 2.2F > 1.0F ? 1.0F : faceAlpha * 2.2F;

    ComPtr<ID2D1PathGeometry> geometry[3];
    ComPtr<ID2D1GeometrySink> sink[3];
    for (int i = 0; i < 3; ++i) {
        if (FAILED(factory->CreatePathGeometry(&geometry[i]))) {
            return;
        }
        if (FAILED(geometry[i]->Open(&sink[i]))) {
            return;
        }
        sink[i]->SetFillMode(D2D1_FILL_MODE_WINDING);
    }

    for (const blocks::DiffBox& box : *boxes) {
        const int slot = static_cast<int>(box.color) - 1;
        if (slot < 0 || slot > 2) {
            continue;
        }
        D2D1_POINT_2F pt[8];
        bool usable = true;
        for (int i = 0; i < 8; ++i) {
            const float px = static_cast<float>(box.x) + ((i & 4) != 0 ? 1.0F : 0.0F);
            const float py = static_cast<float>(box.y) + ((i & 2) != 0 ? 1.0F : 0.0F);
            const float pz = static_cast<float>(box.z) + ((i & 1) != 0 ? 1.0F : 0.0F);
            const float dx = px - cam.eye[0];
            const float dy = py - cam.eye[1];
            const float dz = pz - cam.eye[2];
            const float cx = dx * cam.vp[0] + dy * cam.vp[4] + dz * cam.vp[8] + cam.vp[12];
            const float cy = dx * cam.vp[1] + dy * cam.vp[5] + dz * cam.vp[9] + cam.vp[13];
            const float cw = dx * cam.vp[3] + dy * cam.vp[7] + dz * cam.vp[11] + cam.vp[15];
            if (!(cw > 0.05F) || !finiteFloat(cx) || !finiteFloat(cy)) {
                usable = false;
                break;
            }
            pt[i].x = (cx / cw * 0.5F + 0.5F) * width;
            pt[i].y = (0.5F - cy / cw * 0.5F) * height;
        }
        if (!usable) {
            continue;
        }
        for (const auto& face : kFaces) {
            const D2D1_POINT_2F& a = pt[face[0]];
            const D2D1_POINT_2F& b = pt[face[1]];
            const D2D1_POINT_2F& c = pt[face[2]];
            const D2D1_POINT_2F& d = pt[face[3]];
            const float area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
            if (area <= 0.0F) {
                continue;
            }
            sink[slot]->BeginFigure(a, D2D1_FIGURE_BEGIN_FILLED);
            const D2D1_POINT_2F rest[3] = {b, c, d};
            sink[slot]->AddLines(rest, 3);
            sink[slot]->EndFigure(D2D1_FIGURE_END_CLOSED);
        }
    }

    for (int i = 0; i < 3; ++i) {
        sink[i]->Close();
    }

    for (int i = 0; i < 3; ++i) {
        ComPtr<ID2D1SolidColorBrush> fill;
        ComPtr<ID2D1SolidColorBrush> line;
        if (FAILED(context->CreateSolidColorBrush(
                D2D1::ColorF(kColors[i].r, kColors[i].g, kColors[i].b, faceAlpha), &fill))) {
            continue;
        }
        if (FAILED(context->CreateSolidColorBrush(
                D2D1::ColorF(kColors[i].r, kColors[i].g, kColors[i].b, edgeAlpha), &line))) {
            continue;
        }
        context->FillGeometry(geometry[i].Get(), fill.Get());
        context->DrawGeometry(geometry[i].Get(), line.Get(), 1.5F);
    }
}

}
