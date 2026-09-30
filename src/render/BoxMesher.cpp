#include "render/BoxMesher.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace tsukuyomi::boxmesh {
namespace {

struct Key3 {
    std::int32_t a = 0;
    std::int32_t b = 0;
    std::int32_t c = 0;
    bool operator==(const Key3& other) const noexcept
    {
        return a == other.a && b == other.b && c == other.c;
    }
};

struct Key3Hash {
    std::size_t operator()(const Key3& key) const noexcept
    {
        std::uint64_t h = static_cast<std::uint32_t>(key.a);
        h = h * 0x9E3779B97F4A7C15ULL ^ static_cast<std::uint32_t>(key.b);
        h = h * 0x9E3779B97F4A7C15ULL ^ static_cast<std::uint32_t>(key.c);
        return static_cast<std::size_t>(h ^ (h >> 29));
    }
};

struct Axis {
    std::int32_t dx;
    std::int32_t dy;
    std::int32_t dz;
};
constexpr Axis kNeighbor[6] = {
    {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0},
};

constexpr std::int32_t kColorRadix = 8;

void planeOf(std::uint8_t dir, const Cell& cell, std::int32_t& plane, std::int32_t& u,
             std::int32_t& v)
{
    switch (dir) {
    case kDown:
    case kUp:
        plane = cell.y + (dir == kUp ? 1 : 0);
        u = cell.x;
        v = cell.z;
        break;
    case kNorth:
    case kSouth:
        plane = cell.z + (dir == kSouth ? 1 : 0);
        u = cell.x;
        v = cell.y;
        break;
    default:
        plane = cell.x + (dir == kEast ? 1 : 0);
        u = cell.z;
        v = cell.y;
        break;
    }
}

}

std::vector<Quad> build(const std::vector<Cell>& cells, bool cullShared, bool merge)
{
    std::unordered_map<Key3, std::uint8_t, Key3Hash> occupied;
    occupied.reserve(cells.size() * 2 + 1);
    std::vector<Cell> unique;
    unique.reserve(cells.size());
    for (const Cell& cell : cells) {
        if (cell.color == 0 || cell.color > kMaxColor) {
            continue;
        }
        if (occupied.emplace(Key3{cell.x, cell.y, cell.z}, cell.color).second) {
            unique.push_back(cell);
        }
    }

    std::unordered_map<Key3, std::vector<std::pair<std::int32_t, std::int32_t>>, Key3Hash>
        groups;
    for (const Cell& cell : unique) {
        for (std::uint8_t dir = 0; dir < 6; ++dir) {
            if (cullShared) {
                if ((cell.covered & (1u << dir)) != 0) {
                    continue;
                }
                const Axis& n = kNeighbor[dir];
                if (occupied.count(Key3{cell.x + n.dx, cell.y + n.dy, cell.z + n.dz}) != 0) {
                    continue;
                }
            }
            std::int32_t plane = 0;
            std::int32_t u = 0;
            std::int32_t v = 0;
            planeOf(dir, cell, plane, u, v);
            groups[Key3{static_cast<std::int32_t>(dir) * kColorRadix + cell.color, plane, 0}]
                .emplace_back(u, v);
        }
    }

    std::vector<Quad> out;
    std::unordered_set<Key3, Key3Hash> left;
    for (auto& [key, faces] : groups) {
        std::sort(faces.begin(), faces.end(), [](const auto& p, const auto& q) {
            return p.second != q.second ? p.second < q.second : p.first < q.first;
        });
        left.clear();
        if (merge) {
            left.reserve(faces.size() * 2 + 1);
            for (const auto& [u, v] : faces) {
                left.insert(Key3{u, v, 0});
            }
        }
        const auto dir = static_cast<std::uint8_t>(key.a / kColorRadix);
        const auto color = static_cast<std::uint8_t>(key.a % kColorRadix);
        if (!merge) {
            for (const auto& [u, v] : faces) {
                Quad quad;
                quad.dir = dir;
                quad.color = color;
                quad.plane = key.b;
                quad.u0 = u;
                quad.v0 = v;
                quad.u1 = u + 1;
                quad.v1 = v + 1;
                out.push_back(quad);
            }
            continue;
        }
        for (const auto& [u, v] : faces) {
            if (left.count(Key3{u, v, 0}) == 0) {
                continue;
            }
            std::int32_t width = 1;
            while (left.count(Key3{u + width, v, 0}) != 0) {
                ++width;
            }
            std::int32_t height = 1;
            for (;;) {
                bool full = true;
                for (std::int32_t k = 0; k < width && full; ++k) {
                    full = left.count(Key3{u + k, v + height, 0}) != 0;
                }
                if (!full) {
                    break;
                }
                ++height;
            }
            for (std::int32_t j = 0; j < height; ++j) {
                for (std::int32_t k = 0; k < width; ++k) {
                    left.erase(Key3{u + k, v + j, 0});
                }
            }
            Quad quad;
            quad.dir = dir;
            quad.color = color;
            quad.plane = key.b;
            quad.u0 = u;
            quad.v0 = v;
            quad.u1 = u + width;
            quad.v1 = v + height;
            out.push_back(quad);
        }
    }
    std::sort(out.begin(), out.end(), [](const Quad& p, const Quad& q) {
        if (p.color != q.color) {
            return p.color < q.color;
        }
        if (p.dir != q.dir) {
            return p.dir < q.dir;
        }
        if (p.plane != q.plane) {
            return p.plane < q.plane;
        }
        if (p.v0 != q.v0) {
            return p.v0 < q.v0;
        }
        return p.u0 < q.u0;
    });
    return out;
}

