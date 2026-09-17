#pragma once

#include <atomic>
#include <climits>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "game/BlockRegistry.h"
#include "game/BlockWrite.h"
#include "game/Structure.h"
#include "input/Hotkey.h"
#include "modules/Module.h"

namespace tsukuyomi {

class Schematica : public Module {
public:
    static Schematica& instance();

    const wchar_t* name() const override { return L"Schematica"; }

    bool available() const override { return true; }

    MenuItem buildMenu() override;
    void loadConfig(const nlohmann::json& section) override;
    void saveConfig(nlohmann::json& section) const override;

    void onScansReady() override;

    void shutdown() override;

    void onPlayerViewUpdate();

    void noteGhostCellFreed(std::int32_t x, std::int32_t y, std::int32_t z);

    void noteActorCellTaken(std::int32_t x, std::int32_t y, std::int32_t z);

protected:
    Schematica() = default;

    void onEnabledChanged(bool enabled) override;

    void onUpdate() override;

    bool persistEnabled() const override { return false; }

private:

    void runImport(void* ownerWindow);

    static unsigned long __stdcall importThreadMain(void* param);

    void startImport();

    void finishImport();

    std::atomic<bool> m_importBusy{false};
    std::atomic<bool> m_importDone{false};
    std::mutex m_importMutex;
    std::wstring m_importedStem;

    std::wstring currentFileName() const;
    void selectNextFile();

    struct Blueprint {
        std::wstring name;
        std::wstring fileName;

        std::atomic<bool> visible{false};
        std::atomic<int> posX{0};
        std::atomic<int> posY{0};
        std::atomic<int> posZ{0};
        std::atomic<int> rotation{0};

        structure::Structure loaded;
        std::vector<const void*> paletteBlocks;
        std::size_t paletteBase = 0;
        bool ready = false;

        std::atomic<int> sizeX{0};
        std::atomic<int> sizeY{0};
        std::atomic<int> sizeZ{0};
        std::atomic<std::size_t> solidCount{0};
        std::vector<std::pair<std::string, std::size_t>> materials;
    };

    struct Cell {
        std::int32_t x = 0;
        std::int32_t y = 0;
        std::int32_t z = 0;
        std::size_t entry = 0;
        std::int32_t entry2 = -1;
        int owner = -1;
        const std::string* name = nullptr;
        float yaw = 0.0F;
        int pitch = 0;
    };
    static constexpr std::size_t kAirCell = static_cast<std::size_t>(-1);

    struct Box {
        std::int32_t x0 = 0, y0 = 0, z0 = 0;
        std::int32_t x1 = 0, y1 = 0, z1 = 0;
    };

public:
    std::size_t blueprintCount() const;
    void refreshFiles();
    void selectBlueprintQuiet(std::size_t at);
    std::wstring blueprintName(std::size_t at) const;
    bool blueprintVisible(std::size_t at) const;
    void setBlueprintVisible(std::size_t at, bool on);
    int editingIndex() const;
    void selectBlueprint(std::size_t at);
    int blueprintPos(std::size_t at, int axis) const;
    void setBlueprintPos(std::size_t at, int axis, int value);
    int blueprintRotation(std::size_t at) const;
    void setBlueprintRotation(std::size_t at, int quarters);

    enum LayerMode : int {
        kLayerAll = 0,
        kLayerSingle = 1,
        kLayerBelow = 2,
        kLayerAbove = 3,
        kLayerRange = 4,
    };
    static constexpr int kLayerModeCount = 5;
    int layerMode() const { return m_layerMode.load(std::memory_order_relaxed); }
    int layerAxis() const { return m_layerAxis.load(std::memory_order_relaxed); }
    void layerRange(bool& on, int& axis, int& lo, int& hi) const;
    void layerRangeOffsets(bool& on, int& axis, int& lo, int& hi) const;
    bool layerOrigin(int& x, int& y, int& z, std::wstring* name) const;
    int layerOriginOf(int axis) const;
    void stepLayer(int delta);
    void setLayerHere();

    bool deleteBlueprint(std::size_t at);

    bool blueprintInfo(std::size_t at, int& sizeX, int& sizeY, int& sizeZ,
                       std::size_t& solid) const;

    struct MaterialRow {
        std::wstring name;
        std::size_t count = 0;
        std::int32_t iconIdAux = 0;
        bool hasIcon = false;
        int stackSize = 0;
    };
    std::vector<MaterialRow> blueprintMaterials(std::size_t at, std::size_t limit,
                                                std::size_t* kinds = nullptr) const;

