#include "game/CommandSyntax.h"

#include <Windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "core/Logger.h"
#include "core/Strings.h"
#include "game/AvailableCommandsPatch.h"
#include "game/GameString.h"
#include "hooks/Detours.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"

namespace tsukuyomi::commandsyntax {
namespace {

using CtorFn = void*(__fastcall*)(void* parser, void* registry, std::int32_t version);
using ParseFn = bool(__fastcall*)(void* parser, const void* text);
using DtorFn = void(__fastcall*)(void* parser);
using ParamsFn = void*(__fastcall*)(void* parser, void* out);

constexpr std::size_t kParserBytes = 0x200;
constexpr std::size_t kErrorKey = 0x80;
constexpr char kUnknownCommand[] = "commands.generic.unknown";

struct Functions {
    CtorFn ctor = nullptr;
    ParseFn parse = nullptr;
    DtorFn dtor = nullptr;
    ParamsFn params = nullptr;
    std::int32_t version = 0;
};

std::atomic_bool g_toldUnavailable{false};
std::atomic_bool g_faulted{false};

void unavailable(const wchar_t* reason)
{
    if (!g_toldUnavailable.exchange(true)) {
        log().warn(L"CommandSyntax: the game's command parser is not used for /tk ({}); its own replies are shown instead",
                   reason);
    }
}

bool callable(const void* fn)
{
    return fn != nullptr && memory::inGameModule(const_cast<void*>(fn)) && memory::isExecutable(const_cast<void*>(fn), 1);
}

bool resolve(Functions& out)
{
    const Scanner& scanner = Scanner::instance();
    const auto* ctorSite = scanner.address(Target::CommandParserCtorSite);
    const auto* parseSite = scanner.address(Target::CommandParseSite);
    const auto* dtorSite = scanner.address(Target::CommandParserDtorSite);
    const auto* paramsSite = scanner.address(Target::CommandParseErrorParamsSite);
    if (ctorSite == nullptr || parseSite == nullptr || dtorSite == nullptr || paramsSite == nullptr) return false;
    std::int32_t version = 0;
    if (!memory::copyGuarded(ctorSite + 12, &version, sizeof(version)) || version <= 0 || version > 0x10000) return false;
    out.version = version;
    out.ctor = reinterpret_cast<CtorFn>(memory::ripTarget(ctorSite + 16, 1));
    out.parse = reinterpret_cast<ParseFn>(memory::ripTarget(parseSite + 11, 1));
    out.dtor = reinterpret_cast<DtorFn>(memory::ripTarget(dtorSite + 4, 1));
    out.params = reinterpret_cast<ParamsFn>(memory::ripTarget(paramsSite + 14, 1));
    return callable(reinterpret_cast<const void*>(out.ctor)) && callable(reinterpret_cast<const void*>(out.parse))
        && callable(reinterpret_cast<const void*>(out.dtor)) && callable(reinterpret_cast<const void*>(out.params));
}

bool constructGuarded(CtorFn ctor, void* parser, void* registry, std::int32_t version)
{
    __try {
        ctor(parser, registry, version);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool parseGuarded(ParseFn parse, void* parser, const void* text, bool& ok)
{
    __try {
        ok = parse(parser, text);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool paramsGuarded(ParamsFn params, void* parser, void* out)
{
    __try {
        params(parser, out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool destroyGuarded(DtorFn dtor, void* parser)
{
    __try {
        dtor(parser);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool errorText(const Functions& fns, std::byte* parser, std::string& message)
{
    std::string key;
    if (!gamestring::read(parser + kErrorKey, key) || key.empty() || key == kUnknownCommand) return false;
    alignas(16) std::byte params[24]{};
    if (!paramsGuarded(fns.params, parser, params)) {
        g_faulted.store(true, std::memory_order_release);
        log().error(L"CommandSyntax: building the parse error parameters faulted; the parser is not used again");
        return false;
    }
    alignas(16) std::byte out[32]{};
    const bool translated = hooks::translateWith(out, parser + kErrorKey, params);
    const bool read = translated && gamestring::read(out, message);
    if (translated) gamestring::release(out);
    gamestring::releaseVector(params);
    return read && !message.empty();
}

}

Result check(const std::string& command, std::string& message)
{
    message.clear();
    if (g_faulted.load(std::memory_order_acquire)) return Result::Unavailable;
    Functions fns;
    if (!resolve(fns)) { unavailable(L"the parser signatures are missing"); return Result::Unavailable; }
    void* const registry = availablecommands::clientRegistry();
    if (registry == nullptr || !memory::isReadable(registry, sizeof(void*))) {
        unavailable(L"tk is not in the client command registry yet");
        return Result::Unavailable;
    }
    if (!gamestring::available()) { unavailable(L"game strings are unavailable"); return Result::Unavailable; }
    alignas(16) std::byte text[32]{};
    if (!gamestring::assign(text, command)) return Result::Unavailable;
    alignas(16) std::byte parser[kParserBytes]{};
    if (!constructGuarded(fns.ctor, parser, registry, fns.version)) {
        g_faulted.store(true, std::memory_order_release);
        log().error(L"CommandSyntax: constructing the game's command parser faulted; it is not used again");
        gamestring::release(text);
        return Result::Unavailable;
    }
    bool ok = false;
    Result result = Result::Ok;
    if (!parseGuarded(fns.parse, parser, text, ok)) {
        g_faulted.store(true, std::memory_order_release);
        log().error(L"CommandSyntax: the game's command parser faulted; it is not used again");
        result = Result::Unavailable;
    } else if (!ok) {
        result = errorText(fns, parser, message) ? Result::SyntaxError : Result::Unavailable;
        if (result == Result::Unavailable) unavailable(L"the parse error could not be translated");
    }
    if (!destroyGuarded(fns.dtor, parser)) {
        g_faulted.store(true, std::memory_order_release);
        log().error(L"CommandSyntax: destroying the game's command parser faulted; it is not used again");
    }
    gamestring::release(text);
    static std::atomic<int> told{0};
    if (told.fetch_add(1, std::memory_order_relaxed) < 4) {
        log().info(L"CommandSyntax: {} -> {}", toUtf16(command),
                   result == Result::Ok ? L"ok" : result == Result::SyntaxError ? toUtf16(message) : L"unavailable");
    }
    return result;
}

}
