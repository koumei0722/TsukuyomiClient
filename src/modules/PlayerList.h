#pragma once

#include "modules/Module.h"
#include "game/PlayerListLayout.h"
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <map>
#include <mutex>

namespace tsukuyomi {

class PlayerList : public Module {
public:
    static PlayerList& instance();
    const wchar_t* name() const override { return L"PlayerList"; }
    bool available() const override { return m_definitionRegistered.load(std::memory_order_relaxed); }
    MenuItem buildMenu() override;
    void loadConfig(const nlohmann::json& section) override;
    void saveConfig(nlohmann::json& section) const override;
    void onScansReady() override;
    void shutdown() override;
    void onPlayerViewUpdate();

protected:
    void onUpdate() override;

private:
    PlayerList() = default;
    static void onHudCreated(void* ctrl);
    void publish(bool held, std::uint64_t now);
    void hide();
    struct Sample {
        std::vector<playerlist::Entry> entries;
        std::uint64_t at = 0;
        bool readable = false;
        bool objective = false;
    };
    struct LevelSlots {
        std::int32_t players = 0, scoreboard = 0;
        const void* fetch = nullptr;
    };
    LevelSlots inspectSlots(const void* vtable);
    void prepareFaceDirectory();
    std::string facePathFor(const playerlist::Entry& entry);
    std::filesystem::path m_faceDirectory;
    std::unordered_set<std::string> m_writtenFaces;
    bool m_faceWriteLogged = false;
    std::atomic<const void*> m_healthAttribute{nullptr};
    Hotkey m_listKey{{9}};
    std::atomic<bool> m_definitionRegistered{false}, m_shuttingDown{false}, m_wantSample{false};
    bool m_enabledWasInConfig = false;
    std::mutex m_sampleMutex;
    Sample m_sample;
    std::mutex m_publishMutex;
    std::uint64_t m_lastPublish = 0;
    bool m_visible = false;
    std::map<const void*, LevelSlots> m_slots;
    std::uint64_t m_lastSample = 0;
    bool m_badSlotLogged[3]{};
    bool m_scoreReadLogged = false;
    int m_lastPlayerCount = -1, m_sampleLogs = 0;
    bool m_aloneLogged = false;
    int m_shownLogs = 0;
    std::atomic<unsigned> m_bindLogs{0};
};
}
