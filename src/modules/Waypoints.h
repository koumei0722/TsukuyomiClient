#pragma once

#include "modules/Module.h"
#include "game/WaypointLogic.h"
#include "game/LevelDbIndex.h"

#include <mutex>
#include <map>

namespace tsukuyomi {

class Waypoints : public Module {
public:
    static Waypoints& instance();
    static constexpr int kPageTab = 100;
    const wchar_t* name() const override { return L"Waypoints"; }
    bool available() const override;
    void onScansReady() override;
    void loadConfig(const nlohmann::json& section) override;
    void saveConfig(nlohmann::json& section) const override;
    MenuItem buildMenu() override;
    void onPlayerViewUpdate();
    void shutdown() override;
    void pageAction(int op);
    struct PageRow { std::string label; bool visible; };
    std::vector<PageRow> pageRows();
    std::uint32_t draftColor();
    std::string currentDimension();
    bool hasWorld();

protected:
    void onUpdate() override;
    void onEnabledChanged(bool value) override;

private:
    Waypoints() = default;
    void syncWorld();
    bool saveWorldLocked();
    bool writeWorldLocked(const std::string& key, const std::vector<waypoints::Waypoint>& list, bool blocked);
    void leaveWorldLocked();
    void publishTagsLocked();
    std::vector<std::size_t> dimensionRowsLocked() const;
    void changedLocked();
    mutable std::mutex m_mutex;
    Hotkey m_addKey;
    std::atomic<bool> m_openRequested{false};
    bool m_showDistance = true, m_stopping = false;
    static constexpr float kDefaultLabelSize = 1.5F;
    float m_labelSize = kDefaultLabelSize;
    waypoints::Waypoint m_draft;
    std::string m_lastAddedName;
    std::vector<waypoints::Waypoint> m_list;
    struct Unsaved { std::vector<waypoints::Waypoint> list; bool blocked = false; };
    std::map<std::string, Unsaved> m_unsaved;
    std::string m_worldKey, m_dimension;
    bool m_inWorld = false, m_integrated = false, m_worldWriteBlocked = false;
    std::uint64_t m_worldEntry = 0, m_identityResolveAt = 0, m_dirtyAt = 0, m_saveAttemptAt = 0;
    std::uint64_t m_revision = 1, m_publishedRevision = 0;
    leveldb::WorldIndex m_worldIndex;
    float m_playerPos[3]{};
};
}
