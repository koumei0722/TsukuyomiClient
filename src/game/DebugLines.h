#pragma once

#include "render/DebugLine.h"

#include <cstdint>
#include <span>
#include <vector>

namespace tsukuyomi::debuglines {

inline constexpr float kStrokeWidth = 2.5F;
inline constexpr float kThickWidth = 4.0F;
inline constexpr float kThinWidth = 1.0F;

struct HitBox {
    std::uint32_t id;
    float min[3];
    float max[3];
    bool hasEye;
    bool living;
    float eye[3];
    float look[3];
};

void boxLines(const float min[3], const float max[3], const float rgba[4],
              std::vector<worldmesh::DebugLine>& out, float width = kThinWidth, bool onTop = false);
void chunkBorderLines(int sectionMinX, int sectionMinY, int sectionMinZ,
                      int minY, int maxYExclusive, std::vector<worldmesh::DebugLine>& out);
void lerpBox(const HitBox& from, const HitBox& to, std::uint64_t elapsedMs, HitBox& out);
void arrowLines(const double start[3], const double end[3], const float rgba[4], float width,
                std::vector<worldmesh::DebugLine>& out);
void hitBoxLines(const HitBox& box, std::vector<worldmesh::DebugLine>& out);
void viewVector(float pitchDegrees, float yawDegrees, float out[3]);

class HitBoxTrack {
public:
    std::vector<HitBox> update(std::span<const HitBox> boxes, std::uint64_t nowMs);
    void clear() { m_entries.clear(); }

private:
    struct Entry {
        HitBox from;
        HitBox current;
        std::uint64_t changedAt;
        std::uint64_t seenAt;
    };
    std::vector<Entry> m_entries;
};

}