void corners(const Quad& quad, std::int32_t out[4][3])
{
    std::int32_t uv[4][2] = {};
    const bool uFirst = (quad.dir == kDown || quad.dir == kSouth || quad.dir == kWest);
    if (uFirst) {
        const std::int32_t pts[4][2] = {
            {quad.u0, quad.v0}, {quad.u1, quad.v0}, {quad.u1, quad.v1}, {quad.u0, quad.v1}};
        std::copy(&pts[0][0], &pts[0][0] + 8, &uv[0][0]);
    } else {
        const std::int32_t pts[4][2] = {
            {quad.u0, quad.v0}, {quad.u0, quad.v1}, {quad.u1, quad.v1}, {quad.u1, quad.v0}};
        std::copy(&pts[0][0], &pts[0][0] + 8, &uv[0][0]);
    }
    for (int i = 0; i < 4; ++i) {
        const std::int32_t u = uv[i][0];
        const std::int32_t v = uv[i][1];
        switch (quad.dir) {
        case kDown:
        case kUp:
            out[i][0] = u;
            out[i][1] = quad.plane;
            out[i][2] = v;
            break;
        case kNorth:
        case kSouth:
            out[i][0] = u;
            out[i][1] = v;
            out[i][2] = quad.plane;
            break;
        default:
            out[i][0] = quad.plane;
            out[i][1] = v;
            out[i][2] = u;
            break;
        }
    }
}

namespace {

Key3 cellOnAxis(std::uint8_t axis, std::int32_t t, std::int32_t p, std::int32_t q)
{
    switch (axis) {
    case 0:
        return Key3{t, p, q};
    case 1:
        return Key3{p, t, q};
    default:
        return Key3{p, q, t};
    }
}

void splitOnAxis(std::uint8_t axis, const Cell& cell, std::int32_t& t, std::int32_t& p,
                 std::int32_t& q)
{
    switch (axis) {
    case 0:
        t = cell.x;
        p = cell.y;
        q = cell.z;
        break;
    case 1:
        t = cell.y;
        p = cell.x;
        q = cell.z;
        break;
    default:
        t = cell.z;
        p = cell.x;
        q = cell.y;
        break;
    }
}

struct Key4 {
    std::int32_t a = 0;
    std::int32_t b = 0;
    std::int32_t c = 0;
    std::int32_t d = 0;
    bool operator==(const Key4& other) const noexcept
    {
        return a == other.a && b == other.b && c == other.c && d == other.d;
    }
};

struct Key4Hash {
    std::size_t operator()(const Key4& key) const noexcept
    {
        std::uint64_t h = static_cast<std::uint32_t>(key.a);
        h = h * 0x9E3779B97F4A7C15ULL ^ static_cast<std::uint32_t>(key.b);
        h = h * 0x9E3779B97F4A7C15ULL ^ static_cast<std::uint32_t>(key.c);
        h = h * 0x9E3779B97F4A7C15ULL ^ static_cast<std::uint32_t>(key.d);
        return static_cast<std::size_t>(h ^ (h >> 29));
    }
};

}

