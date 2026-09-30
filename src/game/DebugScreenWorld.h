#pragma once

#include <cstdint>
#include "game/DebugLines.h"

namespace tsukuyomi::debugworld {

int collectHitBoxes(debuglines::HitBox* out, int cap, float range, bool includeSelf);
void setActorFunctions(void* attachPos, void* viewVector, std::int32_t rotationField);
bool worldHeightAt(int x, int z, int& minY, int& maxYExclusive);

struct Wanted {
    bool target = false;
    bool chunks = false;
    bool speed = false;
    bool entities = false;
    bool entityTarget = false;
    bool biome = false;
    bool sections = false;
    bool blockTags = false;
    bool fluid = false;
    bool entityTags = false;
    bool serverChunks = false;
    bool localDifficulty = false;
    bool forcedChunks = false;
    bool localServer = false;
    bool climate = false;
};

inline constexpr int kTextBytes = 96;
inline constexpr int kMaxStates = 16;
inline constexpr int kMaxFluidStates = 5;
inline constexpr int kMaxTags = 7;

struct Sample {
    bool hasTime = false;
    std::int64_t dayTime = 0;
    bool hasDifficulty = false;
    int difficultyId = 0;
    bool hasSimulationDistance = false;
    int simulationDistance = 0;
    bool hasLight = false;
    int skyLight = 0;
    int blockLight = 0;
    bool hasDimension = false;
    int dimensionId = 0;

    bool hasSpeed = false;
    float dx = 0.0f;
    float dy = 0.0f;
    float dz = 0.0f;
    bool hasChunkGrid = false;
    int chunkSlots = 0;
    int chunkLive = 0;
    bool hasTarget = false;
    int targetX = 0;
    int targetY = 0;
    int targetZ = 0;
    char targetName[kTextBytes] = {};
    int targetStateCount = 0;
    char targetStates[kMaxStates][kTextBytes] = {};

    bool hasEntities = false;
    int entityCount = 0;
    int entitySections = 0;
    bool hasEntityTarget = false;
    char entityName[kTextBytes] = {};
    bool hasEntityHit = false;
    float entityHitX = 0.0f;
    float entityHitY = 0.0f;
    float entityHitZ = 0.0f;
    bool hasBiome = false;
    char biome[kTextBytes] = {};
    bool hasSections = false;
    int sectionsNonEmpty = 0;
    int sectionsTotal = 0;

    int targetTagCount = 0;
    char targetTags[kMaxTags][kTextBytes] = {};
    bool hasFluid = false;
    int fluidX = 0;
    int fluidY = 0;
    int fluidZ = 0;
    char fluidName[kTextBytes] = {};
    int fluidStateCount = 0;
    char fluidStates[kMaxFluidStates][kTextBytes] = {};
    int fluidTagCount = 0;
    char fluidTags[kMaxTags][kTextBytes] = {};
    int entityTagCount = 0;
    char entityTags[kMaxTags][kTextBytes] = {};

    bool hasServerChunks = false;
    int serverChunks = 0;
    int serverLoadingChunks = 0;
    bool hasServerEntities = false;
    int serverEntityCount = 0;
    int serverVisibleEntities = 0;
    int serverEntitySections = 0;
    bool hasLocalDifficulty = false;
    float localDifficulty = 0.0f;
    bool hasLocalDifficultyRaw = false;
    float localDifficultyRaw = 0.0f;
    bool hasForcedChunks = false;
    int forcedChunks = 0;

    bool hasClimate = false;
    std::int64_t climate[6] = {};

    bool hasSurfaceLevel = false;
    bool surfaceLevelFound = false;
    int surfaceLevel = 0;
    bool hasDensity = false;
    float density = 0.0f;

    bool hasServerUnload = false;
    int serverUnloadChunks = 0;
};

bool collect(int feetX, int feetY, int feetZ, const Wanted& wanted, bool force = false);

bool countLegacyParticles(const void* engine, int& out);

bool countDataDrivenParticles(const void* engine, const void* frameArg, int& out);
bool read(Sample& out);

void setClimateTargets(void* sampleFunction, const void* generatorVtable, std::int32_t biomeSourceOffset);

struct SurfaceTargetsIn {
    void* columnFunction = nullptr;
    std::int32_t samplerFromSubObject = 0;
    std::int32_t factoryFromSubObject = 0;
    std::int32_t factoryNoBlendFlag = 0;
    const void* densityGrid = nullptr;
};
struct SurfaceConstantsIn {
    float values[9] = {};
    float table[2] = {};
    int topCell = 0;
};
void setSurfaceTargets(const SurfaceTargetsIn& targets, const SurfaceConstantsIn& constants);

void setDiscardTargets(const void* dbVtable, std::int32_t setHead, std::int32_t setSize);
void setRegionalDifficultySlot(std::int32_t slot);
void setRegionalDifficultyTail(const void* tail);

}
