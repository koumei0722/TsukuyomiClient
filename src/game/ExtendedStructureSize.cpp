#include "game/ExtendedStructureSize.h"

#include "config/Config.h"
#include "core/Logger.h"
#include "memory/Memory.h"
#include "memory/Patch.h"
#include "memory/Scanner.h"

#include <libhat/process.hpp>
#include <libhat/scanner.hpp>
#include <libhat/signature.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace tsukuyomi::extendedstructuresize {

namespace {

constexpr char kSection[] = "ExtendedStructureSize";
constexpr std::int32_t kLimit = 0x7fffffff;

constexpr char kScreenPattern[] = "45 39 D0 45 0F 4C D0 29 D0 83 F9 40";
constexpr std::size_t kScreenCmpOffset = 9;
constexpr std::size_t kScreenSites = 4;
constexpr std::size_t kServerCmpOffset = 3;

constexpr std::size_t kSecondSearch = 32;
constexpr std::size_t kReadSpan = 64;

std::vector<Patch> g_patches;

bool parseCmp40(const std::uint8_t* p, int& reg, std::size_t& len)
{
    std::size_t i = 0;
    int ext = 0;
    if (p[0] == 0x41) {
        ext = 8;
        i = 1;
    }
    if (p[i] != 0x83 || (p[i + 1] & 0xF8) != 0xF8 || p[i + 2] != 0x40) {
        return false;
    }
    reg = (p[i + 1] & 7) + ext;
    len = i + 3;
    return true;
}

bool parseMov40(const std::uint8_t* p, int& reg, std::size_t& len)
{
    std::size_t i = 0;
    int ext = 0;
    if (p[0] == 0x41) {
        ext = 8;
        i = 1;
    }
    if ((p[i] & 0xF8) != 0xB8 || p[i + 1] != 0x40 || p[i + 2] != 0 || p[i + 3] != 0 || p[i + 4] != 0) {
        return false;
    }
    reg = (p[i] & 7) + ext;
    len = i + 5;
    return true;
}

bool parseCmov(const std::uint8_t* p, std::uint8_t cc, int& dst, int& src, std::size_t& len)
{
    std::size_t i = 0;
    std::uint8_t rex = 0;
    if ((p[0] & 0xF0) == 0x40) {
        rex = p[0];
        i = 1;
    }
    if ((rex & 0x08) != 0 || p[i] != 0x0F || p[i + 1] != cc || (p[i + 2] & 0xC0) != 0xC0) {
        return false;
    }
    dst = ((p[i + 2] >> 3) & 7) + ((rex & 0x04) != 0 ? 8 : 0);
    src = (p[i + 2] & 7) + ((rex & 0x01) != 0 ? 8 : 0);
    len = i + 3;
    return true;
}

void putMovImm(std::vector<std::byte>& out, int reg, std::int32_t imm)
{
    if (reg >= 8) {
        out.push_back(std::byte{0x41});
    }
    out.push_back(static_cast<std::byte>(0xB8 + (reg & 7)));
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::byte>((static_cast<std::uint32_t>(imm) >> (8 * i)) & 0xff));
    }
}

void putCmpRegReg(std::vector<std::byte>& out, int a, int b)
{
    const std::uint8_t rex = 0x40 | (b >= 8 ? 0x04 : 0) | (a >= 8 ? 0x01 : 0);
    if (rex != 0x40) {
        out.push_back(static_cast<std::byte>(rex));
    }
    out.push_back(std::byte{0x39});
    out.push_back(static_cast<std::byte>(0xC0 | ((b & 7) << 3) | (a & 7)));
}

bool padTo(std::vector<std::byte>& out, std::size_t length)
{
    if (out.size() > length) {
        return false;
    }
    out.resize(length, std::byte{0x90});
    return true;
}

