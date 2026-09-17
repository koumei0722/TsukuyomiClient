#pragma once

#include <array>

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace tsukuyomi::blocks {

using Table = std::unordered_map<std::string, const void*>;

bool resolve(const std::vector<std::string>& wanted, Table& out);

struct WantedState {
    std::string name;
    bool isText = false;
    std::int64_t number = 0;
    std::string text;
};

std::string stateNamesOf(const void* block);

const void* stateVariant(const void* block, const std::vector<WantedState>& want,
                         std::size_t* missing = nullptr);

const void* rotatedBlock(const void* block, int quarters, bool* ok = nullptr);

const void* blockTable();
std::size_t lastEntryCount();

void setGhostRegion(std::int32_t x, std::int32_t y, std::int32_t z,
                    std::int32_t sx, std::int32_t sy, std::int32_t sz, float alpha);
void clearGhostRegion();
bool ghostOn();
bool ghostAt(std::int32_t x, std::int32_t y, std::int32_t z, float& alpha);

float ghostAlpha();

bool ghostBounds(std::int32_t* mn, std::int32_t* mx);

bool lastGhostBounds(std::int32_t* mn, std::int32_t* mx);

std::vector<std::array<std::int32_t, 3>> ghostBlockEntityCells();

inline constexpr int kGhostLayer = 3;

inline constexpr std::size_t kGhostCellLimit = 1u << 22;

void setGhostPalette(std::vector<const void*> blocks);

void setGhostCellBlock(std::int32_t x, std::int32_t y, std::int32_t z, std::size_t entry,
                       int layer = 0);

void setGhostCellOrient(std::int32_t x, std::int32_t y, std::int32_t z, float yawDeg,
                        int pitchQuarters);
bool ghostOrientAt(std::int32_t x, std::int32_t y, std::int32_t z, float& yawDeg,
                   int& pitchQuarters);

bool dropGhostCell(std::int32_t x, std::int32_t y, std::int32_t z);

void setGhostWantCell(std::int32_t x, std::int32_t y, std::int32_t z, std::size_t entry,
                      int layer = 0);
bool restoreGhostCellFromWant(std::int32_t x, std::int32_t y, std::int32_t z);

void setGhostWantAir(std::int32_t x, std::int32_t y, std::int32_t z);

enum class DiffColor : std::uint8_t {
    None = 0,
    Missing = 1,
    Wrong = 2,
    State = 3,
};

bool setDiffCell(std::int32_t x, std::int32_t y, std::int32_t z, DiffColor color);

std::size_t takeDiffCellChanges();

DiffColor diffCellAt(std::int32_t x, std::int32_t y, std::int32_t z);

void setLayerFilter(bool on, int axis, std::int32_t lo, std::int32_t hi);
bool layerShows(std::int32_t x, std::int32_t y, std::int32_t z);

enum class DiffKind : std::uint8_t {
    Match = 0,
    Missing = 1,
    Wrong = 2,
    State = 3,
    UnknownWorld = 4,
    UnknownWant = 5,
};

DiffKind diffKindOf(const void* real, const void* want, bool wantAir);

DiffKind diffKindOf(const void* real, const void* want, bool wantAir,
                    const void* realExtra, bool realExtraKnown, bool wantWater);

DiffColor colorOfDiffKind(DiffKind kind);

bool noteWorldBlockAt(std::int32_t x, std::int32_t y, std::int32_t z, const void* real,
                      const void* realExtra = nullptr, bool realExtraKnown = false);

void clearDiffCells();

struct DiffBox {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
    DiffColor color = DiffColor::None;
    bool hidden = false;
};

void setBoxBlock(const void* block);
const void* boxBlock();

void setMeshBoxes(bool on);
bool meshBoxesOn();

std::size_t collectDiffBoxes(std::vector<DiffBox>& out, std::size_t limit,
                             std::size_t* dropped);

std::size_t appendStateVariants(const void* block, std::vector<const void*>& out);

const void* legacyOfFast(const void* block);

bool ghostSubChunkOccupied(std::int32_t baseX, std::int32_t baseY, std::int32_t baseZ);

bool ghostBoxTouchesSubChunk(std::int32_t baseX, std::int32_t baseY, std::int32_t baseZ);

void subChunkOccupiedStats(std::size_t& calls, std::size_t& cells);

bool ghostInside(std::int32_t x, std::int32_t y, std::int32_t z);

const void* ghostBlockAt(std::int32_t x, std::int32_t y, std::int32_t z, int layer = 0);

const void* wantBlockAt(std::int32_t x, std::int32_t y, std::int32_t z);

void setGhostOverMismatch(bool on);
bool ghostOverMismatchOn();

void noteRealLayer(const void* block, int layer);

int realLayer(const void* block);

void noteGhostYaw(const void* block, float yawDeg);

float ghostYaw(const void* block);

bool hasBlockEntity(const void* block);

bool hasBlockEntityFast(const void* block);

bool calibrateItemIds();

bool blockItemIdAux(const void* block, std::int32_t& out);

bool maxStackSizeOf(const char* name, const void* block, int& out);

bool ghostCell(std::int32_t x, std::int32_t y, std::int32_t z);

void setAirBlock(const void* air);
const void* airBlock();

void waitUntilIdle();

void setSimThread(unsigned long id);
unsigned long simThread();

inline constexpr std::size_t kMeshThreadLimit = 16;

void noteMeshThread(unsigned long id);

bool isMeshThread(unsigned long id);

std::size_t meshThreadCount();

enum class GhostHook : int {
    Layer = 0,
    Tessellate = 1,
    Phantom = 2,
    Alpha = 3,
    Aim = 4,
    Filled = 5,
    Actor = 6,
    Box = 7,
    BoxMiss = 8,
    Count = 9,
};
void noteGhostHit(GhostHook which);

std::size_t ghostHitCount(GhostHook which);

void noteGhostDraw(std::int32_t x, std::int32_t y, std::int32_t z);

}
