#include "game/ExtendedFov.h"

#include "config/Config.h"
#include "core/Logger.h"
#include "game/ExtendedFovLogic.h"
#include "game/UiProbe.h"
#include "memory/Memory.h"
#include "memory/Patch.h"
#include "memory/Scanner.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace tsukuyomi::extendedfov {

namespace {

constexpr char kSection[] = "ExtendedFov";
constexpr std::ptrdiff_t kFieldsOffset = 0x10;
constexpr std::ptrdiff_t kMaxOffset = 0x14;
constexpr std::ptrdiff_t kValueOffset = 0x18;
constexpr std::size_t kSignatureBytes = 0x40;
constexpr std::size_t kVtableLea = 0x13;
constexpr int kMaxOptions = 8;
constexpr int kScanAttempts = 24;
constexpr auto kRetryInterval = std::chrono::seconds(5);
constexpr auto kPollInterval = std::chrono::milliseconds(250);
constexpr int kChangeLogLimit = 20;

Patch g_clampPatch;

std::mutex g_lock;
std::thread g_worker;
std::atomic<bool> g_stop{false};

Registered g_reg;
const void* g_vtable = nullptr;
std::array<std::byte*, kMaxOptions> g_options{};
int g_optionCount = 0;
float g_valueAfterStart = 0.0f;
std::atomic<bool> g_ready{false};

bool g_enabled = false;
bool g_watching = false;
float g_last = 0.0f;
std::chrono::steady_clock::time_point g_nextPoll{};
int g_changeLogs = 0;

LONG accessFilter(DWORD code)
{
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER
                                                                                  : EXCEPTION_CONTINUE_SEARCH;
}

bool scanRegion(const std::uintptr_t* begin, std::size_t words, std::uintptr_t needle, std::byte** out, int cap,
                int& found)
{
    __try {
        for (std::size_t i = 0; i < words && found < cap; ++i) {
            if (begin[i] == needle) {
                out[found++] = reinterpret_cast<std::byte*>(const_cast<std::uintptr_t*>(begin + i));
            }
        }
        return true;
    } __except (accessFilter(GetExceptionCode())) {
        return false;
    }
}

bool readFields(const std::byte* option, float (&fields)[5])
{
    return memory::copyGuarded(option + kFieldsOffset, fields, sizeof(fields));
}

int findFovOptions(std::array<std::byte*, kMaxOptions>& out, int& vtableHits)
{
    constexpr int kCap = 512;
    std::byte* hits[kCap] = {};
    vtableHits = 0;
    MEMORY_BASIC_INFORMATION info{};
    for (auto* at = reinterpret_cast<std::uint8_t*>(std::uintptr_t{0x10000});
         !g_stop.load(std::memory_order_relaxed) && vtableHits < kCap
         && VirtualQuery(at, &info, sizeof(info)) == sizeof(info);
         at = static_cast<std::uint8_t*>(info.BaseAddress) + info.RegionSize) {
        if (info.State != MEM_COMMIT || info.Type != MEM_PRIVATE || info.Protect != PAGE_READWRITE
            || info.RegionSize > (256ull << 20)) {
            continue;
        }
        scanRegion(static_cast<const std::uintptr_t*>(info.BaseAddress), info.RegionSize / 8,
                   reinterpret_cast<std::uintptr_t>(g_vtable), hits, kCap, vtableHits);
    }
    int count = 0;
    for (int i = 0; i < vtableHits && count < kMaxOptions; ++i) {
        float fields[5] = {};
        if (readFields(hits[i], fields) && looksLikeFovOption(fields, g_reg)) {
            out[count++] = hits[i];
        }
    }
    return count;
}

void work(bool restore, float saved)
{
    for (int attempt = 1; attempt <= kScanAttempts && !g_stop.load(std::memory_order_relaxed); ++attempt) {
        const auto began = std::chrono::steady_clock::now();
        std::array<std::byte*, kMaxOptions> found{};
        int vtableHits = 0;
        const int count = findFovOptions(found, vtableHits);
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
        if (count > 0) {
            float before = 0.0f;
            memory::copyGuarded(found[0] + kValueOffset, &before, sizeof(before));
            int raised = 0;
            int restored = 0;
            for (int i = 0; i < count; ++i) {
                if (memory::writeGuarded(found[i] + kMaxOffset, &kExtendedMax, sizeof(kExtendedMax))) {
                    ++raised;
                }
                if (restore && memory::writeGuarded(found[i] + kValueOffset, &saved, sizeof(saved))) {
                    ++restored;
                }
            }
            float after = before;
            memory::copyGuarded(found[0] + kValueOffset, &after, sizeof(after));
            log().info(L"ExtendedFov: raised the field of view limit {} -> {} on {}/{} option(s) "
                       L"({} FloatOption(s) on the heap, attempt {}, {} ms). value {} -> {}{}",
                       g_reg.max, kExtendedMax, raised, count, vtableHits, attempt, elapsed, before, after,
                       restore ? (restored == count ? L" (restored the saved value)" : L" (could not restore)")
                               : L"");
            g_options = found;
            g_optionCount = count;
            g_valueAfterStart = after;
            g_ready.store(true, std::memory_order_release);
            return;
        }
        if (attempt == kScanAttempts) {
            g_clampPatch.restore();
        }
        if (attempt == 1 || attempt == kScanAttempts) {
            log().info(L"ExtendedFov: the field of view option was not found on the heap "
                       L"({} FloatOption(s), attempt {}/{}, {} ms){}",
                       vtableHits, attempt, kScanAttempts, elapsed,
                       attempt == kScanAttempts ? L". Giving up" : L". Retrying every 5 s");
        }
        for (auto waited = std::chrono::milliseconds(0);
             waited < kRetryInterval && !g_stop.load(std::memory_order_relaxed);
             waited += std::chrono::milliseconds(100)) {
            Sleep(100);
        }
    }
}

const float* findRdataFloat(float wanted)
{
    auto* const base = reinterpret_cast<const std::byte*>(GetModuleHandleW(nullptr));
    if (base == nullptr) {
        return nullptr;
    }
    const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (std::memcmp(section->Name, ".rdata", 7) != 0) {
            continue;
        }
        const std::byte* const begin = base + section->VirtualAddress;
        const std::size_t size = section->Misc.VirtualSize & ~static_cast<std::size_t>(3);
        for (std::size_t at = 0; at + sizeof(float) <= size; at += sizeof(float)) {
            float value = 0.0f;
            std::memcpy(&value, begin + at, sizeof(value));
            if (value == wanted) {
                return reinterpret_cast<const float*>(begin + at);
            }
        }
    }
    return nullptr;
}

