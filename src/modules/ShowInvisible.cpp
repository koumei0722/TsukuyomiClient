#include "modules/ShowInvisible.h"

#include "config/Config.h"
#include "config/WriteSwitches.h"
#include "core/Logger.h"
#include "core/Notice.h"
#include "memory/Memory.h"
#include "memory/Scanner.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace tsukuyomi {

namespace {

struct ItemDef {
    const wchar_t* label;
    const char* key;
    Target target;
    Target taskTarget;
    const char* layer;
};

constexpr ItemDef kItems[] = {
    {L"Barrier", "barrier", Target::InvisibleLayerBarrier, Target::InvisibleTaskBarrier, "barrier"},
    {L"Light block", "light", Target::InvisibleLayerLight, Target::InvisibleTaskLight, "light"},
    {L"Structure void", "structureVoid", Target::InvisibleLayerStructureVoid, Target::InvisibleTaskStructureVoid,
     "structure_void"},
    {L"Spectator heads", "spectator", Target::SpectatorViewerFlag, Target::SpectatorViewerFlag, nullptr},
};

static_assert(sizeof(kItems) / sizeof(kItems[0]) == ShowInvisible::kItemCount);

std::string patchName(std::size_t item)
{
    return std::string("ShowInvisible.") + kItems[item].key;
}

bool itemBlocked(std::size_t item)
{
    return writes::blocked(std::string("ShowInvisible:") + kItems[item].key);
}

constexpr std::ptrdiff_t kInlineJeOffset = 4;
constexpr std::ptrdiff_t kTaskJeOffset = 6;

bool layerSiteMatches(const std::byte* je, std::string_view layer)
{
    constexpr std::size_t kWindow = 0x50;
    const auto* const b = reinterpret_cast<const unsigned char*>(je);
    if (!memory::isReadable(b, kWindow + 7) || b[0] != 0x74) {
        return false;
    }
    for (std::size_t i = 2; i < kWindow; ++i) {
        if (b[i] != 0x48 || b[i + 1] != 0x8D || (b[i + 2] != 0x05 && b[i + 2] != 0x0D)) {
            continue;
        }
        std::int32_t disp = 0;
        std::memcpy(&disp, b + i + 3, sizeof(disp));
        const auto* const row = b + i + 7 + disp;
        if (!memory::isReadable(row, 0x18)) {
            return false;
        }
        const char* name = nullptr;
        std::uint64_t length = 0;
        std::memcpy(&name, row + 8, sizeof(name));
        std::memcpy(&length, row + 0x10, sizeof(length));
        if (length != layer.size() || name == nullptr || !memory::isReadable(name, layer.size())) {
            return false;
        }
        return std::memcmp(name, layer.data(), layer.size()) == 0;
    }
    return false;
}

constexpr std::ptrdiff_t kSpectatorAndOffset = 0x3c;

}

ShowInvisible& ShowInvisible::instance()
{
    static ShowInvisible module;
    return module;
}

bool ShowInvisible::itemFound(std::size_t item) const
{
    for (const Patch& patch : m_patches[item]) {
        if (patch.valid()) {
            return true;
        }
    }
    return false;
}

bool ShowInvisible::available() const
{
    for (std::size_t i = 0; i < kItemCount; ++i) {
        if (itemFound(i)) {
            return true;
        }
    }
    return false;
}

void ShowInvisible::resolveLayer(std::size_t item)
{

    const struct {
        Target target;
        std::ptrdiff_t jeOffset;
        const wchar_t* path;
    } sites[] = {

        {kItems[item].target, kItems[item].target == Target::InvisibleLayerLight ? 11 : kInlineJeOffset, L"inline"},
        {kItems[item].taskTarget, kTaskJeOffset, L"task"},
    };
    for (const auto& site : sites) {
        std::byte* const at = Scanner::instance().address(site.target);
        if (at == nullptr) {
            continue;
        }
        if (!layerSiteMatches(at + site.jeOffset, kItems[item].layer)) {
            notice::failOnce((patchName(item) + ".verify").c_str(),
                             std::wstring(L"ShowInvisible: the ") + site.path + L" terrain layer branch for "
                                 + kItems[item].label + L" did not point at its layer; that item is off",
                             "ShowInvisible: a terrain layer was not found (that item is off)");
            m_patches[item].clear();
            return;
        }

        Patch patch = makeSkipPatch(at + site.jeOffset, 2, patchName(item).c_str());
        if (patch.valid()) {
            m_patches[item].push_back(std::move(patch));
        }
    }
}

