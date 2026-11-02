#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "game/BlockRegistry.h"
#include "game/AreaSelection.h"
#include "game/MaterialRules.h"
#include "game/PickItem.h"
#include "game/FlexibleLogic.h"
#include "game/PlacementQuery.h"
#include "game/SchematicSections.h"
#include "game/SchematicWorld.h"
#include "game/SchematicCells.h"
#include "game/Structure.h"
#include "game/PasteCommands.h"
#include "game/SchematicPlacements.h"
#include "game/LevelDbIndex.h"
#include "input/Hotkey.h"
#include "modules/Module.h"
#include "render/DebugLine.h"

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

    int ghostOpacity() const { return m_alpha.load(std::memory_order_relaxed); }

    void shutdown() override;

    void onPlayerViewUpdate();
    bool onBuildBlock(void* gameMode, void* blockPos, unsigned char face, unsigned char extra,
                      bool simTick, bool& handled);
    bool onUseItem(void* gameMode, bool& handled);

    void pressPaste(std::uint64_t placement);
    bool pasteArmed(std::uint64_t placement) const;
    void stopPaste();
    bool pasting() const { return m_pasteBusy.load(); }
    std::string pasteStatus() const;
    std::uint64_t pasteRevision() const { return m_pasteRevision.load(); }

    selection::Area areaSelection() const;
    void editSelection(const std::function<void(selection::Area&)>& change);
    void selectionAction(int action);
    std::wstring selectionFileName() const;
    void setSelectionFileName(std::wstring name);
    std::string selectionSaveStatus() const;
    std::uint64_t selectionRevision() const { return m_areaRevision.load(); }

protected:
    Schematica() = default;

    void onEnabledChanged(bool enabled) override;

    void onUpdate() override;

private:
    struct PasteJob {
        schematic::PlacementSpec spec;
        schematic::Placement source;
        std::shared_ptr<const schematic::RenderSnapshot> snapshot;
        std::shared_ptr<schematic::CellSet> cells;
        schematic::LayerFilter layer;
        paste::Replace replace = paste::Replace::None;
        bool visibleOnly = false, mute = true, muted = false, sending = false;
        std::uint64_t worldGeneration = 0, worldEntry = 0;
        std::size_t section = 0, missing = 0, water = 0, sent = 0, blocks = 0;
        unsigned long long sentAt = 0;
        std::unordered_map<const void*, std::string> arguments;
        std::vector<paste::Cell> runs;
        std::vector<paste::Command> commands;
        std::string error;
    };
    void startPaste(std::uint64_t placement);
    void stepPaste();
    void finishPaste(const std::string& reason);
    void setPasteStatus(std::string status);
    void preparePaste(PasteJob& job);
    std::unique_ptr<PasteJob> m_pasteJob;
    std::unique_ptr<PasteJob> m_pasteWork, m_pasteDone;
    std::atomic<bool> m_pasteBusy{false}, m_pasteStop{false}, m_pasteCancel{false};
    std::atomic<std::uint64_t> m_pasteRequest{0}, m_pasteRevision{0};
    std::atomic<int> m_pasteReplace{0}, m_pastePerTick{8};
    std::atomic<bool> m_pasteMute{true}, m_pasteVisibleOnly{false};
    mutable std::mutex m_pasteMutex;
    std::string m_pasteStatus;
    std::uint64_t m_pasteArmedId = 0;
    unsigned long long m_pasteArmedAt = 0, m_pasteRestAt = 0;

    void runImport(void* ownerWindow);

    static unsigned long __stdcall importThreadMain(void* param);

    void startImport();

    void finishImport();

    std::atomic<bool> m_importBusy{false};
    std::atomic<bool> m_importDone{false};
    std::mutex m_importMutex;
    std::wstring m_importedStem;
    void* m_importThread = nullptr;
    bool m_importStop = false;
    void stopImport();

    struct SchematicFile {
        std::wstring name, fileName;
        std::shared_ptr<const structure::Structure> loaded;
        bool ready = false;
        std::uint64_t generation = 0;
        std::atomic<int> sizeX{0}, sizeY{0}, sizeZ{0};
        std::atomic<std::size_t> solidCount{0};
        std::vector<std::size_t> paletteCounts;
    };
    struct PlacementEntry { schematic::PlacementSpec spec; };