std::vector<Edge> buildEdges(const std::vector<Cell>& cells, EdgeStyle style)
{
    std::unordered_map<Key3, std::uint8_t, Key3Hash> occupied;
    occupied.reserve(cells.size() * 2 + 1);
    std::vector<Cell> unique;
    unique.reserve(cells.size());
    for (const Cell& cell : cells) {
        if (cell.color == 0 || cell.color > kMaxColor) {
            continue;
        }
        if (occupied.emplace(Key3{cell.x, cell.y, cell.z}, cell.color).second) {
            unique.push_back(cell);
        }
    }
    const auto same = [&occupied](const Key3& at, std::uint8_t color) {
        const auto found = occupied.find(at);
        return found != occupied.end() && found->second == color;
    };
    const auto any = [&occupied](const Key3& at) { return occupied.find(at) != occupied.end(); };

    std::unordered_set<Key4, Key4Hash> seen;
    seen.reserve(unique.size() * 6 + 1);
    std::unordered_map<Key3, std::vector<std::int32_t>, Key3Hash> runs;
    for (const Cell& cell : unique) {
        for (std::uint8_t axis = 0; axis < 3; ++axis) {
            std::int32_t t = 0;
            std::int32_t p = 0;
            std::int32_t q = 0;
            splitOnAxis(axis, cell, t, p, q);
            const std::int32_t tag = static_cast<std::int32_t>(axis) * kColorRadix + cell.color;
            for (std::int32_t da = 0; da < 2; ++da) {
                for (std::int32_t db = 0; db < 2; ++db) {
                    const std::int32_t a = p + da;
                    const std::int32_t b = q + db;
                    if (!seen.insert(Key4{tag, a, b, t}).second) {
                        continue;
                    }
                    const bool c00 = same(cellOnAxis(axis, t, a - 1, b - 1), cell.color);
                    const bool c10 = same(cellOnAxis(axis, t, a, b - 1), cell.color);
                    const bool c01 = same(cellOnAxis(axis, t, a - 1, b), cell.color);
                    const bool c11 = same(cellOnAxis(axis, t, a, b), cell.color);
                    const int count = static_cast<int>(c00) + static_cast<int>(c10)
                                      + static_cast<int>(c01) + static_cast<int>(c11);
                    bool draw = false;
                    switch (style) {
                    case EdgeStyle::Shape: {
                        const bool diagonal = count == 2 && ((c00 && c11) || (c10 && c01));
                        draw = count == 1 || count == 3 || diagonal;
                        break;
                    }
                    case EdgeStyle::BlockSurface:
                        draw = !(any(cellOnAxis(axis, t, a - 1, b - 1)) && any(cellOnAxis(axis, t, a, b - 1))
                                 && any(cellOnAxis(axis, t, a - 1, b)) && any(cellOnAxis(axis, t, a, b)));
                        break;
                    case EdgeStyle::BlockAll:
                        draw = true;
                        break;
                    }
                    if (draw) {
                        runs[Key3{tag, a, b}].push_back(t);
                    }
                }
            }
        }
    }

    std::vector<Edge> out;
    for (auto& [key, values] : runs) {
        std::sort(values.begin(), values.end());
        std::size_t at = 0;
        while (at < values.size()) {
            std::size_t end = at + 1;
            while (end < values.size() && values[end] == values[end - 1] + 1) {
                ++end;
            }
            Edge edge;
            edge.axis = static_cast<std::uint8_t>(key.a / kColorRadix);
            edge.color = static_cast<std::uint8_t>(key.a % kColorRadix);
            edge.a = key.b;
            edge.b = key.c;
            edge.t0 = values[at];
            edge.t1 = values[end - 1] + 1;
            out.push_back(edge);
            at = end;
        }
    }
    std::sort(out.begin(), out.end(), [](const Edge& p, const Edge& q) {
        if (p.color != q.color) {
            return p.color < q.color;
        }
        if (p.axis != q.axis) {
            return p.axis < q.axis;
        }
        if (p.a != q.a) {
            return p.a < q.a;
        }
        if (p.b != q.b) {
            return p.b < q.b;
        }
        return p.t0 < q.t0;
    });
    return out;
}

void edgeEnds(const Edge& edge, std::int32_t out[2][3])
{
    const std::int32_t ts[2] = {edge.t0, edge.t1};
    for (int i = 0; i < 2; ++i) {
        const Key3 at = cellOnAxis(edge.axis, ts[i], edge.a, edge.b);
        out[i][0] = at.a;
        out[i][1] = at.b;
        out[i][2] = at.c;
    }
}

std::vector<Edge> edgesFromQuads(const std::vector<Quad>& quads)
{
    std::vector<Edge> out;
    out.reserve(quads.size() * 4);
    const auto push = [&out](std::uint8_t axis, std::uint8_t color, std::int32_t a,
                             std::int32_t b, std::int32_t t0, std::int32_t t1) {
        if (t1 <= t0) {
            return;
        }
        Edge edge;
        edge.axis = axis;
        edge.color = color;
        edge.a = a;
        edge.b = b;
        edge.t0 = t0;
        edge.t1 = t1;
        out.push_back(edge);
    };
    for (const Quad& quad : quads) {
        if (quad.color == 0 || quad.color > kMaxColor) {
            continue;
        }
        switch (quad.dir) {
        case kDown:
        case kUp:
            push(0, quad.color, quad.plane, quad.v0, quad.u0, quad.u1);
            push(0, quad.color, quad.plane, quad.v1, quad.u0, quad.u1);
            push(2, quad.color, quad.u0, quad.plane, quad.v0, quad.v1);
            push(2, quad.color, quad.u1, quad.plane, quad.v0, quad.v1);
            break;
        case kNorth:
        case kSouth:
            push(0, quad.color, quad.v0, quad.plane, quad.u0, quad.u1);
            push(0, quad.color, quad.v1, quad.plane, quad.u0, quad.u1);
            push(1, quad.color, quad.u0, quad.plane, quad.v0, quad.v1);
            push(1, quad.color, quad.u1, quad.plane, quad.v0, quad.v1);
            break;
        default:
            push(2, quad.color, quad.plane, quad.v0, quad.u0, quad.u1);
            push(2, quad.color, quad.plane, quad.v1, quad.u0, quad.u1);
            push(1, quad.color, quad.plane, quad.u0, quad.v0, quad.v1);
            push(1, quad.color, quad.plane, quad.u1, quad.v0, quad.v1);
            break;
        }
    }
    return out;
}

