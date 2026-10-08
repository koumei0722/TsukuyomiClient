#pragma once

#include <cstdint>
#include <cstddef>

namespace tsukuyomi::dbgtext {

inline constexpr int kLineBytes = 128;
inline constexpr int kVariableRows = 7;

template <typename T> struct Value {
    bool available;
    T value;
};

struct TextValue {
    bool available;
    char value[96];
};

struct Sample {
    TextValue biome;
    Value<int> renderedSections, totalSections, spectator, viewDistance, pendingBuild, freeBuffers;
    Value<int> clientWanted, clientLive, clientEntities, clientEntitySections, tickingChunks;
    Value<int> serverChunks, serverWanted, serverEntities, serverVisible, serverSections;
    Value<int> serverTicking, serverLoad, serverUnload;
    Value<std::int64_t> dayTime;
    Value<float> localDifficulty;
    Value<float> localDifficultyRaw;
    Value<double> noiseT, noiseV, noiseC, noiseE, noiseD, noiseW;
    Value<double> noiseN, noisePS;
    Value<int> renderedEntities, totalEntities, simulationDistance;
    Value<int> fps, fpsLimit;
    Value<int> foodHunger;
    Value<float> foodSaturation, foodExhaustion;
    Value<double> refreshRate;
    TextValue gameVersion, launchedVersion, modName;
    Value<int> gpuPercent;
    Value<int> skyLight, blockLight;
    Value<int> difficultyId;
    Value<int> blockX, blockY, blockZ;
    TextValue blockName, blockStates[5], blockTags[kVariableRows];
    TextValue entityName, entityTags[kVariableRows];
    Value<int> fluidX, fluidY, fluidZ;
    TextValue fluidName, fluidStates[5], fluidTags[kVariableRows];
    Value<int> memoryPercent, privateMegabytes, totalMegabytes;
    Value<int> allocatedPercent, workingSetMegabytes;
    Value<int> particleTotal;
    Value<double> x, y, z, yaw, pitch;
    Value<int> dimensionId, forcedChunks;
    TextValue subChunkKey, subChunkFile;
    Value<double> speed;
    TextValue filtering;
    Value<int> staticSounds, staticSoundCap, streamingSounds;
    Value<int> soundCacheBuffers, soundCacheMebibytes;
    TextValue cpu, display, window, graphicsCard, graphicsApi;
    TextValue terrainRendering;
    TextValue serverBrand;
    Value<bool> localServer;
    Value<double> millisecondsPerTick;
    Value<int> packetsUpPerTick, packetsDownPerTick;
};

enum class Facing { South, West, North, East };
Facing facingOf(double yaw);
const char* facingName(Facing facing);
const char* facingTowards(Facing facing);
void formatSection(int blockX, int blockY, int blockZ, char out[kLineBytes]);

double peaksAndValleys(double weirdness);
const char* biomeBuilderPeaksAndValleys(double pv);
const char* biomeBuilderContinentalness(double c);
int biomeBuilderErosion(double e);
int biomeBuilderTemperature(double t);
int biomeBuilderHumidity(double h);

bool validServerVersion(const char* value, std::size_t length);

bool regionalRaw(int difficulty, float clamped, float xmm3, float& raw);

int formatBiome(const Sample&, char out[][kLineBytes], int maxRows);
int formatChunkGenerationStats(const Sample&, char out[][kLineBytes], int maxRows);
int formatChunkRenderStats(const Sample&, char out[][kLineBytes], int maxRows);
int formatChunkSourceStats(const Sample&, char out[][kLineBytes], int maxRows);
int formatDayCount(const Sample&, char out[][kLineBytes], int maxRows);
int formatEntityRenderStats(const Sample&, char out[][kLineBytes], int maxRows);
int formatFps(const Sample&, char out[][kLineBytes], int maxRows);
int formatFoodStats(const Sample&, char out[][kLineBytes], int maxRows);
int formatGameVersion(const Sample&, char out[][kLineBytes], int maxRows);
int formatGpuUtilization(const Sample&, char out[][kLineBytes], int maxRows);
int formatLightLevels(const Sample&, char out[][kLineBytes], int maxRows);
int formatLocalDifficulty(const Sample&, char out[][kLineBytes], int maxRows);
int formatLookingAtBlockState(const Sample&, char out[][kLineBytes], int maxRows);
int formatLookingAtBlockTags(const Sample&, char out[][kLineBytes], int maxRows);
int formatLookingAtEntity(const Sample&, char out[][kLineBytes], int maxRows);
int formatLookingAtEntityTags(const Sample&, char out[][kLineBytes], int maxRows);
int formatLookingAtFluidState(const Sample&, char out[][kLineBytes], int maxRows);
int formatLookingAtFluidTags(const Sample&, char out[][kLineBytes], int maxRows);
int formatMemory(const Sample&, char out[][kLineBytes], int maxRows);
int formatParticleRenderStats(const Sample&, char out[][kLineBytes], int maxRows);
int formatPlayerPosition(const Sample&, char out[][kLineBytes], int maxRows);
int formatPlayerSectionPosition(const Sample&, char out[][kLineBytes], int maxRows);
int formatPlayerSpeed(const Sample&, char out[][kLineBytes], int maxRows);
int formatSimplePerformanceImpactors(const Sample&, char out[][kLineBytes], int maxRows);
int formatSoundCache(const Sample&, char out[][kLineBytes], int maxRows);
int formatSoundMood(const Sample&, char out[][kLineBytes], int maxRows);
int formatSystemSpecs(const Sample&, char out[][kLineBytes], int maxRows);
int formatTps(const Sample&, char out[][kLineBytes], int maxRows);
int formatElement(const char* id, const Sample&, char out[][kLineBytes], int maxRows);

}