bool buildSite(std::byte* site, const char* name, std::vector<Patch>& out)
{
    std::array<std::uint8_t, kReadSpan> code{};
    if (site == nullptr || !memory::copyGuarded(site, code.data(), code.size())) {
        return false;
    }
    int clamped = 0;
    int bound = 0;
    std::size_t cmpLen = 0;
    std::size_t movLen = 0;
    if (!parseCmp40(code.data(), clamped, cmpLen) || !parseMov40(code.data() + cmpLen, bound, movLen)) {
        return false;
    }
    int dst = 0;
    int src = 0;
    std::size_t cmovLen = 0;
    if (!parseCmov(code.data() + cmpLen + movLen, 0x4D, dst, src, cmovLen) || dst != clamped || src != bound) {
        return false;
    }
    const std::size_t from = cmpLen + movLen + cmovLen;
    std::size_t secondAt = 0;
    std::size_t secondLen = 0;
    int other = 0;
    for (std::size_t at = from; at < from + kSecondSearch && secondLen == 0; ++at) {
        int reg = 0;
        std::size_t len = 0;
        if (parseCmp40(code.data() + at, reg, len) && parseCmov(code.data() + at + len, 0x4C, dst, src, cmovLen)
            && dst == bound && src == reg) {
            secondAt = at;
            secondLen = len;
            other = reg;
        }
    }
    if (secondLen == 0) {
        return false;
    }
    std::vector<std::byte> first;
    putMovImm(first, bound, kLimit);
    putCmpRegReg(first, clamped, bound);
    std::vector<std::byte> second;
    putCmpRegReg(second, other, bound);
    if (!padTo(first, cmpLen + movLen) || !padTo(second, secondLen)) {
        return false;
    }
    out.emplace_back(site, std::move(first), name);
    out.emplace_back(site + secondAt, std::move(second), name);
    return true;
}

std::vector<std::byte*> findScreenSites()
{
    std::vector<std::byte*> sites;
    const auto parsed = hat::parse_signature(kScreenPattern);
    if (!parsed.has_value()) {
        return sites;
    }
    const hat::signature_view view{parsed.value()};
    const auto text = hat::process::get_process_module().get_section_data(".text");
    std::byte* at = text.data();
    std::byte* const end = text.data() + text.size();
    while (at < end) {
        const auto hit = hat::find_pattern(at, end, view);
        if (!hit.has_result()) {
            break;
        }
        sites.push_back(hit.get() + kScreenCmpOffset);
        at = hit.get() + 1;
    }
    return sites;
}

std::size_t applyGroup(std::vector<Patch>& group)
{
    for (Patch& patch : group) {
        if (!patch.valid()) {
            return 0;
        }
    }
    for (Patch& patch : group) {
        if (!patch.apply()) {
            for (Patch& undo : group) {
                undo.restore();
            }
            return 0;
        }
    }
    const std::size_t count = group.size();
    for (Patch& patch : group) {
        g_patches.push_back(std::move(patch));
    }
    return count;
}

}

void start()
{
    nlohmann::json& section = Config::instance().section(kSection);
    Config::keepOnly(section, {"enabled"});
    if (!Config::ensureBool(section, "enabled", true)) {
        log().info(L"ExtendedStructureSize: disabled in Tsukuyomi.json");
        return;
    }

    const std::vector<std::byte*> screenSites = findScreenSites();
    std::vector<Patch> screen;
    bool screenShape = screenSites.size() == kScreenSites;
    for (std::byte* site : screenSites) {
        screenShape = screenShape && buildSite(site, "ExtendedStructureSize.Screen", screen);
    }
    std::size_t screenPatched = 0;
    if (!screenShape) {
        log().error(L"ExtendedStructureSize: the structure editor's size clamps were not as expected ({} found)",
                    screenSites.size());
    } else {
        screenPatched = applyGroup(screen);
    }

    std::byte* const serverSite = Scanner::instance().address(Target::StructureSizeClampServer);
    std::vector<Patch> server;
    std::size_t serverPatched = 0;
    if (serverSite == nullptr || !buildSite(serverSite + kServerCmpOffset, "ExtendedStructureSize.Server", server)) {
        log().error(L"ExtendedStructureSize: the integrated server's size clamp was not found");
    } else {
        serverPatched = applyGroup(server);
    }

    if (screenPatched > 0 || serverPatched > 0) {
        log().info(L"ExtendedStructureSize: structure block X/Z are no longer capped at 64 (screen {} / server {} "
                   L"instructions patched)",
                   screenPatched, serverPatched);
    }
}

void shutdown()
{
    for (Patch& patch : g_patches) {
        patch.restore();
    }
    g_patches.clear();
}

}