    void diffTally(std::size_t (&out)[6]) const;

    std::atomic<unsigned long long> m_deleteArmedAt{0};
    std::wstring m_deleteArmedName;
    std::atomic<unsigned long long> m_deleteRestAt{0};
    std::atomic<bool> m_deleteNeedsPick{false};
    bool deleteArmed() const;
    static constexpr unsigned long long kDeleteArmMs = 5000;
    static constexpr unsigned long long kDeleteMinMs = 600;
    static constexpr unsigned long long kDeleteRestMs = 1500;

private:
    struct Saved {
        std::wstring name;
        bool visible = false;
        int x = 0, y = 0, z = 0;
        int rotation = 0;
    };
    std::vector<Saved> m_pendingBlueprints;

    std::vector<std::unique_ptr<Blueprint>> m_blueprints;
    int m_editing = -1;

    std::vector<Cell> m_cells;
    std::unordered_map<std::uint64_t, std::size_t> m_cellAt;
    std::vector<Box> m_boxes;
    Box m_region;
    std::int32_t m_regionSizeX = 0;
    std::int32_t m_regionSizeY = 0;
    std::int32_t m_regionSizeZ = 0;
    Box unionBox() const;

    std::atomic<bool> m_cellsDirty{true};
    void loadVisible();
    void rebuildCells();
    static std::uint64_t packCell(std::int32_t x, std::int32_t y, std::int32_t z);

    mutable std::mutex m_filesMutex;

    std::unordered_map<std::string, std::int32_t> m_iconIds;
    std::size_t m_iconMissing = 0;

    std::unordered_map<std::string, int> m_stackSizes;
    std::size_t m_stackMissing = 0;

    Hotkey m_pageKey;
    std::atomic<bool> m_pageRequested{false};

    static constexpr int kLayerUpDefaultKey = 0x21;
    static constexpr int kLayerDownDefaultKey = 0x22;
    Hotkey m_layerUpKey{std::vector<int>{kLayerUpDefaultKey}};
    Hotkey m_layerDownKey{std::vector<int>{kLayerDownDefaultKey}};

    std::atomic<int> m_layerMode{kLayerAll};
    std::atomic<int> m_layerAxis{1};
    std::atomic<int> m_layerValue{64};
    std::atomic<int> m_layerMin{0};
    std::atomic<int> m_layerMax{0};
    std::atomic<unsigned> m_layerVersion{1};
    static constexpr unsigned long long kLayerTypeSettleMs = 400;
    std::atomic<unsigned long long> m_layerTypedAt{0};
    bool m_layerAppliedOn = false;
    int m_layerAppliedAxis = 1;
    int m_layerAppliedLo = 0;
    int m_layerAppliedHi = 0;
    static int clampLayerValue(int axis, int value);
    void applyLayerFilter();

    void scanFiles();

    void clearStep();

    void pruneStep();

    static constexpr std::size_t kPrunePerFrame = 64;

    std::atomic<bool> m_prunePending{false};

    static constexpr int kPruneDelayFrames = 90;
    int m_pruneDelay = 0;
    std::size_t m_prunedUpTo = 0;
    std::size_t m_prunedDropped = 0;

    void diffStep();

    std::vector<std::string> m_paletteKeys;

    static constexpr std::size_t kDiffPerFrame = 256;

    std::size_t m_diffUpTo = 0;

    std::size_t m_diffTally[6] = {};

    std::size_t m_diffWater = 0;
    std::size_t m_diffWaterUnknown = 0;
    std::size_t m_diffWorldWater = 0;
    std::size_t m_diffWantWater = 0;

    std::size_t m_diffDropped = 0;

    std::size_t m_diffRestored = 0;

    std::vector<std::pair<blockwrite::BlockPos, const void*>> m_diffActors;

    std::size_t m_diffChanged = 0;

    std::unordered_set<std::uint64_t> m_lapTagChunks;
    std::unordered_set<std::uint64_t> m_lapColorChunks;
    void noteLapChunk(std::unordered_set<std::uint64_t>& into, std::int32_t x, std::int32_t y,
                      std::int32_t z);

    void diffCell(std::size_t at, bool early);

    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> m_chunkCells;
    std::unordered_map<std::uint64_t, void*> m_learnedKeys;
    std::vector<blockwrite::BlockPos> m_earlyRegion;
    std::int32_t m_earlyRegionKey[6] = {0, 0, 0, 0, 0, 0};
    std::size_t m_earlyCursor = 0;
    std::unordered_set<std::uint64_t> m_earlyOutside;
    std::uint64_t m_earlyRegionGen = 0;
    static constexpr std::size_t kEarlyCellBudget = 8192;
    std::size_t m_earlyChunks = 0;
    std::size_t m_earlyCells = 0;
    std::size_t m_earlyCalls = 0;
    std::size_t m_earlyLate = 0;
    std::size_t m_relearnChunks = 0;
    std::atomic<std::size_t> m_earlyBusy{0};

