#include "game/GameCommand.h"

#include "hooks/Detours.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"

#include <Windows.h>

namespace tsukuyomi::gamecommand {
namespace {
using CommandFn = bool(__fastcall*)(const std::string*, const void*);

CommandFn resolve()
{
    const auto* site = Scanner::instance().address(Target::ChatCommandRunSite);
    if (!site) return nullptr;
    static const CommandFn resolved = [site]() -> CommandFn {
        if (!memory::isReadable(site, 0x32) || site[0x2D] != std::byte{0xE8}) return nullptr;
        void* fn = memory::ripTarget(site, 0x2E);
        return memory::inGameModule(fn) && memory::isExecutable(fn, 1) ? reinterpret_cast<CommandFn>(fn) : nullptr;
    }();
    return resolved;
}

bool runRaw(void* ci, CommandFn fn, const std::string* text, bool& handled)
{
    __try {
        void* control = *reinterpret_cast<void**>(static_cast<std::byte*>(ci) + 8);
        if (!control || *static_cast<unsigned char*>(control) != 1) return false;
        void* reference[3] = {control, nullptr, ci};
        handled = fn(text, reference);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
}

bool available() { return resolve() != nullptr; }

bool run(const std::string& text, bool& handled)
{
    handled = false;
    const auto fn = resolve();
    void* ci = hooks::gameClientInstance();
    return ci && fn && runRaw(ci, fn, &text, handled);
}
}