namespace {

double gap(double at, std::int32_t lo, std::int32_t hi)
{
    if (at < lo) {
        return static_cast<double>(lo) - at;
    }
    if (at > hi) {
        return at - static_cast<double>(hi);
    }
    return 0.0;
}

double boxDistanceSq(const double eye[3], const std::int32_t lo[3], const std::int32_t hi[3])
{
    double sum = 0.0;
    for (int k = 0; k < 3; ++k) {
        const double d = gap(eye[k], lo[k], hi[k]);
        sum += d * d;
    }
    return sum;
}

template <class T>
float copyNearestOf(const std::vector<T>& from, const double eye[3], std::size_t budget,
                    std::vector<T>& to)
{
    if (from.size() <= budget) {
        to = from;
        return -1.0F;
    }
    to.clear();
    if (budget == 0) {
        return 0.0F;
    }
    std::vector<std::pair<double, std::uint32_t>> order;
    order.reserve(from.size());
    for (std::size_t i = 0; i < from.size(); ++i) {
        order.emplace_back(distanceSq(from[i], eye), static_cast<std::uint32_t>(i));
    }
    std::nth_element(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(budget),
                     order.end());
    order.resize(budget);
    std::sort(order.begin(), order.end(),
              [&from](const std::pair<double, std::uint32_t>& p,
                      const std::pair<double, std::uint32_t>& q) {
                  const std::uint8_t pc = from[p.second].color;
                  const std::uint8_t qc = from[q.second].color;
                  if (pc != qc) {
                      return pc < qc;
                  }
                  return p < q;
              });
    double farthest = 0.0;
    to.reserve(budget);
    for (const auto& [d2, at] : order) {
        to.push_back(from[at]);
        farthest = (std::max)(farthest, d2);
    }
    return static_cast<float>(std::sqrt(farthest));
}

template <class T>
float keepNearestOf(std::vector<T>& items, const double eye[3], std::size_t budget)
{
    if (items.size() <= budget) {
        return -1.0F;
    }
    std::vector<T> kept;
    const float radius = copyNearestOf(items, eye, budget, kept);
    items.swap(kept);
    return radius;
}

}

double distanceSq(const Quad& quad, const double eye[3])
{
    std::int32_t lo[3] = {};
    std::int32_t hi[3] = {};
    const auto span = [&lo, &hi](int axis, std::int32_t from, std::int32_t to) {
        lo[axis] = from;
        hi[axis] = to;
    };
    switch (quad.dir) {
    case kDown:
    case kUp:
        span(0, quad.u0, quad.u1);
        span(1, quad.plane, quad.plane);
        span(2, quad.v0, quad.v1);
        break;
    case kNorth:
    case kSouth:
        span(0, quad.u0, quad.u1);
        span(1, quad.v0, quad.v1);
        span(2, quad.plane, quad.plane);
        break;
    default:
        span(0, quad.plane, quad.plane);
        span(1, quad.v0, quad.v1);
        span(2, quad.u0, quad.u1);
        break;
    }
    return boxDistanceSq(eye, lo, hi);
}

double distanceSq(const Edge& edge, const double eye[3])
{
    std::int32_t ends[2][3] = {};
    edgeEnds(edge, ends);
    std::int32_t lo[3] = {};
    std::int32_t hi[3] = {};
    for (int k = 0; k < 3; ++k) {
        lo[k] = (std::min)(ends[0][k], ends[1][k]);
        hi[k] = (std::max)(ends[0][k], ends[1][k]);
    }
    return boxDistanceSq(eye, lo, hi);
}

float keepNearest(std::vector<Quad>& quads, const double eye[3], std::size_t budget)
{
    return keepNearestOf(quads, eye, budget);
}

float keepNearest(std::vector<Edge>& edges, const double eye[3], std::size_t budget)
{
    return keepNearestOf(edges, eye, budget);
}

float copyNearest(const std::vector<Quad>& from, const double eye[3], std::size_t budget,
                  std::vector<Quad>& to)
{
    return copyNearestOf(from, eye, budget, to);
}

float copyNearest(const std::vector<Edge>& from, const double eye[3], std::size_t budget,
                  std::vector<Edge>& to)
{
    return copyNearestOf(from, eye, budget, to);
}

RadiusCut radiusCut(const std::vector<std::size_t>& histogram, std::size_t limit)
{
    RadiusCut cut;
    std::size_t taken = 0;
    for (std::size_t r = 0; r < histogram.size(); ++r) {
        if (histogram[r] > limit - taken) {
            cut.full = r;
            cut.partial = limit - taken;
            return cut;
        }
        taken += histogram[r];
    }
    cut.full = histogram.size();
    cut.partial = 0;
    return cut;
}