    struct ChunkChange {
        std::int32_t cx = 0;
        std::int32_t cy = 0;
        std::int32_t cz = 0;
        std::uint64_t seq = 0;
        std::uint64_t askedSeq = 0;
        unsigned long long askedAt = 0;
        std::uint32_t askCount = 0;
    };
    static constexpr std::uint32_t kHealMaxAsks = 3;
    std::unordered_map<std::uint64_t, ChunkChange> m_chunkChanges;
    std::size_t m_healAsked = 0;
    static constexpr unsigned long long kHealRetryMs = 4000;
    void noteChunkChange(std::int32_t x, std::int32_t y, std::int32_t z);
    std::size_t healStaleChunks(unsigned long long now);

public:
    void earlyLearnFrame();
private:

    unsigned long long m_learnAt = 0;
    static constexpr unsigned long long kLearnMs = 1000;

    void publishBoxes(bool force, bool changed = false);

    void publishBoxesLive();

    static constexpr std::size_t kBoxLimit = 500000;

    std::size_t m_boxSignature = 0;
    unsigned long long m_boxPublishedAt = 0;
    std::size_t m_boxPublished = 0;
    std::size_t m_boxSkipped = 0;
    static constexpr unsigned long long kBoxPublishMs = 500;

    bool m_boxLiveDirty = false;
    unsigned long long m_boxLiveAt = 0;
    static constexpr unsigned long long kBoxLiveMs = 100;
    unsigned long long m_boxLiveSince = 0;
    std::size_t m_boxLiveCells = 0;
    int m_boxLiveLogged = 0;
    static constexpr int kBoxLiveLogMax = 8;

    std::size_t m_diffLaps = 0;
    unsigned long long m_diffLoggedAt = 0;
    static constexpr unsigned long long kDiffLogMs = 30000;

    static constexpr std::size_t kDirtyCallers = 10;
    std::size_t m_dirtyFrom[kDirtyCallers]{};
    std::size_t m_learnedLate = 0;

    static constexpr unsigned long long kBoxModeRetryMs = 500;
    unsigned long long m_boxModeRetryAt = 0;

    std::size_t dirtyChunks(int who = 0, bool askAll = true);

    void rebuildGhostChunkList();

    std::vector<blockwrite::BlockPos> regionChunkList() const;

    std::size_t learnRegionSubChunks(bool judgeNew);

    void restoreFreedCells();

    std::mutex m_freedMutex;
    std::vector<blockwrite::BlockPos> m_freedCells;
    std::atomic<bool> m_freedPending{false};

    static constexpr std::size_t kFreedLimit = 4096;

public:
    void noteChunkRebuilt(std::int32_t x, std::int32_t y, std::int32_t z);

private:
    void reviewRebuiltChunks();

    std::mutex m_rebuiltMutex;
    std::vector<blockwrite::BlockPos> m_rebuiltChunks;
    std::atomic<bool> m_rebuiltPending{false};
    static constexpr std::size_t kRebuiltLimit = 64;

    void fixTakenActorCells();

    std::mutex m_takenMutex;
    std::vector<blockwrite::BlockPos> m_takenCells;
    std::atomic<bool> m_takenPending{false};
    bool m_takenOverflow = false;
    static constexpr std::size_t kTakenLimit = 4096;

    std::atomic<std::size_t> m_actorLive{0};
    void syncActorLive()
    {
        m_actorLive.store(m_actorPlaced.size(), std::memory_order_relaxed);
    }

    std::atomic<bool> m_meshBoxesArmed{false};

    std::vector<blockwrite::BlockPos> m_ghostChunks;

    void maybeRenudge();

    static constexpr unsigned long long kChunkSettleMs = 1200;
    static constexpr unsigned long long kRenudgeMs = 30000;

    int m_lastChunkX = INT_MIN;
    int m_lastChunkZ = INT_MIN;
    unsigned long long m_chunkMovedAt = 0;
    bool m_chunkSettling = false;
    unsigned long long m_lastRenudgeAt = 0;

    static constexpr unsigned long long kRetryMs = 1000;
    static constexpr unsigned long long kNudgeFailLogMs = 15000;
    bool m_nudgeFailed = false;
    unsigned long long m_nudgeFailLoggedAt = 0;

