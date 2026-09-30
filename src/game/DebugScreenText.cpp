#include "game/DebugScreenText.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace tsukuyomi::dbgtext {

namespace {

namespace kFormats {
inline constexpr const char* plain = "%s";

inline constexpr const char* densityLabel = " %s: %.3f";
inline constexpr const char* biomeBuilder = "Biome builder PV: %s C: %s E: %d T: %d H: %d";
inline constexpr const char* biome = "Biome: %s";
inline constexpr const char* chunkRender = "C: %d/%d";
inline constexpr const char* chunkRenderPartial = "C: %d";

inline constexpr const char* viewDistance = " %sD: %d";
inline constexpr const char* pendingBuild = ", pC: %03d";
inline constexpr const char* freeBuffers = ", aB: %02d";
inline constexpr const char* chunkSource = "Chunks[C] W: %d, %d E: %d,%d";
inline constexpr const char* tickingChunk = ",%d";
inline constexpr const char* serverSource = "Chunks[S] W: %d";
inline constexpr const char* serverEntities = " E: %d,%d,%d,%d,%d";
inline constexpr const char* nextCount = ",%d";
inline constexpr const char* day = "Day #%lld";
inline constexpr const char* entity = "E: %d/%d";
inline constexpr const char* entityPartial = "E: %d";
inline constexpr const char* simulationDistance = ", SD: %d";

inline constexpr const char* fps = "%d fps T: %s @%sHz";

inline constexpr const char* gameVersion = "Minecraft %s (%s/%s)";
inline constexpr const char* gpu = "GPU: %d%%";
inline constexpr const char* gpuCapped = "GPU: §c100%%";
inline constexpr const char* light = "Client Light: %d (%d sky, %d block)";
inline constexpr const char* localDifficulty = "Local Difficulty: %.2f";
inline constexpr const char* localDifficultyRaw = "Local Difficulty: %.2f // %.2f";
inline constexpr const char* targetedBlock = "Targeted Block: %d, %d, %d";
inline constexpr const char* targetedEntity = "Targeted Entity";
inline constexpr const char* targetedFluid = "Targeted Fluid: %d, %d, %d";
inline constexpr const char* mem = "Mem: %2d%% %03d/%03dMiB";
inline constexpr const char* allocated = "Allocated: %2d%% %03dMiB";
inline constexpr const char* particle = "P: T %d";
inline constexpr const char* xyz = "XYZ: %.3f / %.5f / %.3f";
inline constexpr const char* block = "Block: %d %d %d";
inline constexpr const char* chunk = "Chunk: %d %d %d";
inline constexpr const char* facing = "Facing: %s (Towards %s) (%.1f / %.1f)";
inline constexpr const char* dim = "%s";
inline constexpr const char* forcedChunks = " FC: %d";
inline constexpr const char* section = "Section-relative: %02d %02d %02d";
inline constexpr const char* speed = "Speed: %.3f blocks/tick";
inline constexpr const char* filtering = "Filtering: %s";
inline constexpr const char* sounds = "Sounds: %d/%d";
inline constexpr const char* streamingSounds = " + %d";
inline constexpr const char* soundCache = "Sound cache: %d buffers, %d MiB";
inline constexpr const char* cpu = "CPU: %s";
inline constexpr const char* display = "Display: %s";
inline constexpr const char* window = "Window: %s";
inline constexpr const char* terrainRendering = "Terrain Rendering: %s";

inline constexpr const char* tpsInternal = "Integrated server @ %.1f/50.0 ms, %d tx, %d rx";
inline constexpr const char* tpsExternal = "\"%s\" server, %d tx, %d rx";
}

template <typename... Args>
bool add(char out[][kLineBytes], int maxRows, int& rows, const char* format, Args... args)
{
    if (out == nullptr || rows >= maxRows) return false;
    const int n = std::snprintf(out[rows], kLineBytes, format, args...);
    if (n < 0) { out[rows][0] = '\0'; return false; }
    ++rows;
    return true;
}

template <typename... Args>
void append(char line[kLineBytes], const char* format, Args... args)
{
    const std::size_t used = std::strlen(line);
    if (used < kLineBytes - 1)
        std::snprintf(line + used, kLineBytes - used, format, args...);
}

int tags(const TextValue* values, int count, char out[][kLineBytes], int maxRows)
{
    int rows = 0;
    for (int i = 0; i < count; ++i)
        if (values[i].available) add(out, maxRows, rows, kFormats::plain, values[i].value);
    return rows;
}

int floorInt(double number)
{
    const double floored = std::floor(number);
    if (floored < static_cast<double>(std::numeric_limits<int>::min())) return std::numeric_limits<int>::min();
    if (floored > static_cast<double>(std::numeric_limits<int>::max())) return std::numeric_limits<int>::max();
    return static_cast<int>(floored);
}

int floorDiv16(int number)
{
    const std::int64_t n = number;
    return static_cast<int>(n >= 0 ? n / 16 : -((-n + 15) / 16));
}

const char* dimensionName(int id)
{
    switch (id) {
    case 0: return "minecraft:overworld";
    case 1: return "minecraft:the_nether";
    case 2: return "minecraft:the_end";
    default: return nullptr;
    }
}

void colorBoolean(char line[kLineBytes])
{
    const std::size_t len = std::strlen(line);
    constexpr const char* kTrueSuffix = ": true";
    constexpr const char* kFalseSuffix = ": false";
    constexpr std::size_t kTrueSuffixLen = 6;
    constexpr std::size_t kFalseSuffixLen = 7;
    const char* code = nullptr;
    std::size_t wordLen = 0;
    if (len >= kTrueSuffixLen && std::strcmp(line + len - kTrueSuffixLen, kTrueSuffix) == 0) {
        code = "§a";
        wordLen = 4;
    } else if (len >= kFalseSuffixLen && std::strcmp(line + len - kFalseSuffixLen, kFalseSuffix) == 0) {
        code = "§c";
        wordLen = 5;
    } else {
        return;
    }
    const std::size_t insertAt = len - wordLen;
    const std::size_t codeLen = std::strlen(code);
    if (len + codeLen >= kLineBytes) return;
    std::memmove(line + insertAt + codeLen, line + insertAt, wordLen + 1);
    std::memcpy(line + insertAt, code, codeLen);
}

}