float paintOrder(std::vector<Quad>& quads, std::vector<Edge>& edges, const double eye[3],
                 std::size_t budget, double edgeReach, std::vector<std::uint32_t>& order)
{
    order.clear();
    struct Pick {
        double key;
        std::uint32_t item;
    };
    const bool withEdges = edgeReach > 0.0;
    std::vector<Pick> picks;
    picks.reserve(quads.size() + (withEdges ? edges.size() : 0));
    for (std::size_t i = 0; i < quads.size(); ++i) {
        picks.push_back(Pick{distanceSq(quads[i], eye), static_cast<std::uint32_t>(i)});
    }
    if (withEdges) {
        const double scale = 1.0 / (edgeReach * edgeReach);
        for (std::size_t i = 0; i < edges.size(); ++i) {
            picks.push_back(
                Pick{distanceSq(edges[i], eye) * scale, kEdgeItem | static_cast<std::uint32_t>(i)});
        }
    }
    float radius = -1.0F;
    if (picks.size() > budget) {
        const auto nearer = [](const Pick& a, const Pick& b) {
            return a.key != b.key ? a.key < b.key : a.item < b.item;
        };
        std::nth_element(picks.begin(), picks.begin() + static_cast<std::ptrdiff_t>(budget),
                         picks.end(), nearer);
        bool droppedFace = false;
        for (std::size_t i = budget; i < picks.size() && !droppedFace; ++i) {
            droppedFace = (picks[i].item & kEdgeItem) == 0;
        }
        picks.resize(budget);
        if (droppedFace) {
            double farthest = 0.0;
            for (const Pick& pick : picks) {
                if ((pick.item & kEdgeItem) == 0) {
                    farthest = (std::max)(farthest, pick.key);
                }
            }
            radius = static_cast<float>(std::sqrt(farthest));
        }
    }
    std::vector<char> keepQuad(quads.size(), 0);
    std::vector<char> keepEdge(edges.size(), 0);
    for (const Pick& pick : picks) {
        if ((pick.item & kEdgeItem) != 0) {
            keepEdge[pick.item & ~kEdgeItem] = 1;
        } else {
            keepQuad[pick.item] = 1;
        }
    }
    std::size_t kept = 0;
    for (std::size_t i = 0; i < quads.size(); ++i) {
        if (keepQuad[i] != 0) {
            quads[kept++] = quads[i];
        }
    }
    quads.resize(kept);
    kept = 0;
    for (std::size_t i = 0; i < edges.size(); ++i) {
        if (keepEdge[i] != 0) {
            edges[kept++] = edges[i];
        }
    }
    edges.resize(kept);
    std::vector<std::pair<double, std::uint32_t>> byCenter;
    byCenter.reserve(picks.size());
    for (std::size_t i = 0; i < quads.size(); ++i) {
        std::int32_t c[4][3] = {};
        corners(quads[i], c);
        double d2 = 0.0;
        for (int k = 0; k < 3; ++k) {
            const double mid =
                (static_cast<double>(c[0][k]) + static_cast<double>(c[2][k])) * 0.5 - eye[k];
            d2 += mid * mid;
        }
        byCenter.emplace_back(d2, static_cast<std::uint32_t>(i));
    }
    for (std::size_t i = 0; i < edges.size(); ++i) {
        std::int32_t ends[2][3] = {};
        edgeEnds(edges[i], ends);
        double d2 = 0.0;
        for (int k = 0; k < 3; ++k) {
            const double mid =
                (static_cast<double>(ends[0][k]) + static_cast<double>(ends[1][k])) * 0.5 - eye[k];
            d2 += mid * mid;
        }
        byCenter.emplace_back(d2, kEdgeItem | static_cast<std::uint32_t>(i));
    }
    std::sort(byCenter.begin(), byCenter.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first > b.first : a.second < b.second;
    });
    order.reserve(byCenter.size());
    for (const auto& [d2, item] : byCenter) {
        order.push_back(item);
    }
    return radius;
}

bool ribbonCorners(const double p0[3], const double p1[3], const double* forward, double forwardW,
                   double scale, double nearW, double out[4][3])
{
    if (forward == nullptr) {
        return false;
    }
    double p[2][3] = {{p0[0], p0[1], p0[2]}, {p1[0], p1[1], p1[2]}};
    double depth[2] = {};
    for (int i = 0; i < 2; ++i) {
        depth[i] = p[i][0] * forward[0] + p[i][1] * forward[1] + p[i][2] * forward[2] + forwardW;
    }
    if (depth[0] < nearW && depth[1] < nearW) {
        return false;
    }
    if (depth[0] < nearW || depth[1] < nearW) {
        const int back = depth[0] < nearW ? 0 : 1;
        const int front = 1 - back;
        const double t = (nearW - depth[back]) / (depth[front] - depth[back]);
        for (int k = 0; k < 3; ++k) {
            p[back][k] += (p[front][k] - p[back][k]) * t;
        }
        depth[back] = nearW;
    }
    const double d[3] = {p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]};
    const double side[3] = {d[1] * p[0][2] - d[2] * p[0][1], d[2] * p[0][0] - d[0] * p[0][2],
                            d[0] * p[0][1] - d[1] * p[0][0]};
    const double length = std::sqrt(side[0] * side[0] + side[1] * side[1] + side[2] * side[2]);
    if (!(length > 1e-9)) {
        return false;
    }
    for (int i = 0; i < 2; ++i) {
        const double half = depth[i] * scale * 0.5 / length;
        const double sign = i == 0 ? -1.0 : 1.0;
        for (int k = 0; k < 3; ++k) {
            out[i * 2][k] = p[i][k] + sign * side[k] * half;
            out[i * 2 + 1][k] = p[i][k] - sign * side[k] * half;
        }
    }
    return true;
}

