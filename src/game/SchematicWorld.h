#pragma once

#include "game/SchematicSections.h"
#include <string>

namespace tsukuyomi::schematicworld {

struct VerifierPair {
    schematic::DiffKind kind = schematic::DiffKind::Missing;
    std::uint16_t wantEntry = 0xFFFF;
    const void* real = nullptr;
    std::size_t count = 0;
    std::int32_t nearest[3]{};
    double nearestDist2 = 0;
    std::string wantName, realName;
};
struct VerifierReport {
    std::uint64_t serial = 0;
    int scopeOwner = -1;
    std::size_t sectionsDone = 0, sectionsTotal = 0;
    std::size_t counts[schematic::kDiffKindCount]{};
    std::vector<VerifierPair> pairs;
    bool complete = false;
};
struct VerifierSelection {
    bool active = false;
    schematic::DiffKind kind = schematic::DiffKind::Missing;
    bool pair = false;
    std::uint16_t wantEntry = 0xFFFF;
    const void* real = nullptr;
    int scopeOwner = -1;
    bool operator==(const VerifierSelection&) const = default;
};
struct VerifierIgnored {
    schematic::DiffKind kind = schematic::DiffKind::Missing;
    std::string wantName, realName;
    bool operator==(const VerifierIgnored&) const = default;
};
void requestVerifier(int scopeOwner);
std::shared_ptr<const VerifierReport> verifierReport();
void setVerifierSelection(const VerifierSelection& selection);
void setVerifierIgnored(std::vector<VerifierIgnored> ignored);

void install(std::shared_ptr<const schematic::CellSet> cells,
             std::shared_ptr<const std::vector<const void*>> palette, const void* air);
void clear();
void onWorldChanged();
bool active();

struct FrameInput {
    unsigned long long now = 0;
    float eye[3]{};
    void* renderRegion = nullptr;
    int radiusChunks = 18;
    schematic::LayerFilter layer;
    bool ghostOverMismatch = true;
    unsigned compareBudgetUs = 0;
    unsigned learnPerFrame = 96;
};
void frame(const FrameInput& in);
struct SchematicHit {
    bool hit = false;
    std::int32_t pos[3]{};
    const void* want = nullptr;
    int owner = -1;
    double dist = 0;
    bool realEmpty = false;
};
SchematicHit traceSchematic(const float eye[3], const float dir[3], double reach);
const void* realBlockAt(const std::int32_t pos[3]);
bool emptyBlock(const void* block);
std::shared_ptr<const schematic::RenderSnapshot> renderSnapshot();
std::size_t takeBoxDirty(std::vector<schematic::SectionKey>& out);
std::uint64_t indexGeneration();
const std::vector<schematic::SectionKey>& inRangeKeys();
void missingByOwner(std::map<int, std::map<std::uint16_t, std::size_t>>& out);
void onSubChunkWrite(void* subChunk, unsigned layer, unsigned index);
struct BuildWindow { const schematic::RenderSnapshot* snap = nullptr; };
void enterBuildWindow(const BuildWindow* window);
void leaveBuildWindow();
bool answerForBuild(const std::int32_t pos[3], int layer, const void*& out);
void waitUntilIdle();

}