Facing facingOf(double yaw)
{
    if (!std::isfinite(yaw)) return Facing::South;
    const double turns = std::floor(yaw / 90.0 + 0.5);
    double quarter = std::fmod(turns, 4.0);
    if (quarter < 0.0) quarter += 4.0;
    return static_cast<Facing>(static_cast<int>(quarter) & 3);
}

const char* facingName(Facing facing)
{
    constexpr const char* names[] = {"south", "west", "north", "east"};
    return names[static_cast<int>(facing) & 3];
}

const char* facingTowards(Facing facing)
{
    constexpr const char* names[] = {"positive Z", "negative X", "negative Z", "positive X"};
    return names[static_cast<int>(facing) & 3];
}

void formatSection(int blockX, int blockY, int blockZ, char out[kLineBytes])
{
    if (out != nullptr) std::snprintf(out, kLineBytes, kFormats::section,
                                      blockX & 15, blockY & 15, blockZ & 15);
}

double peaksAndValleys(double weirdness)
{
    return -3.0 * (std::fabs(std::fabs(weirdness) - 2.0 / 3.0) - 1.0 / 3.0);
}

const char* biomeBuilderPeaksAndValleys(double pv)
{

    if (pv < -0.85) return "Valley";
    if (pv < -0.2) return "Low";
    if (pv < 0.2) return "Mid";
    if (pv < 0.7) return "High";
    return "Peak";
}

const char* biomeBuilderContinentalness(double c)
{
    if (c < -1.05) return "Mushroom fields";
    if (c < -0.455) return "Deep ocean";
    if (c < -0.19) return "Ocean";
    if (c < -0.11) return "Coast";
    if (c < 0.03) return "Near inland";
    if (c < 0.3) return "Mid inland";
    return "Far inland";
}

namespace {
int bandOf(double v, const double* upper, int count)
{
    for (int i = 0; i < count; ++i)
        if (v < upper[i]) return i;
    return count;
}
}

int biomeBuilderErosion(double e)
{
    static constexpr double kUpper[] = {-0.78, -0.375, -0.2225, 0.05, 0.45, 0.55};
    return bandOf(e, kUpper, 6);
}

int biomeBuilderTemperature(double t)
{
    static constexpr double kUpper[] = {-0.45, -0.15, 0.2, 0.55};
    return bandOf(t, kUpper, 4);
}

int biomeBuilderHumidity(double h)
{
    static constexpr double kUpper[] = {-0.35, -0.1, 0.1, 0.3};
    return bandOf(h, kUpper, 4);
}

