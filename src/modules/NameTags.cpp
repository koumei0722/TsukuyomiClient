#include "modules/NameTags.h"

#include "config/Config.h"
#include "config/WriteSwitches.h"
#include "modules/DebugKeys.h"
#include "core/Logger.h"
#include "core/Notice.h"
#include "game/BlockWrite.h"
#include "game/NameTagPlatform.h"
#include "game/PlayerListMemory.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"

#include <Windows.h>

#include <cstring>
#include <string>
#include <vector>

namespace tsukuyomi {

namespace {

enum class Site {
    Mobs,
    Sneaking,
    Self,
};

struct PatchDef {
    const wchar_t* label;
    const char* key;
    Site site;
    Target target;
    std::ptrdiff_t offset;
};

constexpr PatchDef kPatches[] = {

    {L"mob names", "mobs", Site::Mobs, Target::NameTagVisibleGate, 0},

    {L"sneaking players", "sneaking", Site::Sneaking, Target::NameTagSneakCheck, 0x10},

    {L"your own name tag", "self", Site::Self, Target::NameTagSelfSkip, 0xa},
};

static_assert(sizeof(kPatches) / sizeof(kPatches[0]) == NameTags::kPatchCount);

static_assert(kPatches[static_cast<std::size_t>(Site::Self)].site == Site::Self);

constexpr const char* kAlwaysKey = "alwaysShow";
constexpr const char* kPlatformKey = "showPlatform";

constexpr std::size_t kBlockSourceLevel = 0x20, kPlayersSlot = 0x9F8, kFetchSlot = 0x1F0;

constexpr unsigned long long kSampleIntervalMs = 250;

std::string patchName(std::size_t i)
{
    return std::string("NameTags.") + kPatches[i].key;
}

bool itemBlocked(const char* key)
{
    return writes::blocked(std::string("NameTags:") + key);
}

bool siteMatches(Site site, const unsigned char* b)
{
    if (!memory::isReadable(b, 7)) {
        return false;
    }
    switch (site) {
    case Site::Mobs:
        return b[0] == 0x80 && b[1] == 0x7C && b[3] == 0x01 && b[5] == 0x75 && b[6] <= 0x7A;
    case Site::Sneaking:
        return b[0] == 0x74;
    case Site::Self:

        return b[0] == 0x74 && b[1] >= 0x80;
    }
    return false;
}

bool readPointer(const void* base, std::size_t offset, const void*& out)
{
    return memory::plausiblePointer(base)
        && memory::copyGuarded(static_cast<const char*>(base) + offset, &out, sizeof(out))
        && memory::plausiblePointer(out);
}

LONG accessFilter(DWORD code)
{
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR
        ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

bool getterDisp(const void* vtable, std::size_t slot, std::int32_t& disp)
{
    const void* fn = nullptr;
    unsigned char code[8]{};
    if (!readPointer(vtable, slot, fn) || !memory::inGameModule(fn) || !memory::copyGuarded(fn, code, sizeof(code))
        || code[0] != 0x48 || code[1] != 0x8B || code[2] != 0x81 || code[7] != 0xC3) {
        return false;
    }
    std::memcpy(&disp, code + 3, sizeof(disp));
    return disp > 0 && disp < 0x10000;
}

bool fetchLooksRight(const void* fn)
{
    return playerlist::mem::levelFetchLooksRight(fn);
}

const void* fetchActor(const void* level, const void* expectedVtable, const void* fetch, std::int64_t id)
{
    __try {
        const auto* vtable = *static_cast<const void* const* const*>(level);
        if (vtable != expectedVtable || vtable[kFetchSlot / sizeof(void*)] != fetch) return nullptr;
        using Fetch = const void* (__fastcall*)(const void*, std::int64_t, bool);
        return reinterpret_cast<Fetch>(const_cast<void*>(fetch))(level, id, false);
    } __except (accessFilter(GetExceptionCode())) {
        return nullptr;
    }
}

bool readGameString(const void* string, std::string& out)
{
    struct Raw {
        union {
            char buffer[16];
            const char* pointer;
        };
        std::size_t size;
        std::size_t capacity;
    } raw{};
    if (!memory::plausiblePointer(string) || !memory::copyGuarded(string, &raw, sizeof(raw))
        || raw.size > raw.capacity || raw.size > 4096) {
        return false;
    }
    const char* data = raw.capacity >= 16 ? raw.pointer : static_cast<const char*>(string);
    out.resize(raw.size);
    return raw.size == 0 || memory::copyGuarded(data, out.data(), raw.size);
}

}

NameTags& NameTags::instance()
{
    static NameTags module;
    return module;
}

bool NameTags::available() const
{
    if (!m_patchesReady.load(std::memory_order_acquire)) return false;
    if (m_buildHooked.load(std::memory_order_acquire)) {
        return true;
    }
    for (const Patch& patch : m_patches) {
        if (patch.valid()) {
            return true;
        }
    }
    return false;
}

void NameTags::onScansReady()
{
    for (std::size_t i = 0; i < kPatchCount; ++i) {
        std::byte* const site = Scanner::instance().address(kPatches[i].target);
        if (site == nullptr) {
            continue;
        }
        std::byte* const at = site + kPatches[i].offset;
        const auto* const b = reinterpret_cast<const unsigned char*>(at);
        if (!siteMatches(kPatches[i].site, b)) {
            notice::failOnce((patchName(i) + ".verify").c_str(),
                             std::wstring(L"NameTags: the branch for ") + kPatches[i].label
                                 + L" did not match; that part is off",
                             "NameTags: a name tag branch was not found (that part is off)");
            continue;
        }
        switch (kPatches[i].site) {
        case Site::Mobs: {

            const auto rel = static_cast<unsigned char>(5 + b[6]);
            m_patches[i] = makeAtomicPatch(at, {std::byte{0xEB}, std::byte{rel}}, patchName(i).c_str());
            break;
        }
        case Site::Sneaking:

            m_patches[i] = makeAtomicPatch(at, {std::byte{0xEB}, std::byte{b[1]}}, patchName(i).c_str());
            break;
        case Site::Self:

            m_patches[i] = makeAtomicPatch(at, {std::byte{0x66}, std::byte{0x90}}, patchName(i).c_str());
            break;
        }
    }
    m_patchesReady.store(true, std::memory_order_release);
    if (!available()) {
        return;
    }

    applyPatches();
    log().info(L"NameTags: patches mobs {} / sneaking {} / self {}, platform hook {} ({}; always show {}, platform {})",
               m_patches[0].valid() ? 1 : 0, m_patches[1].valid() ? 1 : 0, m_patches[2].valid() ? 1 : 0,
               m_buildHooked.load(std::memory_order_acquire) ? 1 : 0, enabled() ? L"on" : L"off",
               m_alwaysShow.load(std::memory_order_relaxed) ? L"on" : L"off",
               m_showPlatform.load(std::memory_order_relaxed) ? L"on" : L"off");
}

void NameTags::applyPatches()
{
    if (!m_patchesReady.load(std::memory_order_acquire)) return;
    std::vector<Patch*> patches;
    for (std::size_t i = 0; i < kPatchCount; ++i) {

        if (kPatches[i].site != Site::Self && m_patches[i].valid()) {
            patches.push_back(&m_patches[i]);
        }
    }
    Patch::setAll(patches, alwaysShowActive());
}

void NameTags::onPlayerViewUpdate()
{
    if (!m_patchesReady.load(std::memory_order_acquire)) return;
    Patch& self = m_patches[static_cast<std::size_t>(Site::Self)];
    if (self.valid()) {

        int perspective = 0;
        const bool thirdPerson = DebugKeys::instance().viewPerspective(perspective) && perspective != 0;
        const bool want = alwaysShowActive() && thirdPerson;
        if (self.applied() != want) {
            self.setEnabled(want);
        }
    }
    samplePlatforms();
}

void NameTags::samplePlatforms()
{
    if (!platformActive() || !m_buildHooked.load(std::memory_order_acquire)) {
        if (m_hasKnown.exchange(false, std::memory_order_acq_rel)) {
            std::lock_guard lock(m_knownMutex);
            m_known.clear();
        }
        return;
    }
    const unsigned long long now = GetTickCount64();
    if (m_lastSample != 0 && now - m_lastSample < kSampleIntervalMs) {
        return;
    }
    m_lastSample = now;

    std::vector<Known> known;
    const void* level = nullptr;
    const void* vtable = nullptr;
    const void* map = nullptr;
    const void* fetch = nullptr;
    std::int32_t playersDisp = 0;
    bool ok = readPointer(blockwrite::renderRegion(), kBlockSourceLevel, level) && readPointer(level, 0, vtable)
        && getterDisp(vtable, kPlayersSlot, playersDisp)
        && readPointer(level, static_cast<std::size_t>(playersDisp), map) && readPointer(vtable, kFetchSlot, fetch)
        && fetchLooksRight(fetch);
    int count = -1;
    std::vector<playerlist::mem::RawPlayer> raw;
    if (ok) {
        raw.resize(playerlist::mem::kMaxPlayerEntries);
        count = playerlist::mem::readPlayerMap(map, raw.data(), static_cast<int>(raw.size()));
        ok = count >= 0;
    }
    if (ok) {
        known.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            if (nametag::platformLabel(raw[i].platform).empty()) {
                continue;
            }
            const void* actor = fetchActor(level, vtable, fetch, raw[i].actorId);
            if (memory::plausiblePointer(actor)) {
                known.push_back({actor, raw[i].platform});
            }
        }
    } else if (!m_sampleFailLogged) {
        m_sampleFailLogged = true;
        log().warn(L"NameTags: the player list was unreadable; platforms are not shown until it can be read");
    }
    std::lock_guard lock(m_knownMutex);
    m_known = std::move(known);
    m_hasKnown.store(!m_known.empty(), std::memory_order_release);
}

void* NameTags::onBuildTag(BuildTagFn original, void* a1, void* out, void* a3, void* a4, void* actor,
                           const void* text, const float* position, float a8, const void* a9)
{
    if (!m_hasKnown.load(std::memory_order_acquire) || !platformActive() || actor == nullptr) {
        return original(a1, out, a3, a4, actor, text, position, a8, a9);
    }
    std::int32_t platform = -1;
    {
        std::lock_guard lock(m_knownMutex);
        for (const Known& entry : m_known) {
            if (entry.actor == actor) {
                platform = entry.platform;
                break;
            }
        }
    }
    const std::string_view label = nametag::platformLabel(platform);
    std::string current;
    if (label.empty() || !readGameString(text, current) || current.empty()) {
        return original(a1, out, a3, a4, actor, text, position, a8, a9);
    }
    const std::string replaced = nametag::withPlatform(current, label);

    alignas(16) unsigned char borrowed[32]{};
    const std::size_t length = replaced.size(), capacity = length < 16 ? 15 : length;
    if (length < 16) {
        std::memcpy(borrowed, replaced.c_str(), length + 1);
    } else {
        const char* buffer = replaced.c_str();
        std::memcpy(borrowed, &buffer, sizeof(buffer));
    }
    std::memcpy(borrowed + 0x10, &length, sizeof(length));
    std::memcpy(borrowed + 0x18, &capacity, sizeof(capacity));
    return original(a1, out, a3, a4, actor, borrowed, position, a8, a9);
}

void NameTags::onEnabledChanged(bool )
{
    applyPatches();
}

void NameTags::shutdown()
{

    for (Patch& patch : m_patches) {
        patch.restore();
    }
}

MenuItem NameTags::buildMenu()
{
    std::vector<MenuItem> children;
    children.push_back(enabledItem());
    children.push_back(toggleKeyItem());

    MenuItem always = menu::toggle(
        L"Always show name tags", [this] { return m_alwaysShow.load(std::memory_order_relaxed); },
        [this] {
            const bool value = !m_alwaysShow.load(std::memory_order_relaxed);
            m_alwaysShow.store(value, std::memory_order_relaxed);
            applyPatches();
            log().info(L"NameTags: always show is {}", value ? L"on" : L"off");
        });

    always.available = [this] {
        if (!m_patchesReady.load(std::memory_order_acquire)) return false;
        for (const Patch& patch : m_patches) {
            if (patch.valid()) {
                return true;
            }
        }
        return false;
    };
    always.hidden = itemBlocked(kAlwaysKey);
    children.push_back(std::move(always));

    MenuItem platform = menu::toggle(
        L"Show platform", [this] { return m_showPlatform.load(std::memory_order_relaxed); },
        [this] {
            const bool value = !m_showPlatform.load(std::memory_order_relaxed);
            m_showPlatform.store(value, std::memory_order_relaxed);
            log().info(L"NameTags: show platform is {}", value ? L"on" : L"off");
        });
    platform.available = [this] { return m_buildHooked.load(std::memory_order_acquire); };
    platform.hidden = itemBlocked(kPlatformKey);
    children.push_back(std::move(platform));

    MenuItem item = menu::submenu(name(), std::move(children));
    item.available = [this] { return available(); };
    item.isOn = [this] { return enabled(); };
    return item;
}

bool NameTags::writeBlocked() const
{
    return Module::writeBlocked() || (itemBlocked(kAlwaysKey) && itemBlocked(kPlatformKey));
}

void NameTags::loadConfig(const nlohmann::json& section)
{
    Module::loadConfig(section);
    m_alwaysShow.store(Config::getBool(section, kAlwaysKey, true), std::memory_order_relaxed);
    m_showPlatform.store(Config::getBool(section, kPlatformKey, true), std::memory_order_relaxed);
}

void NameTags::saveConfig(nlohmann::json& section) const
{
    Module::saveConfig(section);
    section[kAlwaysKey] = m_alwaysShow.load(std::memory_order_relaxed);
    section[kPlatformKey] = m_showPlatform.load(std::memory_order_relaxed);
}

}