public:
    void refreshFiles();
    std::size_t fileCount() const;
    std::wstring fileName(std::size_t at) const;
    bool fileInfo(std::size_t at, int& sx, int& sy, int& sz, std::size_t& solid) const;
    bool loadFileAsPlacement(std::size_t at);
    bool deleteFile(std::size_t at);
    std::size_t placementCount() const;
    std::uint64_t placementId(std::size_t at) const;
    bool placementSpec(std::uint64_t id, schematic::PlacementSpec& out) const;
    bool setPlacementName(std::uint64_t id, std::wstring name);
    bool setPlacementEnabled(std::uint64_t id, bool value);
    bool setPlacementRender(std::uint64_t id, bool value);
    bool setPlacementLocked(std::uint64_t id, bool value);
    bool setPlacementPos(std::uint64_t id, int axis, int value);
    bool setPlacementRotation(std::uint64_t id, int quarters);
    bool setPlacementMirror(std::uint64_t id, schematic::Mirror mirror);
    bool cyclePlacementColor(std::uint64_t id);
    bool setPlacementGrid(std::uint64_t id, const schematic::GridSpec& grid);
    bool movePlacementToPlayer(std::uint64_t id);
    bool duplicatePlacement(std::uint64_t id);
    bool removePlacement(std::uint64_t id);
    std::uint64_t selectedPlacement() const { return m_selectedId.load(); }
    bool selectPlacement(std::uint64_t id);
    bool inWorld() const { return m_inWorld.load(); }
    bool placementsSaved() const { return m_hasWorldKey.load(); }
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

    struct MaterialRow {
        std::wstring name;
        std::size_t count = 0;
        std::size_t missing = 0;
        std::size_t available = 0;
        bool missingKnown = false;
        bool availableKnown = false;
        bool ignored = false;
        bool noItem = false;
        std::int32_t iconIdAux = 0;
        bool hasIcon = false;
        int stackSize = 0;
    };
    std::vector<MaterialRow> placementMaterials(std::uint64_t id, std::size_t limit,
                                                std::size_t* kinds = nullptr, bool unfiltered = false) const;

    enum : int {
        kMaterialAll = 0,
        kMaterialMissing = 1,
        kMaterialListTypeCount = 2,
    };
    int materialListType() const { return m_materialListType.load(std::memory_order_relaxed); }
    void cycleMaterialListType();
    bool materialScopeAll() const { return m_materialAll.load(); }
    void setMaterialScopeAll(bool all) { m_materialAll.store(all); }
    int materialMultiplier() const { return m_materialMultiplier.load(); }
    void setMaterialMultiplier(int value);
    bool materialHideAvailable() const { return m_materialHideAvailable.load(); }
    void toggleMaterialHideAvailable() { m_materialHideAvailable.store(!m_materialHideAvailable.load()); }
    int materialSort() const { return m_materialSort.load(); }
    bool materialSortDescending() const { return m_materialSortDescending.load(); }
    void setMaterialSort(int column);
    std::pair<std::size_t, std::size_t> materialItemProgress() const;
    std::uint64_t materialRevision() const { return m_materialRevision.load(); }
    void toggleMaterialIgnored(const std::wstring& name);
    void clearIgnoredMaterials();
    std::size_t ignoredMaterialCount() const;
    void askMaterialRefresh() { m_materialRefreshWanted.store(true, std::memory_order_relaxed); }
    bool writeMaterialList(std::wstring& outPath);

    bool materialAutoCollect() const;
    void toggleMaterialAutoCollect();
    void queueChat(std::string message);

    bool verifierScopeAll() const { return m_verifierAll.load(); }
    void setVerifierScopeAll(bool all);
    void refreshVerifier();
    void selectVerifierKind(schematic::DiffKind kind);
    bool selectVerifierPair(std::size_t at, std::uint64_t serial);
    bool ignoreVerifierPair(std::size_t at, std::uint64_t serial);
    void clearVerifierSelection();
    void clearVerifierIgnored();
    std::size_t verifierIgnoredCount() const;
    schematicworld::VerifierSelection verifierSelection() const;
    std::shared_ptr<const schematicworld::VerifierReport> verifierReport() const;

    std::atomic<unsigned long long> m_deleteArmedAt{0};
    std::wstring m_deleteArmedName;
    std::atomic<unsigned long long> m_deleteRestAt{0};
    bool deleteArmed() const;
    static constexpr unsigned long long kDeleteArmMs = 5000;
    static constexpr unsigned long long kDeleteMinMs = 600;
    static constexpr unsigned long long kDeleteRestMs = 1500;

