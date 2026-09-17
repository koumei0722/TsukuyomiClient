#include "modules/NoRender.h"

#include "config/Config.h"
#include "core/Logger.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"

#include <Windows.h>

#include <cstring>
#include <string>

namespace tsukuyomi {

namespace {

struct StageDef {
    NoRender::Stage stage;
    const wchar_t* label;
    const char* key;
    const char* zones[6];
};

constexpr StageDef kStages[] = {
    {NoRender::Stage::Terrain, L"Block", "block",
     {"Level renderer camera - Render terrain near",
      "Level renderer camera - Render terrain far",
      "Level renderer camera - Render terrain alpha",
      "Level renderer camera - Render terrain blended (A)",
      "Level renderer camera - Render terrain blended (B)", nullptr}},
    {NoRender::Stage::BlockEntities, L"Block entity", "blockEntity",
     {"Level renderer camera - Render block entities", nullptr, nullptr, nullptr, nullptr,
      nullptr}},
    {NoRender::Stage::Entities, L"Entity", "entity",
     {"Level renderer camera - Render entities",
      "Level renderer camera - Render entity effects", nullptr, nullptr, nullptr, nullptr}},
    {NoRender::Stage::Sky, L"Sky", "sky",
     {"Level renderer camera - Render sky", nullptr, nullptr, nullptr, nullptr, nullptr}},
    {NoRender::Stage::Fog, L"Fog", "fog",
     {"Level renderer camera - Render fog", nullptr, nullptr, nullptr, nullptr, nullptr}},
    {NoRender::Stage::Particles, L"Particle", "particle",
     {"Level renderer camera - Render particles (new)",
      "Level renderer camera - Render particles (old)", nullptr, nullptr, nullptr, nullptr}},
    {NoRender::Stage::Weather, L"Weather", "weather",
     {"Level renderer camera - Render weather",
      "Level renderer camera - Render water weather", nullptr, nullptr, nullptr, nullptr}},
    {NoRender::Stage::NameTags, L"Name tag", "nameTag",
     {"Level renderer camera - Render name tags", nullptr, nullptr, nullptr, nullptr, nullptr}},
    {NoRender::Stage::Shadows, L"Shadow", "shadow",
     {"Level renderer camera - Render shadows", nullptr, nullptr, nullptr, nullptr, nullptr}},
    {NoRender::Stage::Cursor, L"Block outline", "cursor",
     {"Level renderer camera - Render cursor", nullptr, nullptr, nullptr, nullptr, nullptr}},
};

static_assert(sizeof(kStages) / sizeof(kStages[0]) == NoRender::kStageCount);

bool leaTarget(const std::byte* at, std::byte*& target)
{
    const auto* const b = reinterpret_cast<const unsigned char*>(at);
    if ((b[0] != 0x48 && b[0] != 0x4C) || b[1] != 0x8D) {
        return false;
    }
    if ((b[2] & 0xC7) != 0x05) {
        return false;
    }
    std::int32_t disp = 0;
    std::memcpy(&disp, at + 3, sizeof(disp));
    target = const_cast<std::byte*>(at) + 7 + disp;
    return true;
}

std::byte* relTarget(const std::byte* at)
{
    std::int32_t rel = 0;
    std::memcpy(&rel, at + 1, sizeof(rel));
    return const_cast<std::byte*>(at) + 5 + rel;
}

bool startsWith(const std::byte* at, const char* prefix)
{
    const std::size_t length = std::strlen(prefix);
    if (!memory::isReadable(at, length + 1)) {
        return false;
    }
    return std::memcmp(at, prefix, length) == 0;
}

bool functionRange(std::byte* inside, std::byte*& begin, std::byte*& end)
{
    ULONG64 base = 0;
    const auto* const entry = RtlLookupFunctionEntry(reinterpret_cast<ULONG64>(inside), &base,
                                                     nullptr);
    if (entry == nullptr || base == 0) {
        return false;
    }
    begin = reinterpret_cast<std::byte*>(base + entry->BeginAddress);
    end = reinterpret_cast<std::byte*>(base + entry->EndAddress);
    return begin < end;
}

bool directCall(const std::byte* at, std::byte*& target, std::byte*& outBegin)
{
    if (static_cast<unsigned char>(*at) != 0xE8) {
        return false;
    }
    std::byte* const t = relTarget(at);
    std::byte* begin = nullptr;
    std::byte* end = nullptr;
    if (!functionRange(t, begin, end) || begin != t) {
        return false;
    }
    target = t;
    outBegin = begin;
    return true;
}

bool indirectCall(const std::byte* at)
{
    const auto* const b = reinterpret_cast<const unsigned char*>(at);
    if (b[0] != 0xFF || b[1] != 0x15) {
        return false;
    }
    std::int32_t disp = 0;
    std::memcpy(&disp, at + 2, sizeof(disp));
    const std::byte* const slot = at + 6 + disp;
    if (!memory::isReadable(slot, sizeof(void*))) {
        return false;
    }
    void* fn = nullptr;
    std::memcpy(&fn, slot, sizeof(fn));
    return fn != nullptr && memory::isExecutable(fn, 1);
}

bool takesOutParam(const std::byte* call, std::size_t size)
{
    constexpr std::ptrdiff_t kBefore = 0x30;
    constexpr std::size_t kAfter = 8;
    const std::byte* const before = call - kBefore;
    if (!memory::isReadable(before, static_cast<std::size_t>(kBefore))
        || !memory::isReadable(call + size, kAfter)) {
        return false;
    }
    const auto* const head = reinterpret_cast<const unsigned char*>(before);
    const auto* const after = reinterpret_cast<const unsigned char*>(call + size);
    for (std::ptrdiff_t i = 0; i + 7 < kBefore; ++i) {
        const unsigned char rex = head[i];
        if ((rex != 0x48 && rex != 0x4C) || head[i + 1] != 0x8D) {
            continue;
        }
        const unsigned char modrm = head[i + 2];
        const unsigned mod = modrm >> 6;
        const unsigned reg = (modrm >> 3) & 7;
        const unsigned rm = modrm & 7;

        if (rm != 5 || mod != 2) {
            continue;
        }

        const bool isArg = (rex == 0x48 && (reg == 1 || reg == 2))
                           || (rex == 0x4C && (reg == 0 || reg == 1));
        if (!isArg) {
            continue;
        }
        for (std::size_t at = 0; at + 4 <= kAfter; ++at) {
            if (std::memcmp(head + i + 3, after + at, 4) == 0) {
                return true;
            }
        }
    }
    return false;
}

bool nearAssert(const std::byte* call, std::size_t size)
{
    constexpr std::size_t kSpan = 0x60;
    static constexpr unsigned char kDead[] = {0xC7, 0x04, 0x25, 0x00, 0x00, 0x00,
                                              0x00, 0xDE, 0xC0, 0xAD, 0xDE};
    if (!memory::isReadable(call + size, kSpan)) {
        return false;
    }
    const auto* const p = reinterpret_cast<const unsigned char*>(call + size);
    for (std::size_t i = 0; i + sizeof(kDead) <= kSpan; ++i) {
        if (std::memcmp(p + i, kDead, sizeof(kDead)) == 0) {
            return true;
        }
    }
    return false;
}

bool makeForceTrueBytes(const std::byte* at, std::size_t& sizeOut,
                        std::vector<std::byte>& out)
{
    const auto* const b = reinterpret_cast<const unsigned char*>(at);
    unsigned char rex = 0;
    std::size_t head = 0;
    if (b[0] >= 0x40 && b[0] <= 0x4F) {
        rex = b[0];
        head = 1;
    }
    const bool isMovzx = (b[head] == 0x0F && b[head + 1] == 0xB6);
    const bool isMov8 = (b[head] == 0x8A);
    if (!isMovzx && !isMov8) {
        return false;
    }
    const std::size_t modrmAt = head + (isMovzx ? 2u : 1u);
    const unsigned char modrm = b[modrmAt];

    if ((modrm & 0xC7) != 0x40 || b[modrmAt + 1] != 0x10) {
        return false;
    }
    const unsigned reg = (modrm >> 3) & 7;
    const bool regHigh = (rex & 0x04) != 0;
    const std::size_t size = modrmAt + 2;

    out.clear();
    if (isMov8) {

        if (rex != 0 || regHigh) {
            out.push_back(static_cast<std::byte>(0x40 | (regHigh ? 0x01 : 0x00)));
        }
        out.push_back(static_cast<std::byte>(0xB0 + reg));
        out.push_back(std::byte{0x01});
    } else {

        if (regHigh) {
            return false;
        }
        out.push_back(std::byte{0x31});
        out.push_back(static_cast<std::byte>(0xC0 | (reg << 3) | reg));
        out.push_back(static_cast<std::byte>(0xB0 + reg));
        out.push_back(std::byte{0x01});
    }
    if (out.size() > size) {
        return false;
    }
    while (out.size() < size) {
        out.push_back(std::byte{0x90});
    }
    sizeOut = size;
    return true;
}

}

NoRender& NoRender::instance()
{
    static NoRender module;
    return module;
}

bool NoRender::available() const
{
    return m_found > 0;
}

void NoRender::resolveOptions()
{

    static constexpr Stage kOptionStages[kOptionCount] = {
        Stage::Terrain,
        Stage::Entities,
        Stage::BlockEntities,
        Stage::Particles,
        Stage::Sky,
        Stage::Weather,
    };

    std::byte* const anchor = Scanner::instance().address(Target::NameTagStageCaller);
    if (anchor == nullptr) {
        return;
    }
    std::byte* begin = nullptr;
    std::byte* end = nullptr;
    if (!functionRange(anchor, begin, end)) {
        begin = anchor;
        end = anchor + 0x6000;
    }

    constexpr std::size_t kMaxHits = 24;
    struct Hit {
        std::byte* at;
        std::uint32_t id;
    };
    Hit hits[kMaxHits]{};
    std::size_t count = 0;
    for (std::byte* scan = begin; scan + 12 < end && count < kMaxHits;) {
        const auto* const b = reinterpret_cast<const unsigned char*>(scan);
        if (b[0] != 0x41 || b[1] != 0xB8) {
            ++scan;
            continue;
        }
        std::uint32_t id = 0;
        std::memcpy(&id, scan + 2, sizeof(id));

        bool called = false;
        for (std::size_t k = 6; k + 6 < 0x18 && scan + k + 6 < end; ++k) {
            if (b[k] == 0xFF && b[k + 1] == 0x15) {
                called = true;
                break;
            }
        }
        if (called) {
            hits[count++] = {scan, id};
        }
        scan += 6;
    }

    std::size_t first = kMaxHits;
    for (std::size_t i = 0; i + kOptionCount <= count; ++i) {
        bool run = true;
        for (std::size_t k = 1; k < kOptionCount && run; ++k) {
            run = (hits[i + k].id == hits[i].id + k);
        }
        if (run) {
            first = i;
            break;
        }
    }
    if (first == kMaxHits) {
        log().warn(L"NoRender: the option lookup sequence was not found ({} candidates); only "
                   L"the stage-call patch will be used",
                   count);
        return;
    }

    int made = 0;
    for (std::size_t k = 0; k < kOptionCount; ++k) {
        std::byte* const callSite = hits[first + k].at;

        constexpr std::ptrdiff_t kReadSpan = 0x60;
        std::byte* read = nullptr;
        std::size_t size = 0;
        std::vector<std::byte> patched;
        for (std::byte* scan = callSite + 6; scan + 6 < callSite + kReadSpan && scan + 6 < end;
             ++scan) {
            if (makeForceTrueBytes(scan, size, patched)) {
                read = scan;
                break;
            }
        }
        if (read == nullptr) {
            log().warn(L"NoRender: no read site found for option {:#x}",
                       hits[first + k].id);
            continue;
        }
        const std::size_t stage = static_cast<std::size_t>(kOptionStages[k]);
        m_patches[stage].push_back(Patch(read, patched));
        m_byOption[stage] = true;
        ++m_found;
        ++made;
    }

    if (std::byte* const stage = Scanner::instance().address(Target::NameTagStage);
        stage != nullptr) {
        const std::size_t at = static_cast<std::size_t>(Stage::NameTags);
        m_patches[at].push_back(Patch(stage, std::vector<std::byte>{std::byte{0xC3}}));
        m_byOption[at] = true;
        ++m_found;
    } else {
        log().warn(L"NoRender: the name-tag stage was not found");
    }

    if (Scanner::instance().address(Target::FogSettingsFetch) != nullptr) {
        m_byOption[static_cast<std::size_t>(Stage::Fog)] = true;
        ++m_found;
    } else {
        log().warn(L"NoRender: the fog distance getter was not found");
    }

    if (std::byte* const outline = Scanner::instance().address(Target::BlockOutlineDraw);
        outline != nullptr) {
        const std::size_t at = static_cast<std::size_t>(Stage::Cursor);
        m_patches[at].push_back(Patch(outline, std::vector<std::byte>{std::byte{0xC3}}));
        m_byOption[at] = true;
        ++m_found;
    } else {
        log().warn(L"NoRender: the block outline draw function was not found");
    }
}

void NoRender::resolveStages()
{
    std::byte* const anchor = Scanner::instance().address(Target::NameTagStageCaller);
    if (anchor == nullptr) {
        log().warn(L"NoRender: the function that lists the stages was not found, so rendering "
                   L"cannot be stopped");
        return;
    }

    std::byte* begin = nullptr;
    std::byte* end = nullptr;
    if (!functionRange(anchor, begin, end)) {

        begin = anchor;
        end = anchor + 0x6000;
        log().warn(L"NoRender: could not get the function bounds from .pdata; scanning {:#x} "
                   L"bytes from the start",
                   static_cast<std::size_t>(end - begin));
    }

    std::byte* zoneBegin = nullptr;
    int mismatched = 0;

    for (std::byte* at = begin; at + 7 < end; ++at) {
        std::byte* text = nullptr;
        if (!leaTarget(at, text) || !startsWith(text, kZonePrefix)) {
            continue;
        }

        const char* const name = reinterpret_cast<const char*>(text);
        int stageIndex = -1;
        for (std::size_t i = 0; i < kStageCount && stageIndex < 0; ++i) {
            for (const char* const zone : kStages[i].zones) {
                if (zone == nullptr) {
                    break;
                }
                if (std::strcmp(zone, name) == 0) {
                    stageIndex = static_cast<int>(i);
                    break;
                }
            }
        }
        if (stageIndex < 0) {
            continue;
        }

        if (m_byOption[stageIndex]) {
            continue;
        }

        std::byte* main = nullptr;
        for (std::byte* scan = at; scan + 5 < at + kColdPathSpan && scan + 5 < end; ++scan) {
            if (static_cast<unsigned char>(*scan) == 0xE9) {
                main = relTarget(scan);
                break;
            }
        }
        if (main == nullptr || main < begin || main >= end) {
            log().warn(L"NoRender: no branch back to the main path for {}",
                       kStages[stageIndex].label);
            continue;
        }

        std::byte* drawCall = nullptr;
        std::size_t drawSize = 0;
        int calls = 0;
        for (std::byte* scan = main; scan + 6 < main + kMainSpan && scan + 6 < end;) {
            std::byte* target = nullptr;
            std::byte* targetBegin = nullptr;
            if (directCall(scan, target, targetBegin)) {
                ++calls;
                if (calls == 1) {
                    if (zoneBegin == nullptr) {
                        zoneBegin = target;
                    } else if (zoneBegin != target) {

                        ++mismatched;
                        calls = -1000;
                        break;
                    }
                } else {
                    drawCall = scan;
                    drawSize = 5;
                    break;
                }
                scan += 5;
                continue;
            }
            if (calls >= 1 && indirectCall(scan)) {

                drawCall = scan;
                drawSize = 6;
                break;
            }
            ++scan;
        }

        if (drawCall == nullptr) {
            if (calls > -1000) {
                log().warn(L"NoRender: no draw call found for {}",
                           kStages[stageIndex].label);
            }
            continue;
        }

        const bool outParam = takesOutParam(drawCall, drawSize);
        const bool assertNear = nearAssert(drawCall, drawSize);
        if (outParam || assertNear) {
            log().warn(L"NoRender: {} at {:#x} does not look like a draw call, so it is left "
                       L"alone ({})",
                       kStages[stageIndex].label,
                       reinterpret_cast<std::uintptr_t>(drawCall)
                           - reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)),
                       outParam ? L"it takes an out parameter" : L"an assert follows it");
            continue;
        }

