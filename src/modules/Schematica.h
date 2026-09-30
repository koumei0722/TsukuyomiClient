#pragma once

#include <array>
#include <atomic>
#include <climits>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "game/BlockRegistry.h"
#include "game/BlockWrite.h"
#include "game/SchematicCells.h"
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

    bool wantsGhostHooks() const
    {
        return enabled() && m_ghostWanted.load(std::memory_order_relaxed);
    }

    void shutdown() override;

    void onPlayerViewUpdate();

    void noteGhostCellFreed(std::int32_t x, std::int32_t y, std::int32_t z);

protected:
    Schematica() = default;

    void onEnabledChanged(bool enabled) override;

    void onUpdate() override;

    bool persistEnabled() const override { return true; }

private:

    void runImport(void* ownerWindow);

    static unsigned long __stdcall importThreadMain(void* param);

    void startImport();

    void finishImport();

    std::atomic<bool> m_importBusy{false};
    std::atomic<bool> m_importDone{false};
    std::mutex m_importMutex;
    std::wstring m_importedStem;

    struct Blueprint {
        std::wstring name;
        std::wstring fileName;

        std::atomic<bool> visible{false};
        std::atomic<int> posX{0};
        std::atomic<int> posY{0};
        std::atomic<int> posZ{0};
        std::atomic<int> rotation{0};

        std::shared_ptr<const structure::Structure> loaded;
        bool ready = false;

        std::atomic<int> sizeX{0};
        std::atomic<int> sizeY{0};
        std::atomic<int> sizeZ{0};
        std::atomic<std::size_t> solidCount{0};
        std::vector<std::pair<std::string, std::size_t>> materials;
    };

    using Cell = schematic::Cell;
    static constexpr std::size_t kAirCell = schematic::kAirCell;
    using Box = schematic::Box;

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
    void setLayerMode(int mode);
    void setLayerAxis(int axis);

    void stepLayer(int delta);
    void setLayerHere();

    bool deleteBlueprint(std::size_t at);

    bool blueprintInfo(std::size_t at, int& sizeX, int& sizeY, int& sizeZ,
                       std::size_t& solid) const;

    struct MaterialRow {
        std::wstring name;
        std::size_t count = 0;
        std::size_t missing = 0;
        std::size_t available = 0;
        bool missingKnown = false;
        bool availableKnown = false;
        bool ignored = false;
        std::int32_t iconIdAux = 0;
        bool hasIcon = false;
        int stackSize = 0;
    };
    std::vector<MaterialRow> blueprintMaterials(std::size_t at, std::size_t limit,
                                                std::size_t* kinds = nullptr) const;

    enum : int {
        kMaterialAll = 0,
        kMaterialMissing = 1,
        kMaterialListTypeCount = 2,
    };
    int materialListType() const { return m_materialListType.load(std::memory_order_relaxed); }
    void cycleMaterialListType();
    void toggleMaterialIgnored(const std::wstring& name);
    void clearIgnoredMaterials();
    std::size_t ignoredMaterialCount() const;
    void askMaterialRefresh() { m_materialRefreshWanted.store(true, std::memory_order_relaxed); }
    bool writeMaterialList(std::size_t at, std::wstring& outPath);

    void diffTally(std::size_t (&out)[blocks::kDiffKindCount]) const;

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
    schematic::CellIndex m_cellAt;
    std::vector<Box> m_boxes;
    Box m_region;
    std::vector<std::shared_ptr<const structure::Structure>> m_cellsKeep;
    std::int32_t m_regionSizeX = 0;
    std::int32_t m_regionSizeY = 0;
    std::int32_t m_regionSizeZ = 0;

    std::atomic<bool> m_cellsDirty{true};

    enum PreparePurpose : unsigned {
        kPrepReload = 1u,
        kPrepRedraw = 2u,
        kPrepCells = 4u,
    };
    struct PrepEntry {
        std::wstring name;
        std::filesystem::path path;
        std::shared_ptr<const structure::Structure> data;
        int x = 0;
        int y = 0;
        int z = 0;
        int rotation = 0;
        bool loadedNow = false;
        schematic::Loaded result;
    };
    struct PrepareJob {
        unsigned purposes = 0;
        std::vector<PrepEntry> entries;
        std::vector<schematic::LayoutKey> prevKey;
        bool prevHasCells = false;
        std::vector<schematic::Placement> placements;
        std::vector<schematic::LayoutKey> key;
        std::unique_ptr<schematic::CellSet> cells;
        bool cancelled = false;
        unsigned long long loadMs = 0;
        unsigned long long cellsMs = 0;
        unsigned long long startedAt = 0;
    };
    void requestPrepare(unsigned purposes);
    void pollPrepare();
    void startPrepare();
    std::vector<PrepEntry> snapshotVisible(bool& needLoad);
    static std::vector<schematic::Placement> placementsOf(const std::vector<PrepEntry>& entries);
    void installPrepared(std::unique_ptr<PrepareJob> job);
    void installCells(std::unique_ptr<schematic::CellSet> cells);
    void finishPrepared(unsigned purposes);
    static unsigned long __stdcall workerThreadMain(void* param);
    void workerMain();
    void runPrepareJob(PrepareJob& job);
    void postJob(std::unique_ptr<PrepareJob> job);
    void postTrash(std::unique_ptr<schematic::CellSet> trash);
    bool ensureWorker();
    void stopWorker();

    std::vector<schematic::Placement> m_live;
    std::atomic<bool> m_ghostWanted{false};
    bool m_prepHasVisible = false;
    unsigned m_prepWant = 0;
    bool m_prepBusy = false;
    bool m_paletteStale = false;

    std::mutex m_workMutex;
    std::condition_variable m_workCv;
    std::unique_ptr<PrepareJob> m_workJob;
    std::unique_ptr<PrepareJob> m_workDone;
    std::vector<std::unique_ptr<schematic::CellSet>> m_workTrash;
    bool m_workStop = false;
    std::atomic<bool> m_workCancel{false};
    void* m_workThread = nullptr;

    std::vector<schematic::LayoutKey> m_cellsKey;
    static std::uint64_t packCell(std::int32_t x, std::int32_t y, std::int32_t z)
    {
        return schematic::packCell(x, y, z);
    }

    mutable std::mutex m_filesMutex;

    std::unordered_map<std::string, std::int32_t> m_iconIds;

    std::unordered_map<std::string, int> m_stackSizes;

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
    static constexpr unsigned long long kLayerCheckMs = 8000;
    static constexpr unsigned kLayerAskTries = 3;
    std::vector<std::array<std::int32_t, 3>> m_layerAskChunks;
    std::uint64_t m_layerAskSeq = 0;
    unsigned long long m_layerAskAt = 0;
    unsigned m_layerAskTries = 0;
    unsigned m_layerAskRound = 0;
    void checkLayerRebuilds(unsigned long long now);
    static int clampLayerValue(int axis, int value);
    void applyLayerFilter();

    void scanFiles();

    void clearStep();

    void pruneStep();

    static constexpr int kPruneDelayFrames = 3;
    std::atomic<bool> m_prunePending{false};
    int m_pruneDelay = 0;

    bool drawCell(std::size_t at);

    static constexpr unsigned kDrawBudgetMs = 8;
    static constexpr std::size_t kDrawBatch = 512;

    unsigned long long m_loadStartedAt = 0;

    void diffStep();

    static constexpr std::size_t kDiffPerFrame = 256;

    std::size_t m_diffUpTo = 0;

    std::size_t m_diffTally[blocks::kDiffKindCount] = {};
    std::size_t m_diffTallyDone[blocks::kDiffKindCount] = {};
    std::atomic<std::size_t> m_diffTallyLaps{0};
    void closeDiffTally();

    std::vector<std::string> m_paletteNames;
    std::vector<const void*> m_paletteLegacy;
    std::vector<std::size_t> m_missingByEntry;
    std::vector<std::size_t> m_missingByEntryDone;
    std::vector<std::string> m_missingNamesDone;
    std::map<std::string, std::size_t> m_availableByName;
    bool m_availableKnown = false;
    std::atomic<bool> m_materialRefreshWanted{false};
    unsigned long long m_availableAt = 0;
    std::atomic<int> m_materialListType{kMaterialAll};
    std::set<std::string> m_ignoredMaterials;
    void refreshAvailableCounts();

    std::size_t m_diffWater = 0;
    std::size_t m_diffWaterUnknown = 0;
    std::size_t m_diffWorldWater = 0;
    std::size_t m_diffWantWater = 0;

    std::size_t m_diffDropped = 0;

    std::size_t m_diffRestored = 0;

    std::size_t m_diffChanged = 0;

    std::unordered_set<std::uint64_t> m_lapTagChunks;
    std::unordered_set<std::uint64_t> m_lapColorChunks;
    void noteLapChunk(std::unordered_set<std::uint64_t>& into, std::int32_t x, std::int32_t y,
                      std::int32_t z);

    blocks::DiffKind diffCell(std::size_t at, bool early);

    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> m_chunkCells;
    std::unordered_map<std::uint64_t, void*> m_learnedKeys;
    std::vector<blockwrite::BlockPos> m_earlyRegion;
    std::int32_t m_earlyRegionKey[6] = {0, 0, 0, 0, 0, 0};
    std::size_t m_earlyCursor = 0;
    std::unordered_set<std::uint64_t> m_earlyOutside;
    std::uint64_t m_earlyRegionGen = 0;
    unsigned long long m_earlySettledAt = 0;
    static constexpr std::size_t kEarlyCellBudget = 8192;

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

    static constexpr std::size_t kBoxLimit = static_cast<std::size_t>(-1);

    unsigned long long m_boxDropLoggedAt = 0;
    static constexpr unsigned long long kBoxDropLogMs = 30000;

    std::size_t m_boxSignature = 0;
    unsigned long long m_boxPublishedAt = 0;
    static constexpr unsigned long long kBoxPublishMs = 500;

    bool m_boxLiveDirty = false;
    unsigned long long m_boxLiveAt = 0;
    static constexpr unsigned long long kBoxLiveMs = 100;

    bool m_boxListCut = false;
    double m_boxListEye[3] = {};
    bool m_boxListEyeValid = false;
    static constexpr double kBoxRecenterBlocks = 16.0;
    static constexpr double kBoxRecenterShare = 0.25;
    bool boxEye(double out[3]);
    double m_boxLastEye[3] = {};
    bool m_boxLastEyeValid = false;

    static constexpr unsigned long long kBoxModeRetryMs = 500;
    unsigned long long m_boxModeRetryAt = 0;

    std::size_t dirtyChunks(bool askAll = true);

    void rebuildGhostChunkList();

    std::vector<blockwrite::BlockPos> regionChunkList() const;

    std::size_t learnRegionSubChunks(bool judgeNew);

    void restoreFreedCells();

    std::mutex m_freedMutex;
    std::vector<blockwrite::BlockPos> m_freedCells;
    std::atomic<bool> m_freedPending{false};

    static constexpr std::size_t kFreedLimit = 4096;