bool ribbonCornersAround(const double p0[3], const double p1[3], double scale, double out[4][3])
{
    const double d[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
    const double side[3] = {d[1] * p0[2] - d[2] * p0[1], d[2] * p0[0] - d[0] * p0[2],
                            d[0] * p0[1] - d[1] * p0[0]};
    const double length = std::sqrt(side[0] * side[0] + side[1] * side[1] + side[2] * side[2]);
    if (!(length > 1e-9)) {
        return false;
    }
    const double* const p[2] = {p0, p1};
    for (int i = 0; i < 2; ++i) {
        const double distance = std::sqrt(p[i][0] * p[i][0] + p[i][1] * p[i][1] + p[i][2] * p[i][2]);
        const double half = distance * scale * 0.5 / length;
        const double sign = i == 0 ? -1.0 : 1.0;
        for (int k = 0; k < 3; ++k) {
            out[i * 2][k] = p[i][k] + sign * side[k] * half;
            out[i * 2 + 1][k] = p[i][k] - sign * side[k] * half;
        }
    }
    return true;
}

void ribbonPieces(const Edge& edge, const double eye[3], std::vector<std::int32_t>& out)
{
    out.clear();
    std::int32_t ends[2][3] = {};
    edgeEnds(edge, ends);
    const int along = edge.axis;
    double off2 = 0.0;
    for (int k = 0; k < 3; ++k) {
        if (k != along) {
            const double c = static_cast<double>(ends[0][k]) - eye[k];
            off2 += c * c;
        }
    }
    std::int32_t t = edge.t0;
    out.push_back(t);
    while (t < edge.t1) {
        const double g = static_cast<double>(t) - eye[along];
        const double here = std::sqrt(off2 + g * g);
        const auto step = (std::max)(static_cast<std::int32_t>(1), static_cast<std::int32_t>(here * 0.25));
        t = (std::min)(edge.t1, t + step);
        out.push_back(t);
    }
}

void faceCell(const Quad& quad, std::int32_t out[3])
{
    switch (quad.dir) {
    case kDown:
        out[0] = quad.u0;
        out[1] = quad.plane;
        out[2] = quad.v0;
        break;
    case kUp:
        out[0] = quad.u0;
        out[1] = quad.plane - 1;
        out[2] = quad.v0;
        break;
    case kNorth:
        out[0] = quad.u0;
        out[1] = quad.v0;
        out[2] = quad.plane;
        break;
    case kSouth:
        out[0] = quad.u0;
        out[1] = quad.v0;
        out[2] = quad.plane - 1;
        break;
    case kWest:
        out[0] = quad.plane;
        out[1] = quad.v0;
        out[2] = quad.u0;
        break;
    default:
        out[0] = quad.plane - 1;
        out[1] = quad.v0;
        out[2] = quad.u0;
        break;
    }
}

double manhattanKey(const std::int32_t low[3], std::int32_t size, const double eye[3])
{
    double sum = 0.0;
    for (int k = 0; k < 3; ++k) {
        sum += std::fabs(static_cast<double>(low[k]) + static_cast<double>(size) * 0.5 - eye[k]);
    }
    return sum;
}

void sortUnitFacesNearFirst(std::vector<Quad>& quads, const double eye[3])
{
    std::vector<std::pair<double, std::uint32_t>> keys;
    keys.reserve(quads.size());
    for (std::size_t i = 0; i < quads.size(); ++i) {
        std::int32_t cell[3] = {};
        faceCell(quads[i], cell);
        keys.emplace_back(manhattanKey(cell, 1, eye), static_cast<std::uint32_t>(i));
    }
    std::stable_sort(keys.begin(), keys.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<Quad> sorted;
    sorted.reserve(quads.size());
    for (const auto& [key, index] : keys) {
        sorted.push_back(quads[index]);
    }
    quads.swap(sorted);
}

RegionKey regionOf(std::int32_t x, std::int32_t y, std::int32_t z)
{
    const auto floorDiv = [](std::int32_t v) {
        return v >= 0 ? v / kRegionSize : -((-(v + 1)) / kRegionSize) - 1;
    };
    return RegionKey{floorDiv(x), floorDiv(y), floorDiv(z)};
}

void regionOrigin(const RegionKey& key, std::int32_t out[3])
{
    out[0] = key.x * kRegionSize;
    out[1] = key.y * kRegionSize;
    out[2] = key.z * kRegionSize;
}

bool nearRegion(const Cell& cell, const std::int32_t origin[3])
{
    const std::int32_t at[3] = {cell.x, cell.y, cell.z};
    for (int k = 0; k < 3; ++k) {
        if (at[k] < origin[k] - 1 || at[k] > origin[k] + kRegionSize) {
            return false;
        }
    }
    return true;
}

namespace {

std::int32_t regionIndex(std::int32_t v)
{
    return v >= 0 ? v / kRegionSize : -((-(v + 1)) / kRegionSize) - 1;
}

void dropCoveredEdges(const std::vector<Edge>& edges, std::vector<Edge>& out)
{
    if (edges.size() == 1) {
        out.push_back(edges.front());
        return;
    }
    std::vector<std::pair<std::int32_t, std::int32_t>> higher;
    for (const Edge& edge : edges) {
        higher.clear();
        for (const Edge& other : edges) {
            if (kEdgePriority[other.color] > kEdgePriority[edge.color] && other.t1 > edge.t0
                && other.t0 < edge.t1) {
                higher.emplace_back(other.t0, other.t1);
            }
        }
        std::sort(higher.begin(), higher.end());
        std::int32_t at = edge.t0;
        for (const auto& [h0, h1] : higher) {
            if (h0 > at) {
                Edge piece = edge;
                piece.t0 = at;
                piece.t1 = (std::min)(h0, edge.t1);
                out.push_back(piece);
            }
            at = (std::max)(at, h1);
            if (at >= edge.t1) {
                break;
            }
        }
        if (at < edge.t1) {
            Edge piece = edge;
            piece.t0 = at;
            out.push_back(piece);
        }
    }
}

}

RegionGeometry buildRegion(const std::vector<Cell>& own, const std::vector<Cell>& margin,
                           const std::int32_t origin[3],
                           const std::function<bool(const RegionKey&)>& exists, bool xray)
{
    RegionGeometry out;
    out.quads = xray ? build(own, false, true) : build(own, true, false);
    std::vector<Cell> all;
    all.reserve(own.size() + margin.size());
    all.insert(all.end(), own.begin(), own.end());
    all.insert(all.end(), margin.begin(), margin.end());
    const std::vector<Edge> edges = buildEdges(all, xray ? EdgeStyle::BlockAll : EdgeStyle::BlockSurface);
    const RegionKey self = regionOf(origin[0], origin[1], origin[2]);
    const std::int32_t selfIndex[3] = {self.x, self.y, self.z};
    const auto present = [&](const RegionKey& key) {
        return key == self || (exists && exists(key));
    };
    std::vector<Edge> mine;
    mine.reserve(edges.size());
    for (const Edge& edge : edges) {
        int pa = 1;
        int pb = 2;
        if (edge.axis == 1) {
            pa = 0;
            pb = 2;
        } else if (edge.axis == 2) {
            pa = 0;
            pb = 1;
        }
        const int along = edge.axis;
        if (edge.a < origin[pa] || edge.a > origin[pa] + kRegionSize || edge.b < origin[pb]
            || edge.b > origin[pb] + kRegionSize) {
            continue;
        }
        const std::int32_t t0 = (std::max)(edge.t0, origin[along]);
        const std::int32_t t1 = (std::min)(edge.t1, origin[along] + kRegionSize);
        if (t1 <= t0) {
            continue;
        }
        std::int32_t candA[2] = {regionIndex(edge.a), 0};
        std::int32_t candB[2] = {regionIndex(edge.b), 0};
        const int countA = edge.a % kRegionSize == 0 ? 2 : 1;
        const int countB = edge.b % kRegionSize == 0 ? 2 : 1;
        candA[1] = candA[0] - 1;
        candB[1] = candB[0] - 1;
        bool mineToDraw = false;
        bool decided = false;
        for (int ib = 0; ib < countB && !decided; ++ib) {
            for (int ia = 0; ia < countA && !decided; ++ia) {
                std::int32_t index[3] = {selfIndex[0], selfIndex[1], selfIndex[2]};
                index[pa] = candA[ia];
                index[pb] = candB[ib];
                const RegionKey candidate{index[0], index[1], index[2]};
                if (present(candidate)) {
                    decided = true;
                    mineToDraw = candidate == self;
                }
            }
        }
        if (!mineToDraw) {
            continue;
        }
        Edge clipped = edge;
        clipped.t0 = t0;
        clipped.t1 = t1;
        mine.push_back(clipped);
    }
    std::sort(mine.begin(), mine.end(), [](const Edge& p, const Edge& q) {
        if (p.axis != q.axis) {
            return p.axis < q.axis;
        }
        if (p.a != q.a) {
            return p.a < q.a;
        }
        if (p.b != q.b) {
            return p.b < q.b;
        }
        return p.t0 < q.t0;
    });
    out.edges.reserve(mine.size());
    std::vector<Edge> line;
    for (std::size_t i = 0; i < mine.size();) {
        std::size_t j = i;
        line.clear();
        while (j < mine.size() && mine[j].axis == mine[i].axis && mine[j].a == mine[i].a
               && mine[j].b == mine[i].b) {
            line.push_back(mine[j]);
            ++j;
        }
        dropCoveredEdges(line, out.edges);
        i = j;
    }
    std::sort(out.edges.begin(), out.edges.end(), [](const Edge& p, const Edge& q) {
        if (p.color != q.color) {
            return p.color < q.color;
        }
        if (p.axis != q.axis) {
            return p.axis < q.axis;
        }
        if (p.a != q.a) {
            return p.a < q.a;
        }
        if (p.b != q.b) {
            return p.b < q.b;
        }
        return p.t0 < q.t0;
    });
    return out;
}

bool sameRegionGeometry(const RegionGeometry& a, const RegionGeometry& b)
{
    if (a.quads.size() != b.quads.size() || a.edges.size() != b.edges.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.quads.size(); ++i) {
        const Quad& x = a.quads[i];
        const Quad& y = b.quads[i];
        if (x.dir != y.dir || x.color != y.color || x.plane != y.plane || x.u0 != y.u0 || x.v0 != y.v0
            || x.u1 != y.u1 || x.v1 != y.v1) {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.edges.size(); ++i) {
        const Edge& x = a.edges[i];
        const Edge& y = b.edges[i];
        if (x.axis != y.axis || x.color != y.color || x.a != y.a || x.b != y.b || x.t0 != y.t0
            || x.t1 != y.t1) {
            return false;
        }
    }
    return true;
}

namespace {

std::atomic<std::uint64_t> g_nextGeneration{1};

std::uint64_t hashCells(const std::vector<Cell>& cells)
{
    std::uint64_t h = 1469598103934665603ULL;
    const auto mix = [&h](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            h = (h ^ ((v >> (i * 8)) & 0xFFU)) * 1099511628211ULL;
        }
    };
    for (const Cell& c : cells) {
        mix(static_cast<std::uint32_t>(c.x));
        mix(static_cast<std::uint32_t>(c.y));
        mix(static_cast<std::uint32_t>(c.z));
        mix(static_cast<std::uint32_t>(c.color) | (static_cast<std::uint32_t>(c.covered) << 8));
    }
    return h ^ cells.size();
}

}

std::size_t RegionSet::update(const std::vector<Cell>& cells, bool xray)
{
    if (xray != xray_) {
        entries_.clear();
        xray_ = xray;
    }
    std::unordered_map<RegionKey, std::vector<Cell>, RegionKeyHash> own;
    for (const Cell& c : cells) {
        if (c.color == 0 || c.color > kMaxColor) {
            continue;
        }
        own[regionOf(c.x, c.y, c.z)].push_back(c);
    }
    std::unordered_map<RegionKey, std::uint64_t, RegionKeyHash> hashes;
    hashes.reserve(own.size());
    std::vector<RegionKey> touched;
    for (const auto& [key, list] : own) {
        const std::uint64_t h = hashCells(list);
        hashes[key] = h;
        const auto it = entries_.find(key);
        if (it == entries_.end() || it->second.hash != h) {
            touched.push_back(key);
        }
    }
    std::vector<RegionKey> removed;
    for (const auto& [key, entry] : entries_) {
        if (own.find(key) == own.end()) {
            removed.push_back(key);
            touched.push_back(key);
        }
    }
    for (const RegionKey& key : removed) {
        entries_.erase(key);
    }
    std::unordered_map<RegionKey, bool, RegionKeyHash> dirty;
    for (const RegionKey& key : touched) {
        for (int dx = -1; dx <= 1; ++dx) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dz = -1; dz <= 1; ++dz) {
                    const RegionKey around{key.x + dx, key.y + dy, key.z + dz};
                    if (own.find(around) != own.end()) {
                        dirty[around] = true;
                    }
                }
            }
        }
    }
    std::size_t renewed = 0;
    for (const auto& [key, unused] : dirty) {
        std::int32_t origin[3] = {};
        regionOrigin(key, origin);
        std::vector<Cell> margin;
        for (int dx = -1; dx <= 1; ++dx) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dz = -1; dz <= 1; ++dz) {
                    if (dx == 0 && dy == 0 && dz == 0) {
                        continue;
                    }
                    const auto it = own.find(RegionKey{key.x + dx, key.y + dy, key.z + dz});
                    if (it == own.end()) {
                        continue;
                    }
                    for (const Cell& c : it->second) {
                        if (nearRegion(c, origin)) {
                            margin.push_back(c);
                        }
                    }
                }
            }
        }
        RegionGeometry geometry = buildRegion(
            own[key], margin, origin, [&own](const RegionKey& k) { return own.find(k) != own.end(); }, xray);
        RegionEntry& entry = entries_[key];
        entry.hash = hashes[key];
        if (entry.geometry && sameRegionGeometry(*entry.geometry, geometry)) {
            continue;
        }
        entry.generation = g_nextGeneration.fetch_add(1, std::memory_order_relaxed);
        entry.geometry = std::make_shared<const RegionGeometry>(std::move(geometry));
        ++renewed;
    }
    return renewed;
}

}