private:
    std::vector<std::unique_ptr<SchematicFile>> m_files;
    std::vector<std::unique_ptr<PlacementEntry>> m_placements;
    std::atomic<std::uint64_t> m_selectedId{0};
    std::uint64_t m_nextId = 1;
    nlohmann::json m_legacyBlueprints = nlohmann::json::array();
    bool m_placementsMigrated = false, m_migrationPending = false;
    bool m_worldWriteBlocked = false;
    std::string m_worldKey, m_worldKind, m_worldLevelId, m_worldName;
    std::uint64_t m_worldEntry = 0, m_placementsRevision = 0;
    unsigned long long m_identityResolveAt = 0;
    unsigned long long m_placementsDirtyAt = 0;
    unsigned long long m_worldSaveAttemptAt = 0;
    unsigned m_prepareLogCount = 0;
    std::atomic<bool> m_inWorld{false}, m_hasWorldKey{false}, m_outlinesDirty{true};
    std::atomic<bool> m_placementOutlines{true};
    std::atomic<unsigned long long> m_lockedNoticeAt{0};
    std::atomic<bool> m_lockedNoticePending{false};
    std::atomic<bool> m_selectionNoticePending{false};
    std::mutex m_worldMutex;
    leveldb::WorldIndex m_worldIndex;
    Hotkey m_placementKeys[6];
    static constexpr const char* kPlacementKeyNames[6] = {
        "nudgePlusKeys", "nudgeMinusKeys", "rotateKeys", "mirrorKeys", "moveToPlayerKeys", "selectNextKeys"};
    std::atomic<std::uint64_t> m_moveToPlayerId{0}, m_loadFileGeneration{0};
    std::map<std::uint64_t,schematic::CopyRange> m_rangeCopies;
    schematic::Box m_copyRange;
    bool m_copyRangeValid = false;
    int m_copyCamX = 0, m_copyCamZ = 0, m_copyRadius = 0;
    bool mutatePlacement(std::uint64_t id, bool geometry, unsigned purpose,
                         const std::function<void(schematic::PlacementSpec&)>& change);
    void placementChanged(unsigned purpose);
    bool rejectLocked(const schematic::PlacementSpec& spec);
    int unusedColor() const;
    void syncWorld();
    void leaveWorld();
    bool saveWorldLocked();
    void saveWorld(bool force);
    void syncPlacementRange();
    void updateOutlines();
    struct OutlineJob {
        struct Entry { schematic::Box box; schematic::CopyRange copies; float color[4]{}; float width=1; };
        std::uint64_t revision=0;
        std::vector<Entry> entries;
        std::vector<worldmesh::DebugLine> lines;
        bool failed=false;
    };
    void runOutlineJob(OutlineJob& job);
    std::atomic<std::uint64_t> m_outlineRevision{0};
    std::unique_ptr<OutlineJob> m_outlineWork, m_outlineDone;

    std::shared_ptr<const schematic::CellSet> m_cellSet = std::make_shared<schematic::CellSet>();
    std::shared_ptr<const schematic::CellSet> m_cellTrash;

    enum PreparePurpose : unsigned {
        kPrepReload = 1u,
        kPrepRedraw = 2u,
    };
    struct PrepEntry {
        std::uint64_t placementId = 0;
        int owner = -1;
        schematic::PlacementSpec spec;
        bool loadOnly = false;
        std::uint64_t generation = 0;
        std::filesystem::path path;
        std::shared_ptr<const structure::Structure> data;
        int x = 0;
        int y = 0;
        int z = 0;
        int rotation = 0, mirror = 0;
        bool loadedNow = false;
        schematic::Loaded result;
    };
    struct PrepareJob {
        unsigned purposes = 0;
        std::uint64_t revision = 0;
        schematic::Box range;
        std::vector<PrepEntry> entries;
        std::vector<schematic::LayoutKey> prevKey;
        bool prevHasCells = false;
        std::vector<schematic::Placement> placements;
        std::vector<schematic::LayoutKey> key;
        std::unique_ptr<schematic::CellSet> cells;
        bool cancelled = false;
        const char* why = nullptr;
        unsigned long long loadMs = 0;
        unsigned long long cellsMs = 0;
        unsigned long long startedAt = 0;
    };
    void requestPrepare(unsigned purposes);
    void pollPrepare();
    void startPrepare();
    std::vector<PrepEntry> snapshotVisible(bool& needLoad);
    static std::vector<schematic::Placement> placementsOf(const std::vector<PrepEntry>& entries, const schematic::Box& range);
    void installPrepared(std::unique_ptr<PrepareJob> job);
    void installCells(std::unique_ptr<schematic::CellSet> cells);
    void finishPrepared(unsigned purposes);
    static unsigned long __stdcall workerThreadMain(void* param);
    void workerMain();
    struct SelectionSaveJob {
        selection::Area area;
        structure::Structure data;
        std::int32_t lo[3]{}, hi[3]{}, origin[3]{};
        int sectionLo[3]{}, sectionHi[3]{}, section[3]{};
        unsigned cell = 0;
        std::size_t sectionsDone = 0, sectionsTotal = 0;
        unsigned progressLogged = 0;
        std::uint64_t worldGeneration = 0;
        std::filesystem::path path;
        std::unordered_map<const void*,std::uint16_t> palette;
        std::string status;
        bool allocate = true, ok = false;
    };
    void runSelectionSave(SelectionSaveJob& job);
    void stepSelection();
    void startSelectionSave();
    void updateSelectionLines();
    bool saveSelectionLocked(bool force = true);
    void loadSelectionLocked(bool keepCurrent);
    mutable std::mutex m_areaMutex;
    selection::Area m_area;
    bool m_areaDirty = false, m_areaWriteBlocked = false;
    unsigned long long m_areaDirtyAt = 0, m_areaSaveAttemptAt = 0;
    std::wstring m_areaFileName = L"Unnamed";
    std::string m_areaSaveStatus;
    std::atomic<std::uint64_t> m_areaRevision{1};
    std::uint64_t m_areaLinesRevision = 0;
    bool m_areaLinesEnabled = false;
    std::atomic<unsigned> m_areaActions{0};
    Hotkey m_areaKeys[4];
    static constexpr const char* kAreaKeyNames[4] = {"corner1Keys","corner2Keys","addBoxKeys","nextBoxKeys"};
    std::unique_ptr<SelectionSaveJob> m_saveRead;
    std::unique_ptr<SelectionSaveJob> m_saveWork, m_saveDone;
    std::vector<std::unique_ptr<SelectionSaveJob>> m_saveTrash;
    bool m_saveBusy = false;
    void runPrepareJob(PrepareJob& job);
    void postJob(std::unique_ptr<PrepareJob> job);
    void postTrash(std::shared_ptr<const schematic::CellSet> trash);
    bool ensureWorker();
    void stopWorker();

    std::vector<schematic::Placement> m_live;
    std::vector<std::pair<std::uint64_t, std::size_t>> m_materialOwners;
    std::map<std::uint64_t, std::size_t> m_materialBases;
    std::vector<std::vector<materialrules::Item>> m_slotItems;
    std::vector<structure::PaletteEntry> m_slotSources;
    std::size_t m_slotItemsDone = 0;
    std::unordered_map<const void*, pickitem::Result> m_pickedItems;
    bool m_pickWorked = false;
    std::uint64_t m_pickEmptySince = 0;
    struct PlacementInfo { std::string item, block; materialrules::States states; };
    std::unordered_map<const void*, PlacementInfo> m_placementInfo;
    std::atomic<bool> m_easyPlace{false}, m_placementRestriction{false}, m_pickRequested{false};
    std::atomic<bool> m_easyPlaceRotate{true}, m_easyPlaceHold{true};
    std::atomic<bool> m_easyAutoSneak{true};
    bool m_easySneakAsked = false;
    bool m_easySneakSeen = false;
    bool m_easySneakReleased = false;
    std::uint64_t m_easySneakAskedInputs = 0;
    std::uint64_t m_easySneakReleasedInputs = 0;
    std::uint64_t m_easySneakAskedMs = 0;
    static constexpr std::uint64_t kEasySneakWaitInputs = 20, kEasySneakStallMs = 2000;
    static constexpr std::uint64_t kEasySneakAfterInputs = 0;
    void stopEasySneak();
    bool autoSneakUsable() const;
    void keepEasySneak();
    bool easySneakSettling(void* gameMode);
    void stepEasySneak();
    int m_useButton = -1;
    std::uint64_t m_easyPressSeq = 0, m_easyTargetAt = 0;
    bool m_easyPressSeen = false, m_easyTargetSeen = false;
    std::int32_t m_easyTarget[3]{};
    std::uint64_t m_easyPickSettleUntil = 0;
    std::uint64_t m_easyPickInputs = 0;
    static constexpr std::uint64_t kEasyPickSettleMs = 200;
    std::uint64_t m_easyPickAt = 0;
    bool easyPickSettling();
    struct EasyRetry { bool active = false; std::int32_t pos[3]{}; const void* want = nullptr; std::uint64_t at = 0; int tries = 0; };
    EasyRetry m_easyRetry;
    bool m_inEasyRetry = false;
    int m_easyRetryTries = 0;
    static constexpr std::uint64_t kEasyRetryMs = 1500;
    void queueEasyRetry(const schematicworld::SchematicHit& hit);
    void stepEasyRetry();
    bool easyPlaceAt(void* gameMode, const schematicworld::SchematicHit& hit, unsigned char extra, bool simTick,
                     bool viewContext);
    void stepEasyHold();
    struct PlaceProfile {
        std::unordered_map<const void*, materialrules::States> states;
        std::set<std::string> varying;
        std::vector<const void*> answers;
        std::vector<std::vector<const void*>> spotAnswers;
        bool multiSupport = false;
        unsigned coveredFaces = 0;
        int learnings = 0;
        bool thin() const { return answers.size() < 2; }
    };
    static constexpr int kProfileRelearnLimit = 8;
    mutable std::mutex m_profileMutex;
    std::unordered_map<const void*, std::shared_ptr<const PlaceProfile>> m_placeProfiles;
    placementquery::HeldBlock m_profileWanted;
    std::int32_t m_profileWantedAt[3]{};
    std::uint64_t m_profileStepAt = 0;
    std::shared_ptr<const PlaceProfile> placeProfile(const void* type) const;
    bool profileNeedsLearning(const std::shared_ptr<const PlaceProfile>& profile, const std::int32_t target[3]) const;
    static bool placementAccepted(const PlaceProfile* profile, const std::set<std::string>& names, const void* predicted,
                                  const void* want, const materialrules::States& wantStates);
    std::shared_ptr<const PlaceProfile> buildPlaceProfile(const placementquery::HeldBlock& held, const std::int32_t target[3]);
    void stepEasyProfile();
    struct EasyWatch {
        std::int32_t pos[3]{};
        const void* want = nullptr;
        const void* type = nullptr;
        const void* clickedFrom = nullptr;
        std::uint64_t since = 0, clickAt = 0;
        int clicks = 0;
        bool blind = false;
    };
    std::mutex m_watchMutex;
    std::vector<EasyWatch> m_easyWatches;
    void watchPlacement(const std::int32_t pos[3], const void* want, const void* type);
    static int safeUseFace(const std::int32_t pos[3]);
    struct ClickPlan {
        int clicks = 0;
        int differs = 0;
        int face = -1;
        bool computed = false;
        bool turned = false;
        bool blind = false;
        flexible::Look look;
    };
    ClickPlan planClicks(void* gameMode, const std::int32_t pos[3], const materialrules::States& want);
    std::atomic<bool> m_dryRunBroken{false};
    void stepEasyWatch();
    int slotAt(const std::int32_t pos[3], std::size_t& slot) const;
    const void* wantAt(const std::int32_t pos[3]) const;
    static constexpr float kEasyReach = 5.0F;
    std::atomic<unsigned> m_pickSlots{0x1FF};
    Hotkey m_easyToggleKey, m_pickBlockKey;
    mutable std::mutex m_placeNoticeMutex;
    std::set<std::string> m_placeNotices;
    std::vector<std::string> m_placeMessages;
    unsigned long long m_restrictedAt = 0;
    void placeNotice(const std::string& key, const std::string& message);
    void stepPickBlock();
    bool restrictionItemAt(const std::int32_t pos[3], std::string& item) const;
    bool nearPlacement(const std::int32_t pos[3], int margin) const;
    bool placementBlocked(const std::int32_t target[3]) const;
    static constexpr int kRestrictionMargin = 2;
    std::atomic<std::uint64_t> m_materialRevision{0};
    std::atomic<bool> m_ghostWanted{false};
    bool m_prepHasVisible = false;
    unsigned m_prepWant = 0;
    bool m_prepBusy = false;
    bool m_paletteStale = false;

    std::mutex m_workMutex;
    std::condition_variable m_workCv;
    std::unique_ptr<PrepareJob> m_workJob;
    std::unique_ptr<PrepareJob> m_workDone;
    std::vector<std::shared_ptr<const schematic::CellSet>> m_workTrash;
    bool m_workStop = false;
    std::atomic<bool> m_workCancel{false};
    void* m_workThread = nullptr;

    mutable std::mutex m_filesMutex;

    std::unordered_map<std::string, std::int32_t> m_iconIds;

    std::unordered_map<std::string, int> m_stackSizes;

    Hotkey m_pageKey;
    mutable std::mutex m_keysMutex;
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
    static constexpr unsigned long long kLayerTypeSettleMs = 400;
    std::atomic<unsigned long long> m_layerTypedAt{0};
    static int clampLayerValue(int axis, int value);
    void applyLayerFilter();
    schematic::LayerFilter m_layerFilter;
    std::uint64_t m_layerGeneration = 0;
    void syncBoxSections();
    std::unordered_map<schematic::SectionKey, bool, schematic::SectionKeyHash> m_boxSent;
    bool m_boxWasOn = false, m_boxXray = false;
    std::uint64_t m_boxIndexGeneration = 0, m_boxLayerGeneration = 0;
    std::uint64_t m_boxHighlightGeneration = 0;
    mutable std::mutex m_verifierMutex;
    std::atomic<bool> m_verifierAll{false}, m_verifierRefresh{true};
    schematicworld::VerifierSelection m_verifierSelection;
    std::vector<schematicworld::VerifierIgnored> m_verifierIgnored;
    bool m_verifierIgnoredPending = true;
    int m_verifierOwner = -2;
    std::uint64_t m_verifierIndexGeneration = 0;
    void syncVerifier();
    int m_radiusChunks = 18, m_boxRadius = 0, m_boxCamX = 0, m_boxCamZ = 0;
    unsigned long long m_radiusReadAt = 0;
    std::atomic<bool> m_clearRequest{false};

    void scanFiles();

    std::uint64_t m_nextFileGeneration = 0;
    std::map<std::string, std::size_t> m_availableByName;
    bool m_availableKnown = false;
    std::atomic<bool> m_materialRefreshWanted{false};
    unsigned long long m_availableAt = 0;
    std::atomic<int> m_materialListType{kMaterialAll};
    std::atomic<bool> m_materialAll{false}, m_materialHideAvailable{false};
    std::atomic<int> m_materialMultiplier{1}, m_materialSort{0};
    std::atomic<bool> m_materialSortDescending{false};
    std::set<std::string> m_ignoredMaterials;
    void refreshAvailableCounts();
    void resolveMaterialItems();