bool validServerVersion(const char* value, std::size_t length)
{
    if (value == nullptr || length < 1 || length > 31) return false;
    for (std::size_t i = 0; i < length; ++i) {
        if ((value[i] < '0' || value[i] > '9') && value[i] != '.') return false;
    }
    return true;
}

int formatBiome(const Sample& s, char out[][kLineBytes], int maxRows)
{
    int n = 0;
    if (s.biome.available) add(out, maxRows, n, kFormats::biome, s.biome.value);
    return n;
}

int formatChunkGenerationStats(const Sample& s, char out[][kLineBytes], int maxRows)
{

    if (!(s.noiseT.available && s.noiseV.available && s.noiseC.available && s.noiseE.available && s.noiseD.available
          && s.noiseW.available))
        return 0;
    const double pv = peaksAndValleys(s.noiseW.value);
    char line[kLineBytes] = "Density";
    if (s.noiseN.available) append(line, kFormats::densityLabel, "N", s.noiseN.value);
    append(line, kFormats::densityLabel, "T", s.noiseT.value);
    append(line, kFormats::densityLabel, "V", s.noiseV.value);
    append(line, kFormats::densityLabel, "C", s.noiseC.value);
    append(line, kFormats::densityLabel, "E", s.noiseE.value);
    append(line, kFormats::densityLabel, "D", s.noiseD.value);
    append(line, kFormats::densityLabel, "W", s.noiseW.value);
    append(line, kFormats::densityLabel, "PV", pv);
    if (s.noisePS.available) append(line, kFormats::densityLabel, "PS", s.noisePS.value);
    int n = 0;
    add(out, maxRows, n, kFormats::plain, line);
    add(out, maxRows, n, kFormats::biomeBuilder, biomeBuilderPeaksAndValleys(pv),
        biomeBuilderContinentalness(s.noiseC.value), biomeBuilderErosion(s.noiseE.value),
        biomeBuilderTemperature(s.noiseT.value), biomeBuilderHumidity(s.noiseV.value));
    return n;
}

int formatChunkRenderStats(const Sample& s, char out[][kLineBytes], int maxRows)
{
    if (!s.renderedSections.available) return 0;
    int n = 0;
    if (s.totalSections.available) {
        if (!add(out, maxRows, n, kFormats::chunkRender, s.renderedSections.value, s.totalSections.value)) return 0;
    } else if (!add(out, maxRows, n, kFormats::chunkRenderPartial, s.renderedSections.value)) return 0;
    char* line = out[0];

    if (s.viewDistance.available) {
        const char* spectator = (s.spectator.available && s.spectator.value != 0) ? "(s) " : "";
        append(line, kFormats::viewDistance, spectator, s.viewDistance.value);
    }
    if (s.pendingBuild.available) append(line, kFormats::pendingBuild, s.pendingBuild.value);
    if (s.freeBuffers.available) append(line, kFormats::freeBuffers, s.freeBuffers.value);
    return n;
}

int formatChunkSourceStats(const Sample& s, char out[][kLineBytes], int maxRows)
{
    int n = 0;
    if (s.clientWanted.available && s.clientLive.available &&
        s.clientEntities.available && s.clientEntitySections.available &&
        add(out, maxRows, n, kFormats::chunkSource, s.clientWanted.value, s.clientLive.value,
            s.clientEntities.value, s.clientEntitySections.value)) {
        if (s.tickingChunks.available) append(out[n - 1], kFormats::tickingChunk, s.tickingChunks.value);
    }

    if (s.serverChunks.available && add(out, maxRows, n, kFormats::serverSource, s.serverChunks.value)) {
        char* line = out[n - 1];
        if (s.serverEntities.available && s.serverVisible.available && s.serverSections.available &&
            s.serverWanted.available && s.serverTicking.available) {
            append(line, kFormats::serverEntities, s.serverEntities.value, s.serverVisible.value,
                   s.serverSections.value, s.serverWanted.value, s.serverTicking.value);
            if (s.serverLoad.available) {
                append(line, kFormats::nextCount, s.serverLoad.value);
                if (s.serverUnload.available) append(line, kFormats::nextCount, s.serverUnload.value);
            }
        }
    }
    return n;
}

int formatDayCount(const Sample& s, char out[][kLineBytes], int maxRows)
{
    int n = 0;
    if (s.dayTime.available) add(out, maxRows, n, kFormats::day, static_cast<long long>(s.dayTime.value / 24000));
    return n;
}