        m_patches[stageIndex].push_back(makeNopPatch(drawCall, drawSize));
        ++m_found;

        if (drawSize != 5) {
            continue;
        }
        std::byte* const callee = relTarget(drawCall);
        std::byte* stop = main + kMainSpan;
        if (stop > end) {
            stop = end;
        }

        for (std::byte* scan = main; scan + 6 < stop; ++scan) {
            const auto* const b = reinterpret_cast<const unsigned char*>(scan);
            if (b[0] != 0x88 || b[1] != 0x85) {
                continue;
            }
            for (std::byte* look = scan + 6; look + 7 < stop; ++look) {
                const auto* const c = reinterpret_cast<const unsigned char*>(look);
                if (c[0] == 0x80 && c[1] == 0xBD && c[6] == 0x01
                    && std::memcmp(c + 2, b + 2, 4) == 0) {
                    stop = look;
                    break;
                }
            }
            break;
        }

        int more = 0;

        for (std::byte* scan = drawCall + 5; scan + 5 <= stop;) {
            std::byte* target = nullptr;
            std::byte* targetBegin = nullptr;
            if (directCall(scan, target, targetBegin) && target == callee) {
                if (!takesOutParam(scan, 5) && !nearAssert(scan, 5)) {
                    m_patches[stageIndex].push_back(makeNopPatch(scan, 5));
                    ++more;
                }
                scan += 5;
                continue;
            }
            ++scan;
        }
    }

    if (mismatched > 0) {
        log().warn(L"NoRender: dropped {} stage(s) whose zone start address did not match",
                   mismatched);
    }
}