public:
private:

    static constexpr int kMinAlpha = 10;
    static constexpr int kMaxAlpha = 100;

    void resolvePalette(const std::vector<schematic::Placement>& live);

    std::atomic<bool> m_reloadPending{false};
    std::atomic<bool> m_redrawPending{false};

    std::uint64_t m_regionGeneration = 0;

    unsigned long long m_worldSettleUntil = 0;

    static constexpr unsigned long long kWorldSettleMs = 4000;

    const void* m_air = nullptr;

    std::atomic<int> m_alpha{45};

    std::atomic<bool> m_diffBoxes{true};
    std::atomic<bool> m_diffXray{false};
    std::atomic<bool> m_ghostOverMismatch{true};
    std::atomic<int> m_boxAlpha{35};

    std::atomic<bool> m_shuttingDown{false};

    std::atomic<bool> m_enabledAtShutdown{false};

    std::vector<std::wstring> m_fileStems;
    std::vector<std::wstring> m_fileNames;

    mutable std::mutex m_mutex;

    blocks::Table m_palette;

    std::shared_ptr<const std::vector<const void*>> m_paletteBlocks;

    std::atomic<unsigned long long> m_posChangedAt{0};
    static constexpr unsigned long long kPosSettleMs = 800;

    static constexpr int kMinXZ = -30000000;
    static constexpr int kMaxXZ = 30000000;
    static constexpr int kMaxLayerOffsetY = 1024;
    static constexpr int kMinY = -64;
    static constexpr int kMaxY = 319;
};

}