int formatEntityRenderStats(const Sample& s, char out[][kLineBytes], int maxRows)
{
    if (!s.renderedEntities.available) return 0;
    int n = 0;
    if (s.totalEntities.available) add(out, maxRows, n, kFormats::entity, s.renderedEntities.value, s.totalEntities.value);
    else add(out, maxRows, n, kFormats::entityPartial, s.renderedEntities.value);
    if (n && s.simulationDistance.available) append(out[0], kFormats::simulationDistance, s.simulationDistance.value);
    return n;
}

int formatFps(const Sample& s, char out[][kLineBytes], int maxRows)
{
    if (!s.fps.available) return 0;

    char limit[16];
    if (!s.fpsLimit.available || s.fpsLimit.value == 0) std::snprintf(limit, sizeof limit, "inf");
    else std::snprintf(limit, sizeof limit, "%d", s.fpsLimit.value);

    char hz[32];
    if (!s.refreshRate.available) {
        std::snprintf(hz, sizeof hz, "0");
    } else if (std::rint(s.refreshRate.value) == s.refreshRate.value) {

        std::snprintf(hz, sizeof hz, "%d", static_cast<int>(std::rint(s.refreshRate.value)));
    } else {
        std::snprintf(hz, sizeof hz, "%.2f", s.refreshRate.value);
    }

    int n = 0;
    add(out, maxRows, n, kFormats::fps, s.fps.value, limit, hz);
    return n;
}

int formatGameVersion(const Sample& s, char out[][kLineBytes], int maxRows)
{

    if (!s.gameVersion.available || !s.modName.available) return 0;
    const char* const launched = s.launchedVersion.available ? s.launchedVersion.value : s.gameVersion.value;
    int n = 0;
    add(out, maxRows, n, kFormats::gameVersion, s.gameVersion.value, launched, s.modName.value);
    return n;
}

int formatGpuUtilization(const Sample& s, char out[][kLineBytes], int maxRows)
{
    int n = 0;
    if (s.gpuPercent.available) {
        if (s.gpuPercent.value > 100) add(out, maxRows, n, kFormats::gpuCapped);
        else add(out, maxRows, n, kFormats::gpu, s.gpuPercent.value);
    }
    return n;
}

int formatLightLevels(const Sample& s, char out[][kLineBytes], int maxRows)
{
    if (!s.skyLight.available || !s.blockLight.available) return 0;
    int n = 0;
    add(out, maxRows, n, kFormats::light, std::max(s.skyLight.value, s.blockLight.value),
        s.skyLight.value, s.blockLight.value);
    return n;
}

bool regionalRaw(int difficulty, float clamped, float xmm3, float& raw)
{
    if (difficulty < 0) return false;

    if (difficulty == 0) raw = 0.0f;
    else if (std::bit_cast<std::uint32_t>(xmm3) == std::bit_cast<std::uint32_t>(clamped)) {

        raw = clamped * 2.0f + 2.0f;
    } else raw = xmm3;
    return std::isfinite(raw) && raw >= 0.0f && raw <= 10.0f;
}

int formatLocalDifficulty(const Sample& s, char out[][kLineBytes], int maxRows)
{
    if (!s.localDifficulty.available) return 0;
    int n = 0;

    const double rounded = std::round(static_cast<double>(s.localDifficulty.value) * 100.0) / 100.0;
    if (s.localDifficultyRaw.available) {
        const double roundedRaw = std::round(static_cast<double>(s.localDifficultyRaw.value) * 100.0) / 100.0;
        add(out, maxRows, n, kFormats::localDifficultyRaw, roundedRaw, rounded);
    } else add(out, maxRows, n, kFormats::localDifficulty, rounded);
    return n;
}

int formatLookingAtBlockState(const Sample& s, char out[][kLineBytes], int maxRows)
{
    if (!s.blockX.available || !s.blockY.available || !s.blockZ.available) return 0;
    int n = 0;
    add(out, maxRows, n, kFormats::targetedBlock, s.blockX.value, s.blockY.value, s.blockZ.value);
    if (s.blockName.available) add(out, maxRows, n, kFormats::plain, s.blockName.value);
    for (const TextValue& state : s.blockStates) {
        if (state.available && add(out, maxRows, n, kFormats::plain, state.value)) colorBoolean(out[n - 1]);
    }
    return n;
}

int formatLookingAtBlockTags(const Sample& s, char out[][kLineBytes], int maxRows)
{
    return tags(s.blockTags, kVariableRows, out, maxRows);
}