public:
private:
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

    void resolvePalette(const std::vector<schematic::Placement>& live);

    std::atomic<bool> m_drawPending{false};

    std::atomic<bool> m_reloadPending{false};
    std::atomic<bool> m_redrawPending{false};
    std::atomic<bool> m_clearRequest{false};

    std::atomic<bool> m_clearPending{false};

    std::uint64_t m_regionGeneration = 0;

    unsigned long long m_worldSettleUntil = 0;

    static constexpr unsigned long long kWorldSettleMs = 4000;

    std::size_t m_clearedUpTo = 0;

    const void* m_air = nullptr;

    std::atomic<int> m_alpha{45};

    std::atomic<bool> m_diffBoxes{true};
    std::atomic<bool> m_diffXray{false};
    std::atomic<bool> m_ghostOverMismatch{true};
    std::atomic<int> m_boxAlpha{35};
    std::atomic<bool> m_boxModeChanged{false};
    const void* m_poke = nullptr;

    std::size_t m_drawnUpTo = 0;

    std::size_t m_drawnPlaced = 0;

    bool m_waitedLogged = false;
    bool m_firstLogged = false;
    bool m_anchorLogged = false;

    static constexpr unsigned long long kBoxModeReloadSlowMs = 5000;
    std::atomic<bool> m_boxReloadWanted{false};
    std::atomic<unsigned long long> m_boxReloadWantedAt{0};
    bool m_boxReloadSlowLogged = false;

    std::atomic<bool> m_shuttingDown{false};

    std::atomic<bool> m_enabledAtShutdown{false};

    std::int32_t m_anchorX = 0;
    std::int32_t m_anchorY = 0;
    std::int32_t m_anchorZ = 0;

    std::vector<std::wstring> m_files;
    std::vector<std::wstring> m_fileNames;

    std::string m_pendingFile;

    mutable std::mutex m_mutex;

    blocks::Table m_palette;

    std::vector<const void*> m_paletteBlocks;

    std::atomic<int> m_posX{0};
    std::atomic<int> m_posY{0};
    std::atomic<int> m_posZ{0};

    std::atomic<unsigned long long> m_posChangedAt{0};
    static constexpr unsigned long long kPosSettleMs = 800;

    static constexpr int kMinXZ = -30000000;
    static constexpr int kMaxXZ = 30000000;
    static constexpr int kMaxLayerOffsetY = 1024;
    static constexpr int kMinY = -64;
    static constexpr int kMaxY = 319;
};

}