    const void* blockFor(const std::string& name, std::size_t entry) const;

    static constexpr int kMinAlpha = 10;
    static constexpr int kMaxAlpha = 100;
    static constexpr const char* kGhostBlock = "minecraft:light_blue_stained_glass";

    static constexpr const char* kPokeBlock = "minecraft:structure_void";

    static constexpr const char* kBoxBlock = "minecraft:stone";

    void resolvePalette();

    std::atomic<bool> m_drawPending{false};

    std::atomic<bool> m_reloadPending{false};
    std::atomic<bool> m_redrawPending{false};
    std::atomic<bool> m_clearRequest{false};

    std::atomic<bool> m_clearPending{false};

    std::vector<blockwrite::BlockPos> m_placed;

    std::vector<std::pair<blockwrite::BlockPos, const void*>> m_actorCells;

    void placeGhostBlockEntities();
    void restoreGhostBlockEntities();

    enum class ActorPlace {
        Placed,
        NoGhost,
        Refused,
        Leftover,
        Already,
        Learned,
    };

    ActorPlace placeOneGhostActor(void* region, const blockwrite::BlockPos& where,
                                  const void* block);

    void restoreFreedActors(
        const std::vector<std::pair<blockwrite::BlockPos, const void*>>& cells);

    std::size_t forgetPlacedActors(
        const std::vector<std::pair<blockwrite::BlockPos, const void*>>& cells);

    struct PlacedActor {
        blockwrite::BlockPos at;
        const void* block = nullptr;
    };
    std::vector<PlacedActor> m_actorPlaced;

    void* m_actorRegion = nullptr;

    std::uint64_t m_regionGeneration = 0;

    unsigned long long m_worldSettleUntil = 0;

    static constexpr unsigned long long kWorldSettleMs = 4000;

    std::atomic<bool> m_actorPending{false};
    std::size_t m_clearedUpTo = 0;

    const void* m_air = nullptr;

    std::atomic<int> m_alpha{45};

    std::atomic<bool> m_diffBoxes{true};
    std::atomic<bool> m_diffXray{false};
    std::atomic<bool> m_ghostOverMismatch{true};
    std::atomic<int> m_boxAlpha{35};
    std::atomic<bool> m_boxModeChanged{false};
    const void* m_ghost = nullptr;
    const void* m_poke = nullptr;

    std::size_t m_drawnUpTo = 0;

    std::size_t m_drawnPlaced = 0;

    bool m_waitedLogged = false;
    bool m_firstLogged = false;
    bool m_anchorLogged = false;

    bool m_reloadAsked = false;

    static constexpr int kBoxReloadTries = 4;
    static constexpr unsigned long long kBoxReloadRetryMs = 3000;
    int m_boxReloadTries = 0;
    unsigned long long m_boxReloadAt = 0;

    static constexpr unsigned long long kBoxesPlacedWaitMs = 5000;
    unsigned long long m_boxesPlacedAt = 0;
    bool m_boxesPlacedLogged = false;
    bool m_boxesGaveUpLogged = false;

    static constexpr unsigned long long kBoxModeReloadSlowMs = 5000;
    std::atomic<bool> m_boxReloadWanted{false};
    std::atomic<unsigned long long> m_boxReloadWantedAt{0};
    bool m_boxReloadSlowLogged = false;

    std::atomic<bool> m_shuttingDown{false};

    std::int32_t m_anchorX = 0;
    std::int32_t m_anchorY = 0;
    std::int32_t m_anchorZ = 0;

    static constexpr std::size_t kPerFrame = 256;

    std::vector<std::wstring> m_files;
    std::vector<std::wstring> m_fileNames;

    int m_selected = -1;

    std::string m_pendingFile;

    mutable std::mutex m_mutex;
    structure::Structure m_loaded;

    blocks::Table m_palette;

    std::vector<const void*> m_paletteBlocks;

    std::atomic<int> m_posX{0};
    std::atomic<int> m_posY{0};
    std::atomic<int> m_posZ{0};

    std::atomic<unsigned long long> m_posChangedAt{0};
    static constexpr unsigned long long kPosSettleMs = 800;

    static constexpr unsigned long long kModelSettleMs = 700;

    unsigned long long m_modelRedrawnAt = 0;

    static constexpr int kMinXZ = -30000000;
    static constexpr int kMaxXZ = 30000000;
    static constexpr int kMaxLayerOffsetY = 1024;
    static constexpr int kMinY = -64;
    static constexpr int kMaxY = 319;
};

}