int formatLookingAtEntity(const Sample& s, char out[][kLineBytes], int maxRows)
{
    if (!s.entityName.available) return 0;
    int n = 0;
    add(out, maxRows, n, kFormats::plain, kFormats::targetedEntity);
    add(out, maxRows, n, kFormats::plain, s.entityName.value);
    return n;
}

int formatLookingAtEntityTags(const Sample& s, char out[][kLineBytes], int maxRows)
{
    return tags(s.entityTags, kVariableRows, out, maxRows);
}

int formatLookingAtFluidState(const Sample& s, char out[][kLineBytes], int maxRows)
{
    if (!s.fluidX.available || !s.fluidY.available || !s.fluidZ.available) return 0;
    int n = 0;
    add(out, maxRows, n, kFormats::targetedFluid, s.fluidX.value, s.fluidY.value, s.fluidZ.value);
    if (s.fluidName.available) add(out, maxRows, n, kFormats::plain, s.fluidName.value);
    for (const TextValue& state : s.fluidStates) {
        if (state.available && add(out, maxRows, n, kFormats::plain, state.value)) colorBoolean(out[n - 1]);
    }

    return n;
}

int formatLookingAtFluidTags(const Sample& s, char out[][kLineBytes], int maxRows)
{
    return tags(s.fluidTags, kVariableRows, out, maxRows);
}

int formatMemory(const Sample& s, char out[][kLineBytes], int maxRows)
{
    int n = 0;
    if (s.memoryPercent.available && s.privateMegabytes.available && s.totalMegabytes.available)
        add(out, maxRows, n, kFormats::mem, s.memoryPercent.value, s.privateMegabytes.value,
            s.totalMegabytes.value);
    if (s.allocatedPercent.available && s.workingSetMegabytes.available)
        add(out, maxRows, n, kFormats::allocated, s.allocatedPercent.value, s.workingSetMegabytes.value);
    return n;
}

int formatParticleRenderStats(const Sample& s, char out[][kLineBytes], int maxRows)
{
    int n = 0;
    if (s.particleTotal.available) add(out, maxRows, n, kFormats::particle, s.particleTotal.value);

    return n;
}

int formatPlayerPosition(const Sample& s, char out[][kLineBytes], int maxRows)
{
    int n = 0;
    if (s.x.available && s.y.available && s.z.available) {
        add(out, maxRows, n, kFormats::xyz, s.x.value, s.y.value, s.z.value);
        const int bx = floorInt(s.x.value), by = floorInt(s.y.value), bz = floorInt(s.z.value);
        add(out, maxRows, n, kFormats::block, bx, by, bz);
        const int cx = floorDiv16(bx), cy = floorDiv16(by), cz = floorDiv16(bz);
        const bool chunkAdded = add(out, maxRows, n, kFormats::chunk, cx, cy, cz);
        if (chunkAdded && s.dimensionId.available && s.subChunkKey.available) {
            if (s.subChunkFile.available)
                append(out[n - 1], " [%s in %s]", s.subChunkKey.value, s.subChunkFile.value);
            else
                append(out[n - 1], " [%s]", s.subChunkKey.value);
        }
    }
    if (s.yaw.available && s.pitch.available) {
        const Facing facing = facingOf(s.yaw.value);
        add(out, maxRows, n, kFormats::facing, facingName(facing), facingTowards(facing),
            s.yaw.value, s.pitch.value);
    }
    if (s.dimensionId.available) {
        const char* dimension = dimensionName(s.dimensionId.value);
        if (dimension != nullptr) {

            if (add(out, maxRows, n, kFormats::dim, dimension))
                append(out[n - 1], kFormats::forcedChunks, s.forcedChunks.available ? s.forcedChunks.value : 0);
        }
    }
    return n;
}

int formatPlayerSectionPosition(const Sample& s, char out[][kLineBytes], int maxRows)
{
    if (!s.x.available || !s.y.available || !s.z.available) return 0;
    int n = 0;
    if (out != nullptr && maxRows > 0) {
        formatSection(floorInt(s.x.value), floorInt(s.y.value), floorInt(s.z.value), out[0]);
        n = 1;
    }
    return n;
}

int formatPlayerSpeed(const Sample& s, char out[][kLineBytes], int maxRows)
{
    int n = 0;
    if (s.speed.available) add(out, maxRows, n, kFormats::speed, s.speed.value);
    return n;
}