bool raiseRenderClamp()
{
    std::byte* const site = Scanner::instance().address(Target::FovRenderClamp);
    std::array<std::byte, kClampNextOffset> code{};
    if (site == nullptr || !memory::copyGuarded(site, code.data(), code.size()) || !isClampSite(code)) {
        log().error(L"ExtendedFov: the clamp of the rendered field of view was not found");
        return false;
    }
    std::int32_t disp = 0;
    std::memcpy(&disp, code.data() + kClampDispOffset, sizeof(disp));
    const std::byte* const next = site + kClampNextOffset;
    float current = 0.0f;
    if (!memory::copyGuarded(next + disp, &current, sizeof(current))) {
        log().error(L"ExtendedFov: could not read the clamp of the rendered field of view");
        return false;
    }
    if (!(current > g_reg.max && current < kExtendedMax)) {
        log().error(L"ExtendedFov: unexpected clamp of the rendered field of view ({})", current);
        return false;
    }
    const float* const target = findRdataFloat(kExtendedMax);
    if (target == nullptr) {
        log().error(L"ExtendedFov: no {} constant in the game to point the clamp at", kExtendedMax);
        return false;
    }
    const std::int64_t newDisp = reinterpret_cast<const std::byte*>(target) - next;
    if (newDisp < INT32_MIN || newDisp > INT32_MAX) {
        return false;
    }
    const auto disp32 = static_cast<std::int32_t>(newDisp);
    std::vector<std::byte> bytes(sizeof(disp32));
    std::memcpy(bytes.data(), &disp32, sizeof(disp32));
    g_clampPatch = Patch(site + kClampDispOffset, std::move(bytes), "ExtendedFov.RenderClamp");
    if (!g_clampPatch.valid()) {
        return false;
    }
    if (!g_clampPatch.apply()) {
        log().error(L"ExtendedFov: could not patch the clamp of the rendered field of view");
        return false;
    }
    log().info(L"ExtendedFov: rendered field of view clamp {} -> {}", current, kExtendedMax);
    return true;
}

