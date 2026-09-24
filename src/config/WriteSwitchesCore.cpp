#include "config/WriteSwitches.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <set>

namespace tsukuyomi::writes {

namespace {

constexpr const char* kPlayerViewNeeds =
    "AntiEffect,AutoTool,CreativeNoClip,FastBlockPlacement,FlySpeed,FreeCamera,Fullbright,HandRestock,"
    "OffhandSwap,Scaffold,Schematica,NoRender:fog";

const std::vector<Entry> kTable = {
    {"AbilitiesAccess", Kind::Hook, "CreativeNoClip,FlySpeed", "", ""},
    {"AddRequestAction", Kind::Hook, "", "OffhandSwap", ""},
    {"AttackCore", Kind::Hook, "AutoTool:attack", "", ""},
    {"BeRenderLoop", Kind::Hook, "Schematica", "", ""},
    {"BlockRenderLookup", Kind::Hook, "Schematica", "", ""},
    {"BlockSourceGetBlock", Kind::Hook, "Schematica", "", ""},
    {"BlockSourceGetExtra", Kind::Hook, "", "Schematica", ""},
    {"BlockSourceSetBlock", Kind::Hook, "Schematica", "", ""},
    {"BlockTessellate", Kind::Hook, "Schematica", "", ""},
    {"CameraUpdate", Kind::Hook, "FreeCamera,Zoom", "Schematica", ""},
    {"ChunkCoordinatorFrame", Kind::Hook, "Schematica", "", ""},
    {"ChunkMeshBuild", Kind::Hook, "Schematica", "", ""},
    {"ChunkVisibilityScan", Kind::Hook, "Schematica", "", ""},
    {"ContainerGetItem", Kind::Hook, "InventoryHUD:offhand", "", ""},
    {"ContainerOpenHandle", Kind::Hook, "HandRestock,OffhandSwap", "", ""},
    {"ContainerOpenRead", Kind::Hook, "", "OffhandSwap", ""},
    {"ContainerScreenCtor", Kind::Hook, "InventoryHUD,ItemScroller,ShulkerPreview", "", ""},
    {"ContainerScreenDtor", Kind::Hook, "ItemScroller,ShulkerPreview", "", ""},
    {"ContainerScreenTick", Kind::Hook, "ItemScroller,ShulkerPreview", "", ""},
    {"ContainerSm", Kind::Hook, "ItemScroller", "", ""},
    {"FogSettingsFetch", Kind::Hook, "NoRender:fog", "", ""},
    {"GetActorEffect", Kind::Hook, "AntiEffect,Fullbright", "", ""},
    {"GetDestroySpeed", Kind::Hook, "AutoTool", "", ""},
    {"HandleItemStackResponse", Kind::Hook, "HandRestock,OffhandSwap", "", ""},
    {"HitResultAssign", Kind::Hook, "", "Schematica", ""},
    {"HoverRendererRender", Kind::Hook, "ShulkerPreview:tooltip", "", ""},
    {"HudScreenCtor", Kind::Hook, "InventoryHUD", "", ""},
    {"I18nGet", Kind::Hook, "", "Menu:settings", ""},
    {"InputGather", Kind::Hook, "FreeCamera", "ItemScroller", ""},
    {"InventoryContentRead", Kind::Hook, "OffhandSwap", "HandRestock", ""},
    {"InventoryHotbarKey", Kind::Hook, "", "OffhandSwap", ""},
    {"InventoryHoveredSlot", Kind::Hook, "FastInventory", "OffhandSwap", ""},
    {"LegacyParticleInsert", Kind::Hook, "", "NoRender:particle", ""},
    {"LevelBuildDispatch", Kind::Hook, "Schematica", "", ""},
    {"MoveInputHandler", Kind::Hook, "Schematica:page", "FreeCamera,HandRestock,ItemScroller", ""},
    {"MoveIntentFromInput", Kind::Hook, "FreeCamera", "", ""},
    {"NotifyInventoryOpen", Kind::Hook, "FastInventory,HandRestock,OffhandSwap", "", ""},
    {"OreKeyRowsBuild", Kind::Hook, "", "Menu:keys", ""},
    {"PacketSend", Kind::Hook, "FreeCamera,HandRestock,OffhandSwap", "", ""},
    {"PlayerView", Kind::Hook, kPlayerViewNeeds, "NoRender:particle,ItemScroller", ""},
    {"SceneStackPush", Kind::Hook, "", "ItemScroller", ""},
    {"SendComplexTransaction", Kind::Hook, "AutoTool:attack", "", ""},
    {"SetGameMode", Kind::Hook, "InventoryHUD:offhand,Schematica", "Scaffold", ""},
    {"SetSelectedSlot", Kind::Hook, "AutoTool,HandRestock,OffhandSwap", "Zoom", ""},
    {"SettingsFindComponent", Kind::Hook, "", "Menu:settings", ""},
    {"SettingsGroupInfoUpdate", Kind::Hook, "", "", ""},
    {"SettingsGroupRegister", Kind::Hook, "", "Menu:settings", ""},
    {"SettingsProviderCall", Kind::Hook, "", "Menu:settings", ""},
    {"ShulkerContentsText", Kind::Hook, "ShulkerPreview:tooltip", "", ""},
    {"SubChunkSetBlock", Kind::Hook, "Schematica", "", ""},
    {"SubChunkStoragePredicate", Kind::Hook, "Schematica", "", ""},
    {"TradeCurrentTier", Kind::Hook, "", "ItemScroller", ""},
    {"TradeHoverInvoke", Kind::Hook, "", "ItemScroller", ""},
    {"TradeSecondaryInvoke", Kind::Hook, "", "ItemScroller", ""},
    {"TradeSelParse", Kind::Hook, "", "ItemScroller", ""},
    {"TradeSelectorTotal", Kind::Hook, "", "ItemScroller", ""},
    {"TradeTierName", Kind::Hook, "", "ItemScroller", ""},
    {"TradeTierTotal", Kind::Hook, "", "ItemScroller", ""},
    {"TradeTierUnlocked", Kind::Hook, "", "ItemScroller", ""},
    {"TradeTierVisible", Kind::Hook, "", "ItemScroller", ""},
    {"UiDefLookup", Kind::Hook, "InventoryHUD,Schematica:page,ShulkerPreview:icons", "ItemScroller", ""},
    {"UiEventDispatch", Kind::Hook, "Schematica:page", "", ""},
    {"UiSliderPublish", Kind::Hook, "", "Schematica:page", ""},
    {"ViewVector", Kind::Hook, "", "FreeCamera,Scaffold", ""},
    {"VisibilityGate", Kind::Hook, "Schematica", "", ""},
    {"buildBlock", Kind::Hook, "FastBlockPlacement,Scaffold", "", ""},
    {"useItem", Kind::Hook, "", "", "FastRightClick"},
    {"useItemTransaction", Kind::Hook, "", "", "FastRightClick"},
    {"PeekMessageW", Kind::Hook, "", "Menu:settings", ""},
    {"PlaySound", Kind::Hook, "", "Menu:sound", ""},
    {"Present", Kind::Hook, "", "Schematica", ""},
    {"Present1", Kind::Hook, "", "", ""},
    {"ExecuteCommandLists", Kind::Hook, "", "", ""},
    {"NameTagStage", Kind::Hook, "Schematica:boxes", "", ""},
    {"NameTagStageCaller", Kind::Hook, "Schematica:boxes", "", ""},
    {"D3D12ResourceBarrier", Kind::Hook, "", "", ""},
    {"D3D12DrawIndexedInstanced", Kind::Hook, "", "", ""},
    {"D3D12DrawInstanced", Kind::Hook, "", "", ""},
    {"D3D12SetPipelineState", Kind::Hook, "", "", ""},
    {"D3D12CreateGraphicsPipelineState", Kind::Hook, "", "", ""},
    {"D3D12CommandListReset", Kind::Hook, "", "", ""},
    {"D3D12ResolveSubresource", Kind::Hook, "", "", ""},
    {"D3D12ClearRenderTargetView", Kind::Hook, "", "", ""},
    {"D3D12SetMarker", Kind::Hook, "", "", ""},
    {"D3D12BeginEvent", Kind::Hook, "", "", ""},
    {"D3D12EndEvent", Kind::Hook, "", "", ""},
    {"D3D12ExecuteIndirect", Kind::Hook, "", "", ""},
    {"D3D12OMSetRenderTargets", Kind::Hook, "", "", ""},
    {"D3D12CommandListClose", Kind::Hook, "", "", ""},
    {"D3D12RSSetViewports", Kind::Hook, "", "", ""},
    {"D3D12ClearDepthStencilView", Kind::Hook, "", "", ""},
    {"NtCreateFile", Kind::Hook, "", "Menu:keys,Menu:titleVersion", ""},
    {"NtOpenFile", Kind::Hook, "", "Menu:keys,Menu:titleVersion", ""},
    {"FreeCamera.CameraPosition", Kind::Patch, "FreeCamera", "Schematica", ""},
    {"FreeCamera.BodyRotation", Kind::Patch, "", "FreeCamera", ""},
    {"FreeCamera.HeadRotation", Kind::Patch, "", "FreeCamera", ""},
    {"FreeCamera.ThirdPerson", Kind::Patch, "", "FreeCamera", ""},
    {"Zoom.Fov", Kind::Patch, "Zoom", "", ""},
    {"NoRender.block", Kind::Patch, "NoRender:block", "", ""},
    {"NoRender.blockEntity", Kind::Patch, "NoRender:blockEntity", "", ""},
    {"NoRender.entity", Kind::Patch, "NoRender:entity", "", ""},
    {"NoRender.sky", Kind::Patch, "NoRender:sky", "", ""},
    {"NoRender.particle", Kind::Patch, "NoRender:particle", "", ""},
    {"NoRender.weather", Kind::Patch, "NoRender:weather", "", ""},
    {"NoRender.nameTag", Kind::Patch, "NoRender:nameTag", "", ""},
    {"NoRender.shadow", Kind::Patch, "NoRender:shadow", "", ""},
    {"NoRender.cursor", Kind::Patch, "NoRender:cursor", "", ""},
};

bool listHas(std::string_view list, std::string_view feature)
{
    std::size_t at = 0;
    while (at <= list.size()) {
        const std::size_t comma = list.find(',', at);
        const std::string_view one = list.substr(at, comma == std::string_view::npos ? list.npos : comma - at);
        if (one == feature) {
            return true;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        at = comma + 1;
    }
    return false;
}

void appendSection(std::string& out, const char* title, Kind kind,
                   const std::map<std::string, bool>& values,
                   const std::vector<std::pair<std::string, bool>>& unknown)
{
    out += "  \"";
    out += title;
    out += "\": {\n";
    std::vector<const Entry*> rows;
    for (const Entry& e : kTable) {
        if (e.kind == kind) {
            rows.push_back(&e);
        }
    }
    std::sort(rows.begin(), rows.end(), [](const Entry* a, const Entry* b) {
        return std::string_view(a->name) < std::string_view(b->name);
    });
    const std::size_t total = rows.size() + unknown.size();
    std::size_t written = 0;
    for (const Entry* e : rows) {
        const auto it = values.find(e->name);
        const bool on = (it == values.end()) ? true : it->second;
        out += "    \"";
        out += e->name;
        out += "\": ";
        out += on ? "true" : "false";
        if (++written < total) {
            out += ",";
        }
        std::string note;
        if (*e->required != 0) {
            note += "required by: ";
            note += e->required;
        }
        if (*e->anyOf != 0) {
            if (!note.empty()) {
                note += " / ";
            }
            note += "one of these is enough: ";
            note += e->anyOf;
        }
        if (*e->optional != 0) {
            if (!note.empty()) {
                note += " / ";
            }
            note += "partly lost without it: ";
            note += e->optional;
        }
        if (note.empty()) {
            note = "no feature depends on it (diagnostics / pass-through)";
        }
        out += "   // ";
        out += note;
        out += "\n";
    }
    for (const auto& [name, on] : unknown) {
        out += "    ";
        out += nlohmann::json(name).dump();
        out += ": ";
        out += on ? "true" : "false";
        if (++written < total) {
            out += ",";
        }
        out += "   // unknown name (typo?) - has no effect\n";
    }
    out += "  }";
}

}

const std::vector<Entry>& table()
{
    return kTable;
}

bool blockedBy(std::string_view feature, const std::vector<std::string>& off)
{
    if (feature.empty()) {
        return false;
    }
    const auto isOff = [&off](const char* name) {
        return std::find(off.begin(), off.end(), name) != off.end();
    };
    bool anyOfSeen = false;
    bool anyOfAlive = false;
    for (const Entry& e : kTable) {
        if (listHas(e.required, feature) && isOff(e.name)) {
            return true;
        }
        if (listHas(e.anyOf, feature)) {
            anyOfSeen = true;
            anyOfAlive = anyOfAlive || !isOff(e.name);
        }
    }
    return anyOfSeen && !anyOfAlive;
}

std::string render(const std::vector<std::pair<std::string, bool>>& values,
                   const std::vector<std::pair<std::string, bool>>& unknownHooks,
                   const std::vector<std::pair<std::string, bool>>& unknownPatches)
{
    const std::map<std::string, bool> map(values.begin(), values.end());
    std::string out;
    out += "// Tsukuyomi: turns each write into the game's protected memory on or off.\n";
    out += "//   hooks   = hooks that rewrite the head of a function / patches = patches that rewrite instructions.\n";
    out += "//   Anything set to false is never written into the game's memory. Features that cannot work without\n";
    out += "//   it (\"required by\" in the comments) disappear from the settings screen and cannot be turned on.\n";
    out += "//   Features marked \"partly lost without it\" still show up, but that part does not work.\n";
    out += "//   Changes take effect after restarting the game and injecting again (read once, on injection).\n";
    out += "//   Names missing here are treated as on (true). Names added by a game update are written as true.\n";
    out += "//   Everything defaults to true.\n";
    out += "{\n";
    appendSection(out, "hooks", Kind::Hook, map, unknownHooks);
    out += ",\n";
    appendSection(out, "patches", Kind::Patch, map, unknownPatches);
    out += "\n}\n";
    return out;
}

bool parse(std::string_view text, std::vector<std::pair<std::string, bool>>& hooks,
           std::vector<std::pair<std::string, bool>>& patches, std::vector<std::string>* invalid)
{
    hooks.clear();
    patches.clear();
    if (invalid != nullptr) {
        invalid->clear();
    }
    const nlohmann::json root = nlohmann::json::parse(text.begin(), text.end(), nullptr,  false,
                                                       true);
    if (root.is_discarded() || !root.is_object()) {
        return false;
    }
    const auto read = [&root, invalid](const char* key, std::vector<std::pair<std::string, bool>>& out) {
        const auto it = root.find(key);
        if (it == root.end() || !it->is_object()) {
            return;
        }
        for (const auto& [name, value] : it->items()) {
            if (value.is_boolean()) {
                out.emplace_back(name, value.get<bool>());
            } else if (invalid != nullptr) {
                invalid->push_back(std::string(key) + "." + name);
            }
        }
    };
    read("hooks", hooks);
    read("patches", patches);
    return true;
}

}
