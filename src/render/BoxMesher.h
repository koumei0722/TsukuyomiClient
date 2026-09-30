#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace tsukuyomi::boxmesh {

inline constexpr std::uint8_t kMaxColor = 4;

struct Cell {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
    std::uint8_t color = 0;
    std::uint8_t covered = 0;
};

enum Dir : std::uint8_t {
    kDown = 0,
    kUp = 1,
    kNorth = 2,
    kSouth = 3,
    kWest = 4,
    kEast = 5,
};

struct Quad {
    std::uint8_t dir = 0;
    std::uint8_t color = 0;
    std::int32_t plane = 0;
    std::int32_t u0 = 0;
    std::int32_t v0 = 0;
    std::int32_t u1 = 0;
    std::int32_t v1 = 0;
};

std::vector<Quad> build(const std::vector<Cell>& cells, bool cullShared, bool merge = true);

struct Edge {
    std::uint8_t axis = 0;
    std::uint8_t color = 0;
    std::int32_t a = 0;
    std::int32_t b = 0;
    std::int32_t t0 = 0;
    std::int32_t t1 = 0;
};

enum class EdgeStyle : std::uint8_t {
    Shape,
    BlockSurface,
    BlockAll,
};

std::vector<Edge> buildEdges(const std::vector<Cell>& cells, EdgeStyle style = EdgeStyle::Shape);

void edgeEnds(const Edge& edge, std::int32_t out[2][3]);

std::vector<Edge> edgesFromQuads(const std::vector<Quad>& quads);

void corners(const Quad& quad, std::int32_t out[4][3]);

double distanceSq(const Quad& quad, const double eye[3]);
double distanceSq(const Edge& edge, const double eye[3]);

float keepNearest(std::vector<Quad>& quads, const double eye[3], std::size_t budget);
float keepNearest(std::vector<Edge>& edges, const double eye[3], std::size_t budget);

float copyNearest(const std::vector<Quad>& from, const double eye[3], std::size_t budget,
                  std::vector<Quad>& to);
float copyNearest(const std::vector<Edge>& from, const double eye[3], std::size_t budget,
                  std::vector<Edge>& to);

struct RadiusCut {
    std::size_t full = 0;
    std::size_t partial = 0;
};
RadiusCut radiusCut(const std::vector<std::size_t>& histogram, std::size_t limit);

inline constexpr std::uint32_t kEdgeItem = 0x80000000U;
float paintOrder(std::vector<Quad>& quads, std::vector<Edge>& edges, const double eye[3],
                 std::size_t budget, double edgeReach, std::vector<std::uint32_t>& order);

bool ribbonCorners(const double p0[3], const double p1[3], const double* forward, double forwardW,
                   double scale, double nearW, double out[4][3]);

bool ribbonCornersAround(const double p0[3], const double p1[3], double scale, double out[4][3]);
void ribbonPieces(const Edge& edge, const double eye[3], std::vector<std::int32_t>& out);

void faceCell(const Quad& quad, std::int32_t out[3]);
double manhattanKey(const std::int32_t low[3], std::int32_t size, const double eye[3]);
void sortUnitFacesNearFirst(std::vector<Quad>& quads, const double eye[3]);

inline constexpr std::int32_t kRegionSize = 16;

struct RegionKey {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
    bool operator==(const RegionKey& other) const
    {
        return x == other.x && y == other.y && z == other.z;
    }
};
struct RegionKeyHash {
    std::size_t operator()(const RegionKey& key) const
    {
        std::uint64_t h = 1469598103934665603ULL;
        for (const std::int32_t v : {key.x, key.y, key.z}) {
            h = (h ^ static_cast<std::uint32_t>(v)) * 1099511628211ULL;
        }
        return static_cast<std::size_t>(h);
    }
};

RegionKey regionOf(std::int32_t x, std::int32_t y, std::int32_t z);
void regionOrigin(const RegionKey& key, std::int32_t out[3]);
bool nearRegion(const Cell& cell, const std::int32_t origin[3]);

struct RegionGeometry {
    std::vector<Quad> quads;
    std::vector<Edge> edges;
};
inline constexpr std::uint8_t kEdgePriority[kMaxColor + 1] = {0, 4, 1, 2, 3};
RegionGeometry buildRegion(const std::vector<Cell>& own, const std::vector<Cell>& margin,
                           const std::int32_t origin[3],
                           const std::function<bool(const RegionKey&)>& exists, bool xray);
bool sameRegionGeometry(const RegionGeometry& a, const RegionGeometry& b);

struct RegionEntry {
    std::uint64_t hash = 0;
    std::uint64_t generation = 0;
    std::shared_ptr<const RegionGeometry> geometry;
};
class RegionSet {
public:
    std::size_t update(const std::vector<Cell>& cells, bool xray);
    const std::unordered_map<RegionKey, RegionEntry, RegionKeyHash>& entries() const
    {
        return entries_;
    }

private:
    std::unordered_map<RegionKey, RegionEntry, RegionKeyHash> entries_;
    bool xray_ = false;
};

}
