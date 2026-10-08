#include "game/DebugLines.h"

#include <algorithm>
#include <cmath>

namespace tsukuyomi::debuglines {
namespace {

void line(double ax, double ay, double az, double bx, double by, double bz,
          const float color[4], float width, std::vector<worldmesh::DebugLine>& out)
{
    worldmesh::DebugLine edge{{ax, ay, az}, {bx, by, bz},
                              {color[0], color[1], color[2], color[3]}};
    edge.width = width;
    out.push_back(edge);
}

void rotateFromX(const double d[3], const double v[3], double out[3])
{
    const double c = d[0];
    const double k[3] = {0.0, -d[2], d[1]};
    const double s = std::sqrt(k[1] * k[1] + k[2] * k[2]);
    if (s < 1e-9) {
        if (c > 0) {
            std::copy_n(v, 3, out);
        } else {
            out[0] = -v[0];
            out[1] = v[1];
            out[2] = -v[2];
        }
        return;
    }
    const double n[3] = {0.0, k[1] / s, k[2] / s};
    const double cross[3] = {n[1] * v[2] - n[2] * v[1], n[2] * v[0] - n[0] * v[2], n[0] * v[1] - n[1] * v[0]};
    const double dot = n[0] * v[0] + n[1] * v[1] + n[2] * v[2];
    for (int i = 0; i < 3; ++i) out[i] = v[i] * c + cross[i] * s + n[i] * dot * (1.0 - c);
}

}

void boxLines(const float min[3], const float max[3], const float rgba[4],
              std::vector<worldmesh::DebugLine>& out, float width, bool onTop)
{
    for (int axis = 0; axis < 3; ++axis) {
        const int u = (axis + 1) % 3;
        const int v = (axis + 2) % 3;
        for (int iu = 0; iu < 2; ++iu) {
            for (int iv = 0; iv < 2; ++iv) {
                worldmesh::DebugLine edge{};
                edge.a[axis] = min[axis];
                edge.b[axis] = max[axis];
                edge.a[u] = edge.b[u] = iu ? max[u] : min[u];
                edge.a[v] = edge.b[v] = iv ? max[v] : min[v];
                std::copy_n(rgba, 4, edge.rgba);
                edge.width = width;
                edge.onTop = onTop;
                out.push_back(edge);
            }
        }
    }
}

void chunkBorderLines(int x, int y, int z, int minY, int maxY,
                      std::vector<worldmesh::DebugLine>& out)
{
    if (maxY < minY) return;
    const float red[4] = {1, 0, 0, 0.5F};
    const float cyan[4] = {0, 155.0F / 255.0F, 155.0F / 255.0F, 1};
    const float yellow[4] = {1, 1, 0, 1};
    const float blue[4] = {0.25F, 0.25F, 1, 1};
    for (int dx = -16; dx <= 32; dx += 16)
        for (int dz = -16; dz <= 32; dz += 16)
            line(x + dx, minY, z + dz, x + dx, maxY, z + dz, red, kThickWidth, out);
    for (int k = 2; k < 16; k += 2) {
        const float* color = k % 4 == 0 ? cyan : yellow;
        for (int side = 0; side <= 16; side += 16) {
            line(x + k, minY, z + side, x + k, maxY, z + side, color, kThinWidth, out);
            line(x + side, minY, z + k, x + side, maxY, z + k, color, kThinWidth, out);
        }
    }
    for (int height = minY; height <= maxY; height += 2) {
        const float* color = height % 8 == 0 ? cyan : yellow;
        line(x, height, z, x, height, z + 16, color, kThinWidth, out);
        line(x, height, z + 16, x + 16, height, z + 16, color, kThinWidth, out);
        line(x + 16, height, z + 16, x + 16, height, z, color, kThinWidth, out);
        line(x + 16, height, z, x, height, z, color, kThinWidth, out);
    }
    for (int dx = 0; dx <= 16; dx += 16)
        for (int dz = 0; dz <= 16; dz += 16)
            line(x + dx, minY, z + dz, x + dx, maxY, z + dz, blue, kThickWidth, out);
    const float boxMin[3] = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
    const float boxMax[3] = {static_cast<float>(x + 16), static_cast<float>(y + 16),
                             static_cast<float>(z + 16)};
    boxLines(boxMin, boxMax, blue, out, kThinWidth, true);
    for (int height = minY; height <= maxY; height += 16) {
        line(x, height, z, x, height, z + 16, blue, kThickWidth, out);
        line(x, height, z + 16, x + 16, height, z + 16, blue, kThickWidth, out);
        line(x + 16, height, z + 16, x + 16, height, z, blue, kThickWidth, out);
        line(x + 16, height, z, x, height, z, blue, kThickWidth, out);
    }
}

void lerpBox(const HitBox& from, const HitBox& to, std::uint64_t elapsedMs, HitBox& out)
{
    const float t = static_cast<float>((std::min)(elapsedMs, std::uint64_t{50})) / 50.0F;
    out = to;
    for (int i = 0; i < 3; ++i) {
        out.min[i] = from.min[i] + (to.min[i] - from.min[i]) * t;
        out.max[i] = from.max[i] + (to.max[i] - from.max[i]) * t;
    }
}

void arrowLines(const double start[3], const double end[3], const float rgba[4], float width,
                std::vector<worldmesh::DebugLine>& out)
{
    line(start[0], start[1], start[2], end[0], end[1], end[2], rgba, width, out);
    const double diff[3] = {end[0] - start[0], end[1] - start[1], end[2] - start[2]};
    const double length = std::sqrt(diff[0] * diff[0] + diff[1] * diff[1] + diff[2] * diff[2]);
    if (!(length > 1e-9)) return;
    const double d[3] = {diff[0] / length, diff[1] / length, diff[2] / length};
    const double s = std::clamp(length * 0.1, 0.1, 1.0);
    const double heads[4][3] = {{-s, s, 0.0}, {-s, 0.0, s}, {-s, -s, 0.0}, {-s, 0.0, -s}};
    for (const auto& head : heads) {
        double r[3]{};
        rotateFromX(d, head, r);
        line(end[0] + r[0], end[1] + r[1], end[2] + r[2], end[0], end[1], end[2], rgba, width, out);
    }
}

void hitBoxLines(const HitBox& box, std::vector<worldmesh::DebugLine>& out)
{
    const float white[4] = {1, 1, 1, 1};
    boxLines(box.min, box.max, white, out, kStrokeWidth);
    if (!box.hasEye) return;
    const double eye[3] = {static_cast<double>(box.min[0]) + box.eye[0], static_cast<double>(box.min[1]) + box.eye[1],
                           static_cast<double>(box.min[2]) + box.eye[2]};
    if (box.living) {
        const float red[4] = {1, 0, 0, 1};
        const float planeMin[3] = {box.min[0], static_cast<float>(eye[1] - 0.01), box.min[2]};
        const float planeMax[3] = {box.max[0], static_cast<float>(eye[1] + 0.01), box.max[2]};
        boxLines(planeMin, planeMax, red, out, kStrokeWidth);
    }
    const float blue[4] = {0, 0, 1, 1};
    const double tip[3] = {eye[0] + box.look[0] * 2.0, eye[1] + box.look[1] * 2.0, eye[2] + box.look[2] * 2.0};
    arrowLines(eye, tip, blue, kStrokeWidth, out);
}

void viewVector(float pitchDegrees, float yawDegrees, float out[3])
{
    constexpr double kRadians = 3.14159265358979323846 / 180.0;
    const double f = pitchDegrees * kRadians;
    const double g = -yawDegrees * kRadians;
    const double h = std::cos(g);
    const double i = std::sin(g);
    const double j = std::cos(f);
    const double k = std::sin(f);
    out[0] = static_cast<float>(i * j);
    out[1] = static_cast<float>(-k);
    out[2] = static_cast<float>(h * j);
}

std::vector<HitBox> HitBoxTrack::update(std::span<const HitBox> boxes, std::uint64_t nowMs)
{
    std::vector<HitBox> result;
    result.reserve(boxes.size());
    for (const HitBox& box : boxes) {
        auto it = std::find_if(m_entries.begin(), m_entries.end(),
                               [&](const Entry& entry) { return entry.current.id == box.id; });
        if (it == m_entries.end()) {
            m_entries.push_back({box, box, nowMs, nowMs});
            result.push_back(box);
            continue;
        }
        HitBox shown{};
        lerpBox(it->from, it->current, nowMs - it->changedAt, shown);
        bool changed = false;
        for (int i = 0; i < 3; ++i)
            changed |= it->current.min[i] != box.min[i] || it->current.max[i] != box.max[i];
        if (changed) {
            it->from = shown;
            it->changedAt = nowMs;
        }
        it->current = box;
        it->seenAt = nowMs;
        lerpBox(it->from, it->current, nowMs - it->changedAt, shown);
        result.push_back(shown);
    }
    std::erase_if(m_entries, [nowMs](const Entry& entry) {
        return nowMs - entry.seenAt > 1000;
    });
    return result;
}

}
