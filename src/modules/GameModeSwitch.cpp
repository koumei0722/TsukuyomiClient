#include "modules/GameModeSwitch.h"

#include "config/Config.h"
#include "core/Logger.h"
#include "game/CommandRequest.h"
#include "hooks/Detours.h"
#include "input/Foreground.h"
#include "memory/Scanner.h"
#include "render/Overlay.h"

#include <Windows.h>

#include <chrono>
#include <utility>
#include <vector>

namespace tsukuyomi {

namespace {

long long nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

}

GameModeSwitch& GameModeSwitch::instance()
{
    static GameModeSwitch module;
    return module;
}

bool GameModeSwitch::available() const
{
    return Scanner::instance().found(Target::SetGameMode);
}

const wchar_t* GameModeSwitch::modeName(int mode)
{
    return gamemode::name(mode);
}

void GameModeSwitch::onSetGameMode(void* self, int mode, int extra)
{

    if (self != nullptr) {
        const long long now = nowMs();
        void* const previous = m_self.exchange(self, std::memory_order_acq_rel);
        if (previous != self) {

            m_selfAltCapturedMs.store(m_selfCapturedMs.load(std::memory_order_relaxed),
                                      std::memory_order_relaxed);
            m_selfAlt.store(previous, std::memory_order_release);
        }

        m_selfCapturedMs.store(now, std::memory_order_relaxed);
    }
    m_extra.store(extra, std::memory_order_relaxed);

    if (!gamemode::isSelectable(mode) && !m_warnedUnusualMode) {
        m_warnedUnusualMode = true;
        log().info(L"GameModeSwitch: the game reported game mode id {} "
                   L"(not one of the four JE modes)",
                   mode);
    }

    const int current = m_current.load(std::memory_order_relaxed);
    if (current != mode && gamemode::isSelectable(current)) {
        m_previous.store(current, std::memory_order_relaxed);
    }
    m_current.store(mode, std::memory_order_relaxed);
}

int GameModeSwitch::switchTarget() const
{

    const int previous = m_previous.load(std::memory_order_relaxed);
    if (gamemode::isSelectable(previous)) {
        return previous;
    }
    return (m_current.load(std::memory_order_relaxed) == kCreative) ? kSurvival : kCreative;
}

int GameModeSwitch::nextInCycle(int mode)
{

    for (size_t i = 0; i < std::size(gamemode::kCycle); ++i) {
        if (gamemode::kCycle[i] == mode) {
            return gamemode::kCycle[(i + 1) % std::size(gamemode::kCycle)];
        }
    }
    return gamemode::kCycle[0];
}

bool GameModeSwitch::modifiersDown(const Hotkey& key)
{
    const std::vector<int>& combo = key.combo();
    if (combo.size() < 2) {
        return false;
    }
    for (size_t i = 0; i + 1 < combo.size(); ++i) {
        if ((GetAsyncKeyState(combo[i]) & 0x8000) == 0) {
            return false;
        }
    }
    return true;
}

int GameModeSwitch::spectatorTarget() const
{

    if (m_current.load(std::memory_order_relaxed) != kSpectator) {
        return kSpectator;
    }
    const int previous = m_previous.load(std::memory_order_relaxed);
    return (gamemode::isSelectable(previous) && previous != kSpectator) ? previous : kCreative;
}

bool GameModeSwitch::hasTarget()
{
    if (m_self.load(std::memory_order_relaxed) != nullptr) {
        return true;
    }
    if (!m_warnedNoSelf) {
        m_warnedNoSelf = true;

        log().warn(L"GameModeSwitch: waiting for the player. Re-enter the world, "
                   L"or use /gamemode once (not needed if you inject before entering)");
    }
    return false;
}

void GameModeSwitch::request(Request wanted)
{
    m_request.store(wanted, std::memory_order_relaxed);
}

void GameModeSwitch::publishSelection() const
{

    render::setGameModeSelection(m_selecting.load(std::memory_order_relaxed),
                                 m_selected.load(std::memory_order_relaxed));
}

void GameModeSwitch::onUpdate()
{

    const bool wantsSwitch = m_switchKey.triggered();
    const bool wantsSpectator = m_spectatorKey.triggered();
    const bool modifiers = modifiersDown(m_switchKey);

    if (!enabled() || !input::isGameForeground()) {

        m_selecting.store(false, std::memory_order_relaxed);
        publishSelection();
        return;
    }

    if (wantsSpectator && hasTarget()) {
        m_selecting.store(false, std::memory_order_relaxed);
        m_selected.store(kUnknown, std::memory_order_relaxed);
        request(Request::Spectator);
    }

    if (wantsSwitch && hasTarget()) {
        if (!modifiers) {

            request(Request::Switch);
        } else {

            const bool wasSelecting = m_selecting.exchange(true, std::memory_order_relaxed);
            const int next = wasSelecting
                                 ? nextInCycle(m_selected.load(std::memory_order_relaxed))
                                 : switchTarget();
            m_selected.store(next, std::memory_order_relaxed);
            log().info(L"GameModeSwitch: selecting {}", modeName(next));
        }
    }

    if (m_selecting.load(std::memory_order_relaxed) && !modifiers) {
        m_selecting.store(false, std::memory_order_relaxed);
        request(Request::Commit);
    }

    publishSelection();
}

void GameModeSwitch::onPlayerViewUpdate()
{

    const long long now = nowMs();
    const long long last = m_lastViewMs.exchange(now, std::memory_order_relaxed);
    if (last != 0 && now - last > kViewGapMs) {

        m_request.store(Request::None, std::memory_order_relaxed);
        m_selecting.store(false, std::memory_order_relaxed);
        m_selected.store(kUnknown, std::memory_order_relaxed);

        if (m_selfCapturedMs.load(std::memory_order_relaxed) > last) {

            if (m_selfAltCapturedMs.load(std::memory_order_relaxed) <= last) {
                m_selfAlt.store(nullptr, std::memory_order_relaxed);
            }
            return;
        }

        m_self.store(nullptr, std::memory_order_relaxed);
        m_selfAlt.store(nullptr, std::memory_order_relaxed);
        m_current.store(kUnknown, std::memory_order_relaxed);
        m_previous.store(kUnknown, std::memory_order_relaxed);
        m_warnedNoSelf = false;
        return;
    }

    const Request wanted = m_request.exchange(Request::None, std::memory_order_relaxed);
    if (wanted == Request::None || !enabled()) {
        return;
    }

    applyRequest(wanted);
}

void GameModeSwitch::applyRequest(Request wanted)
{
    switch (wanted) {
    case Request::Switch:
        apply(switchTarget());
        break;

    case Request::Spectator:
        apply(spectatorTarget());
        break;

    case Request::Commit: {

        const int selected = m_selected.load(std::memory_order_relaxed);
        if (selected != kUnknown) {
            apply(selected);
        }
        break;
    }

    case Request::None:
    default:
        break;
    }
}

int GameModeSwitch::countTargets() const
{
    void* const targets[] = {m_self.load(std::memory_order_acquire),
                             m_selfAlt.load(std::memory_order_acquire)};
    int count = 0;
    for (size_t i = 0; i < std::size(targets); ++i) {
        if (targets[i] == nullptr || (i > 0 && targets[i] == targets[0])) {
            continue;
        }
        ++count;
    }
    return count;
}

void GameModeSwitch::apply(int mode)
{

    void* const targets[] = {m_self.load(std::memory_order_acquire),
                             m_selfAlt.load(std::memory_order_acquire)};
    if (targets[0] == nullptr) {
        return;
    }

    const int current = m_current.load(std::memory_order_relaxed);
    if (current == mode) {

        log().info(L"GameModeSwitch: already {}", modeName(mode));
        return;
    }

    if (countTargets() <= 1) {
        if (CommandRequest::instance().run(gamemode::command(mode))) {

            log().success(L"GameModeSwitch: {} -> {} (asked the server with {})",
                          modeName(current), modeName(mode), gamemode::commandW(mode));
            return;
        }

        log().warn(L"GameModeSwitch: this looks like a server world, "
                   L"so {} needs the /gamemode command and it could not be sent",
                   modeName(mode));
        return;
    }

    const int extra = m_extra.load(std::memory_order_relaxed);
    int applied = 0;
    for (size_t i = 0; i < std::size(targets); ++i) {
        void* const target = targets[i];
        if (target == nullptr || (i > 0 && target == targets[0])) {
            continue;
        }
        if (!hooks::callSetGameMode(target, mode, extra)) {

            (i == 0 ? m_self : m_selfAlt).store(nullptr, std::memory_order_release);
            continue;
        }
        ++applied;
    }

    if (applied == 0) {

        log().warn(L"GameModeSwitch: no usable target for {}, re-enter the world",
                   modeName(mode));
        return;
    }

    if (current != kUnknown) {
        m_previous.store(current, std::memory_order_relaxed);
    }
    m_current.store(mode, std::memory_order_relaxed);

    log().success(L"GameModeSwitch: {} -> {} ({} target{})", modeName(current), modeName(mode),
                  applied, (applied == 1) ? L"" : L"s");
}

MenuItem GameModeSwitch::buildMenu()
{
    std::vector<MenuItem> children;
    children.push_back(menu::back());
    children.push_back(enabledItem());
    children.push_back(toggleKeyItem());
    children.push_back(menu::keybind(
        L"Switch key", [this] { return m_switchKey.combo(); },
        [this](std::vector<int> combo) {
            m_switchKey.set(std::move(combo));
            log().info(L"GameModeSwitch: switch key set to {}", m_switchKey.name());
        }));
    children.push_back(menu::keybind(
        L"Spectator key", [this] { return m_spectatorKey.combo(); },
        [this](std::vector<int> combo) {
            m_spectatorKey.set(std::move(combo));
            log().info(L"GameModeSwitch: spectator key set to {}", m_spectatorKey.name());
        }));

    MenuItem item = menu::submenu(name(), std::move(children));
    item.available = [this] { return available(); };
    item.isOn = [this] { return enabled(); };
    return item;
}

void GameModeSwitch::loadConfig(const nlohmann::json& section)
{
    Module::loadConfig(section);
    m_hadEnabledSetting = section.find("enabled") != section.end();

    const auto readCombo = [&section](const char* key, std::vector<int> fallback) {
        const auto it = section.find(key);
        if (it == section.end() || !it->is_array()) {
            return fallback;
        }
        std::vector<int> combo;
        for (const auto& value : *it) {
            if (value.is_number_integer()) {
                combo.push_back(value.get<int>());
            }
        }
        return combo;
    };

    m_switchKey.set(readCombo("switchKeys", {VK_F3, VK_F4}));
    m_spectatorKey.set(readCombo("spectatorKeys", {VK_F3, 'N'}));
}

void GameModeSwitch::saveConfig(nlohmann::json& section) const
{
    Module::saveConfig(section);
    section["switchKeys"] = m_switchKey.combo();
    section["spectatorKeys"] = m_spectatorKey.combo();
}

void GameModeSwitch::onScansReady()
{

    CommandRequest::instance().onScansReady();

    if (!m_hadEnabledSetting) {
        setEnabled(true);
    }
}

}