void ShowInvisible::resolveSpectator()
{
    constexpr std::size_t item = static_cast<std::size_t>(Item::Spectator);
    std::byte* const site = Scanner::instance().address(kItems[item].target);
    if (site == nullptr) {
        return;
    }
    std::byte* const andAlCl = site + kSpectatorAndOffset;
    const auto* const b = reinterpret_cast<const unsigned char*>(andAlCl);

    if (!memory::isReadable(b, 4) || b[0] != 0x20 || b[1] != 0xC8 || b[2] != 0x88 || b[3] != 0x85) {
        notice::failOnce("ShowInvisible.spectator.verify",
                         L"ShowInvisible: the spectator viewer flag did not match (no and al,cl); Spectator heads is off",
                         "ShowInvisible: Spectator heads is off (the viewer check was not found)");
        return;
    }

    Patch patch = makeAtomicPatch(andAlCl, {std::byte{0xB0}, std::byte{0x01}}, patchName(item).c_str());
    if (patch.valid()) {
        m_patches[item].push_back(std::move(patch));
    }
}

void ShowInvisible::onScansReady()
{
    for (std::size_t i = 0; i < kItemCount; ++i) {
        if (kItems[i].layer != nullptr) {
            resolveLayer(i);
        } else {
            resolveSpectator();
        }
    }
    if (!available()) {
        return;
    }

    applyPatches();
    log().info(L"ShowInvisible: patches barrier {} / light {} / structure void {} / spectator {} ({})",
               m_patches[0].size(), m_patches[1].size(), m_patches[2].size(), m_patches[3].size(),
               enabled() ? L"on" : L"off");
}

void ShowInvisible::applyPatches()
{
    const bool on = enabled();
    std::vector<Patch*> apply;
    std::vector<Patch*> remove;
    for (std::size_t i = 0; i < kItemCount; ++i) {
        for (Patch& patch : m_patches[i]) {
            (on && m_show[i] ? apply : remove).push_back(&patch);
        }
    }
    Patch::setAll(remove, false);
    Patch::setAll(apply, true);
}

void ShowInvisible::onEnabledChanged(bool )
{
    applyPatches();
}

void ShowInvisible::shutdown()
{

    for (auto& patches : m_patches) {
        for (Patch& patch : patches) {
            patch.restore();
        }
    }
}

MenuItem ShowInvisible::buildMenu()
{
    std::vector<MenuItem> children;
    children.push_back(enabledItem());
    children.push_back(toggleKeyItem());
    for (std::size_t i = 0; i < kItemCount; ++i) {
        MenuItem item = menu::toggle(
            kItems[i].label, [this, i] { return m_show[i]; },
            [this, i] {
                m_show[i] = !m_show[i];
                applyPatches();
                log().info(L"ShowInvisible: {} is {}", kItems[i].label, m_show[i] ? L"shown" : L"vanilla");
            });

        item.available = [this, i] { return itemFound(i); };
        item.hidden = itemBlocked(i);
        children.push_back(std::move(item));
    }
    MenuItem item = menu::submenu(name(), std::move(children));
    item.available = [this] { return available(); };
    item.isOn = [this] { return enabled(); };
    return item;
}

bool ShowInvisible::writeBlocked() const
{
    if (Module::writeBlocked()) {
        return true;
    }
    for (std::size_t i = 0; i < kItemCount; ++i) {
        if (!itemBlocked(i)) {
            return false;
        }
    }
    return true;
}

void ShowInvisible::loadConfig(const nlohmann::json& section)
{
    Module::loadConfig(section);
    for (std::size_t i = 0; i < kItemCount; ++i) {
        m_show[i] = Config::getBool(section, kItems[i].key, true);
    }
}

void ShowInvisible::saveConfig(nlohmann::json& section) const
{
    Module::saveConfig(section);
    for (std::size_t i = 0; i < kItemCount; ++i) {
        section[kItems[i].key] = m_show[i];
    }
}

}
