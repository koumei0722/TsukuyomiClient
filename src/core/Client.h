#pragma once

#include <Windows.h>

#include <atomic>

#include "input/Hotkey.h"
#include "ui/Console.h"
#include "ui/Menu.h"

namespace tsukuyomi {

class Client {
public:
    static Client& instance();

    void run(HMODULE self);

    void requestUnload();
    bool unloadRequested() const;

    Console& window() { return m_console; }

private:
    Client() = default;

    void startup();
    void mainLoop();
    void shutdown();

    void registerModules();
    void loadHotkeys();
    void saveHotkeys();
    MenuItem buildRootMenu();

    HMODULE m_self = nullptr;
    std::atomic<bool> m_unloadRequested{false};

    Console m_console;

    Hotkey m_consoleKey;
    Hotkey m_forceShowKey;
    Hotkey m_unloadKey;
};

}
