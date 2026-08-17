#pragma once

#include <Windows.h>

#include <atomic>
#include <chrono>

#include "input/Hotkey.h"

namespace tsukuyomi {

class Client {
public:
    static Client& instance();

    void run(HMODULE self);

    void requestUnload();
    bool unloadRequested() const;

private:
    Client() = default;

    void startup();
    void mainLoop();
    void shutdown();

    void registerModules();
    void loadHotkeys();
    void saveHotkeys();

    HMODULE m_self = nullptr;

    bool m_settingsSavePending = false;
    std::chrono::steady_clock::time_point m_settingsSaveAt{};

    std::atomic<bool> m_unloadRequested{false};

    Hotkey m_unloadKey;
};

}
