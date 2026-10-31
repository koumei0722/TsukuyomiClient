#include "game/SkinFaceLogic.h"

#include <algorithm>
#include <cmath>

namespace tsukuyomi::skinface {
namespace {
constexpr float kEpsilon = 0.02f;
constexpr float kHeadMinX = -4.0f, kHeadMaxX = 4.0f, kHeadMinY = 24.0f, kHeadMaxY = 32.0f, kHeadFrontZ = -4.0f;
constexpr float kOverlayNearest = -4.6f;

struct Rect {
    float minX = 0, maxX = 0, minY = 0, maxY = 0, z = 0;
    int texture = 0;
    float uv[2][2][2]{};
};

bool isFront(const Quad& q)
{
    if (std::fabs(q.normal[0]) > kEpsilon || std::fabs(q.normal[1]) > kEpsilon || std::fabs(q.normal[2] + 1.0f) > kEpsilon) {
        return false;
    }
    for (int i = 1; i < 4; ++i) {
        if (std::fabs(q.position[i][2] - q.position[0][2]) > kEpsilon) return false;
    }
    return true;
}

bool toRect(const Quad& q, Rect& out)
{
    out.minX = out.maxX = q.position[0][0];
    out.minY = out.maxY = q.position[0][1];
    for (int i = 1; i < 4; ++i) {
        out.minX = std::min(out.minX, q.position[i][0]);
        out.maxX = std::max(out.maxX, q.position[i][0]);
        out.minY = std::min(out.minY, q.position[i][1]);
        out.maxY = std::max(out.maxY, q.position[i][1]);
    }
    out.z = q.position[0][2];
    out.texture = q.texture;
    if (out.maxX - out.minX < kEpsilon || out.maxY - out.minY < kEpsilon) return false;
    bool used[4]{};
    for (int ix = 0; ix < 2; ++ix) {
        for (int iy = 0; iy < 2; ++iy) {
            const float cx = ix == 0 ? out.minX : out.maxX;
            const float cy = iy == 0 ? out.minY : out.maxY;
            int best = -1;
            for (int i = 0; i < 4; ++i) {
                if (!used[i] && std::fabs(q.position[i][0] - cx) < kEpsilon && std::fabs(q.position[i][1] - cy) < kEpsilon) {
                    best = i;
                    break;
                }
            }
            if (best < 0) return false;
            used[best] = true;
            out.uv[ix][iy][0] = q.uv[best][0];
            out.uv[ix][iy][1] = q.uv[best][1];
        }
    }
    return true;
}

void uvAt(const Rect& r, float x, float y, float& u, float& v)
{
    const float s = std::clamp((x - r.minX) / (r.maxX - r.minX), 0.0f, 1.0f);
    const float t = std::clamp((y - r.minY) / (r.maxY - r.minY), 0.0f, 1.0f);
    for (int k = 0; k < 2; ++k) {
        const float bottom = r.uv[0][0][k] + (r.uv[1][0][k] - r.uv[0][0][k]) * s;
        const float top = r.uv[0][1][k] + (r.uv[1][1][k] - r.uv[0][1][k]) * s;
        (k == 0 ? u : v) = bottom + (top - bottom) * t;
    }
}

bool contains(const Rect& r, float x, float y)
{
    return x >= r.minX - kEpsilon && x <= r.maxX + kEpsilon && y >= r.minY - kEpsilon && y <= r.maxY + kEpsilon;
}

bool samplePixel(const Rect& r, float x, float y, const std::vector<Texture>& textures, std::uint8_t* rgba)
{
    if (r.texture < 0 || static_cast<std::size_t>(r.texture) >= textures.size()) return false;
    const Texture& texture = textures[static_cast<std::size_t>(r.texture)];
    if (texture.width <= 0 || texture.height <= 0 || !texture.sample) return false;
    float u = 0, v = 0;
    uvAt(r, x, y, u, v);
    const int px = std::clamp(static_cast<int>(std::floor(u * static_cast<float>(texture.width))), 0, texture.width - 1);
    const int py = std::clamp(static_cast<int>(std::floor((1.0f - v) * static_cast<float>(texture.height))), 0,
                              texture.height - 1);
    return texture.sample(px, py, rgba);
}
}

bool personaFace(const std::vector<Quad>& quads, const std::vector<Texture>& textures, int side, std::uint8_t* out)
{
    if (side <= 0 || out == nullptr || textures.empty()) return false;
    Rect base;
    bool haveBase = false;
    std::vector<Rect> overlays;
    for (const Quad& q : quads) {
        Rect r;
        if (!isFront(q) || !toRect(q, r)) continue;
        if (!haveBase && std::fabs(r.z - kHeadFrontZ) < kEpsilon && r.minX <= kHeadMinX + kEpsilon
            && r.maxX >= kHeadMaxX - kEpsilon && r.minY <= kHeadMinY + kEpsilon && r.maxY >= kHeadMaxY - kEpsilon) {
            base = r;
            haveBase = true;
        } else if (r.z < kHeadFrontZ - kEpsilon && r.z >= kOverlayNearest - kEpsilon && r.maxX > kHeadMinX
                   && r.minX < kHeadMaxX && r.maxY > kHeadMinY && r.minY < kHeadMaxY) {
            overlays.push_back(r);
        }
    }
    if (!haveBase) return false;
    std::stable_sort(overlays.begin(), overlays.end(), [](const Rect& a, const Rect& b) { return a.z > b.z; });
    float uLeft = 0, uRight = 0, vIgnored = 0;
    uvAt(base, kHeadMinX, kHeadMaxY, uLeft, vIgnored);
    uvAt(base, kHeadMaxX, kHeadMaxY, uRight, vIgnored);
    const bool xIncreases = uLeft <= uRight;
    const float cell = (kHeadMaxX - kHeadMinX) / static_cast<float>(side);
    for (int py = 0; py < side; ++py) {
        const float y = kHeadMaxY - (static_cast<float>(py) + 0.5f) * cell;
        for (int px = 0; px < side; ++px) {
            const float offset = (static_cast<float>(px) + 0.5f) * cell;
            const float x = xIncreases ? kHeadMinX + offset : kHeadMaxX - offset;
            std::uint8_t* o = out + (static_cast<std::size_t>(py) * side + px) * 4;
            if (!samplePixel(base, x, y, textures, o)) return false;
            for (const Rect& r : overlays) {
                if (!contains(r, x, y)) continue;
                std::uint8_t h[4]{};
                if (!samplePixel(r, x, y, textures, h)) continue;
                const unsigned a = h[3];
                for (int c = 0; c < 3; ++c) o[c] = static_cast<std::uint8_t>((h[c] * a + o[c] * (255 - a) + 127) / 255);
                o[3] = static_cast<std::uint8_t>(std::max<unsigned>(o[3], a));
            }
        }
    }
    return true;
}

}