int formatSimplePerformanceImpactors(const Sample& s, char out[][kLineBytes], int maxRows)
{
    int n = 0;
    if (s.filtering.available) add(out, maxRows, n, kFormats::filtering, s.filtering.value);

    if (s.terrainRendering.available) add(out, maxRows, n, kFormats::terrainRendering, s.terrainRendering.value);
    return n;
}

int formatSoundCache(const Sample& s, char out[][kLineBytes], int maxRows)
{

    if (!s.soundCacheBuffers.available || !s.soundCacheMebibytes.available) return 0;
    int n = 0;
    add(out, maxRows, n, kFormats::soundCache, s.soundCacheBuffers.value, s.soundCacheMebibytes.value);
    return n;
}

int formatSoundMood(const Sample& s, char out[][kLineBytes], int maxRows)
{
    if (!s.staticSounds.available || !s.staticSoundCap.available) return 0;
    int n = 0;
    add(out, maxRows, n, kFormats::sounds, s.staticSounds.value, s.staticSoundCap.value);
    if (!n) return 0;
    if (s.streamingSounds.available) append(out[0], kFormats::streamingSounds, s.streamingSounds.value);

    return n;
}

int formatSystemSpecs(const Sample& s, char out[][kLineBytes], int maxRows)
{
    int n = 0;
    if (s.cpu.available) add(out, maxRows, n, kFormats::cpu, s.cpu.value);
    if (s.display.available) add(out, maxRows, n, kFormats::display, s.display.value);
    if (s.window.available) add(out, maxRows, n, kFormats::window, s.window.value);
    if (s.graphicsCard.available) add(out, maxRows, n, kFormats::plain, s.graphicsCard.value);
    if (s.graphicsApi.available) add(out, maxRows, n, kFormats::plain, s.graphicsApi.value);
    return n;
}

int formatTps(const Sample& s, char out[][kLineBytes], int maxRows)
{
    const int tx = s.packetsUpPerTick.available ? s.packetsUpPerTick.value : 0;
    const int rx = s.packetsDownPerTick.available ? s.packetsDownPerTick.value : 0;
    int n = 0;
    if (s.localServer.available && s.localServer.value) {

        const double ms = s.millisecondsPerTick.available ? s.millisecondsPerTick.value : 0.0;
        add(out, maxRows, n, kFormats::tpsInternal, ms, tx, rx);
        return n;
    }

    if (!s.serverBrand.available) return 0;
    add(out, maxRows, n, kFormats::tpsExternal, s.serverBrand.value, tx, rx);
    return n;
}

int formatElement(const char* id, const Sample& s, char out[][kLineBytes], int maxRows)
{
    if (id == nullptr) {
        return 0;
    }
    struct Entry {
        const char* id;
        int (*fn)(const Sample&, char[][kLineBytes], int);
    };
    static constexpr Entry kTable[] = {
        {"biome", formatBiome},
        {"chunk_generation_stats", formatChunkGenerationStats},
        {"chunk_render_stats", formatChunkRenderStats},
        {"chunk_source_stats", formatChunkSourceStats},
        {"day_count", formatDayCount},
        {"entity_render_stats", formatEntityRenderStats},
        {"fps", formatFps},
        {"game_version", formatGameVersion},
        {"gpu_utilization", formatGpuUtilization},
        {"light_levels", formatLightLevels},
        {"local_difficulty", formatLocalDifficulty},
        {"looking_at_block_state", formatLookingAtBlockState},
        {"looking_at_block_tags", formatLookingAtBlockTags},
        {"looking_at_entity", formatLookingAtEntity},
        {"looking_at_entity_tags", formatLookingAtEntityTags},
        {"looking_at_fluid_state", formatLookingAtFluidState},
        {"looking_at_fluid_tags", formatLookingAtFluidTags},
        {"memory", formatMemory},
        {"particle_render_stats", formatParticleRenderStats},
        {"player_position", formatPlayerPosition},
        {"player_section_position", formatPlayerSectionPosition},
        {"player_speed", formatPlayerSpeed},
        {"simple_performance_impactors", formatSimplePerformanceImpactors},
        {"sound_cache", formatSoundCache},
        {"sound_mood", formatSoundMood},
        {"system_specs", formatSystemSpecs},
        {"tps", formatTps},
    };
    static_assert(sizeof kTable / sizeof kTable[0] == 27);
    for (const Entry& entry : kTable) {
        if (std::strcmp(entry.id, id) == 0) {
            return entry.fn(s, out, maxRows);
        }
    }
    return 0;
}

}