bool remember(float fov)
{
    nlohmann::json& section = Config::instance().section(kSection);
    if (shouldRemember(fov, g_reg)) {
        const auto it = section.find("fov");
        if (it != section.end() && it->is_number() && it->get<float>() == fov) {
            return false;
        }
        section["fov"] = fov;
        return true;
    }
    return section.erase("fov") > 0;
}

}

void start()
{
    nlohmann::json& section = Config::instance().section(kSection);
    Config::keepOnly(section, {"enabled", "fov"});
    if (const auto it = section.find("fov"); it != section.end() && !it->is_number()) {
        (void)Config::getFloat(section, "fov", 0.0f);
        section.erase(it);
    }
    g_enabled = Config::ensureBool(section, "enabled", true);
    if (!g_enabled) {
        log().info(L"ExtendedFov: disabled in Tsukuyomi.json");
        return;
    }

    std::byte* const site = Scanner::instance().address(Target::FovOptionCtor);
    std::array<std::byte, kSignatureBytes> code{};
    if (site == nullptr || !memory::copyGuarded(site, code.data(), code.size())
        || !readRegistration(code, g_reg)) {
        log().error(L"ExtendedFov: the field of view option was not found in the game. Not raising the limit");
        return;
    }
    g_vtable = memory::ripTarget(site + kVtableLea, 3);
    if (g_vtable == nullptr) {
        log().error(L"ExtendedFov: could not read the FloatOption vtable. Not raising the limit");
        return;
    }

    if (!raiseRenderClamp()) {
        log().error(L"ExtendedFov: not raising the field of view limit");
        return;
    }

    const float saved = Config::getFloat(section, "fov", 0.0f);
    const bool restore = restorable(saved, g_reg);

    std::lock_guard<std::mutex> lock(g_lock);
    g_worker = std::thread([restore, saved] { work(restore, saved); });
}

void update()
{
    if (!g_enabled) {
        return;
    }
    if (!g_watching) {
        if (!g_ready.load(std::memory_order_acquire)) {
            return;
        }
        g_watching = true;
        g_last = g_valueAfterStart;
        if (remember(g_last)) {
            uiprobe::markSettingsDirty();
        }
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < g_nextPoll) {
        return;
    }
    g_nextPoll = now + kPollInterval;

    float current = g_last;
    bool readAny = false;
    for (int i = 0; i < g_optionCount; ++i) {
        float fields[5] = {};
        const void* head = nullptr;
        if (!memory::copyGuarded(g_options[i], &head, sizeof(head)) || head != g_vtable
            || !readFields(g_options[i], fields) || !looksLikeFovOption(fields, g_reg)) {
            continue;
        }
        readAny = true;
        if (fields[2] != g_last) {
            current = fields[2];
            break;
        }
    }
    if (!readAny) {
        g_watching = false;
        g_enabled = false;
        log().warn(L"ExtendedFov: the field of view option no longer looks right. Stopped watching it");
        return;
    }
    if (current == g_last) {
        return;
    }
    g_last = current;
    const bool changed = remember(current);
    if (changed) {
        uiprobe::markSettingsDirty();
    }
    if (g_changeLogs < kChangeLogLimit) {
        ++g_changeLogs;
        log().info(L"ExtendedFov: field of view is now {}{}", current,
                   shouldRemember(current, g_reg) ? L" (saved for the next injection)"
                                                  : (changed ? L" (forgot the saved value)" : L""));
    }
}

void shutdown()
{
    g_stop.store(true, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (g_worker.joinable()) {
            g_worker.join();
        }
    }
    g_clampPatch.restore();
}

}