void NoRender::onScansReady()
{

    resolveOptions();
    resolveStages();
    if (!available()) {
        return;
    }

    applyStages();
}

void NoRender::applyStages()
{
    const bool on = enabled();
    for (std::size_t i = 0; i < kStageCount; ++i) {
        const bool want = on && m_off[i];
        for (Patch& patch : m_patches[i]) {
            patch.setEnabled(want);
        }
    }
}

void NoRender::onEnabledChanged(bool )
{
    applyStages();
}

void NoRender::shutdown()
{

    for (std::size_t i = 0; i < kStageCount; ++i) {
        for (Patch& patch : m_patches[i]) {
            patch.restore();
        }
    }
}

MenuItem NoRender::buildMenu()
{
    std::vector<MenuItem> children;
    children.push_back(menu::back());
    children.push_back(enabledItem());
    children.push_back(toggleKeyItem());

    for (std::size_t i = 0; i < kStageCount; ++i) {
        const StageDef& def = kStages[i];
        MenuItem item = menu::toggle(
            def.label, [this, i] { return m_off[i]; },
            [this, i] {
                m_off[i] = !m_off[i];
                applyStages();
                log().info(L"NoRender: {} is {}",
                           kStages[i].label,
                           m_off[i] ? L"hidden" : L"shown");
            });

        item.available = [this, i] { return !m_patches[i].empty() || m_byOption[i]; };
        children.push_back(std::move(item));
    }

    MenuItem item = menu::submenu(name(), std::move(children));
    item.available = [this] { return available(); };
    item.isOn = [this] { return enabled(); };
    return item;
}

void NoRender::loadConfig(const nlohmann::json& section)
{
    Module::loadConfig(section);
    for (std::size_t i = 0; i < kStageCount; ++i) {
        m_off[i] = Config::getBool(section, kStages[i].key, false);
    }
}

void NoRender::saveConfig(nlohmann::json& section) const
{
    Module::saveConfig(section);
    for (std::size_t i = 0; i < kStageCount; ++i) {
        section[kStages[i].key] = m_off[i];
    }
}

}
