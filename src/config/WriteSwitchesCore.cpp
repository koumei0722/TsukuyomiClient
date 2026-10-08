#include "config/WriteSwitches.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <set>

namespace tsukuyomi::writes {

namespace {

constexpr const char* kPlayerViewNeeds =
    "AntiEffect,AutoTool,CreativeNoClip,FastBlockPlacement,FlySpeed,FreeCamera,Fullbright,HandRestock,"
    "OffhandSwap,Scaffold,Schematica,NoRender:fog,DebugScreen,ChatCommands,AppleSkin,ArmorHUD";

const std::vector<Entry> kTable = {
    {"AbilitiesAccess", Kind::Hook, "CreativeNoClip,FlySpeed", "", ""},
    {"AttackCore", Kind::Hook, "AutoTool:attack", "", ""},
    {"BeRenderLoop", Kind::Hook, "DebugScreen,Schematica", "", ""},
    {"BlockRenderLookup", Kind::Hook, "Schematica", "", ""},
    {"BlockSourceGetBlock", Kind::Hook, "Schematica", "", ""},
    {"BlockSourceGetExtra", Kind::Hook, "", "Schematica", ""},
    {"BlockSourceSetBlock", Kind::Hook, "Schematica", "", ""},
    {"BlockTessellate", Kind::Hook, "Schematica", "", ""},
    {"CameraUpdate", Kind::Hook, "FreeCamera,Zoom", "Schematica", ""},
    {"ChunkCoordinatorFrame", Kind::Hook, "Schematica", "", ""},
    {"ChunkMeshBuild", Kind::Hook, "Schematica", "", ""},
    {"ChunkVisibilityScan", Kind::Hook, "Schematica", "", ""},
    {"CommandAutoComplete", Kind::Hook, "", "ChatCommand:complete", ""},
    {"CommandAutoCompleteFilter", Kind::Hook, "", "ChatCommand:complete", ""},
    {"CommandRegistryLoadPacket", Kind::Hook, "", "ChatCommand:complete", ""},
    {"ContainerGetItem", Kind::Hook, "ArmorHUD,OffhandSlot,ShulkerPreview:tooltip", "", ""},
    {"ContainerOpenHandle", Kind::Hook, "HandRestock,OffhandSwap", "", ""},
    {"ContainerScreenCtor", Kind::Hook, "ArmorHUD,EffectTimer,InventoryEffects,InventoryHUD,ItemScroller,ShulkerPreview", "", ""},
    {"ContainerScreenDtor", Kind::Hook, "InventoryEffects,ItemScroller,ShulkerPreview", "", ""},
    {"ContainerScreenTick", Kind::Hook, "InventoryEffects,ItemScroller,ShulkerPreview", "", ""},
    {"ContainerSm", Kind::Hook, "ItemScroller", "", ""},
    {"FogSettingsFetch", Kind::Hook, "NoRender:fog", "", ""},
    {"EmoteWheelCtor", Kind::Hook, "DebugScreen:gameModeWheel", "", ""},
    {"EmoteWheelDtor", Kind::Hook, "DebugScreen:gameModeWheel", "", ""},
    {"EmoteWheelSelected", Kind::Hook, "DebugScreen:gameModeWheel", "", ""},
    {"EmoteWheelImage", Kind::Hook, "DebugScreen:gameModeWheel", "", ""},
    {"EmoteWheelImageValid", Kind::Hook, "DebugScreen:gameModeWheel", "", ""},
    {"EmoteWheelFileSystem", Kind::Hook, "DebugScreen:gameModeWheel", "", ""},
    {"EmoteWheelEmoteValid", Kind::Hook, "DebugScreen:gameModeWheel", "", ""},
    {"EmoteWheelName", Kind::Hook, "DebugScreen:gameModeWheel", "", ""},
    {"GameButtonActionName", Kind::Hook, "Hotkeys", "", ""},
    {"GameButtonBindAction", Kind::Hook, "Hotkeys", "", ""},
    {"GameButtonFindKeymap", Kind::Hook, "Hotkeys", "", ""},
    {"GameButtonInputUpdate", Kind::Hook, "Hotkeys", "", ""},
    {"GameButtonPadBind", Kind::Hook, "Hotkeys", "", ""},
    {"GameModeContinueDestroyBlock", Kind::Hook, "FastBlockBreak", "", ""},
    {"GameModeDestroyBlock", Kind::Hook, "FastBlockBreak", "", ""},
    {"GetActorEffect", Kind::Hook, "AntiEffect,Fullbright", "", ""},
    {"GetDestroySpeed", Kind::Hook, "AutoTool", "", ""},
    {"HandleItemStackResponse", Kind::Hook, "HandRestock,OffhandSwap", "", ""},
    {"HeartRendererUpdate", Kind::Hook, "AppleSkin", "", ""},
    {"HitResultAssign", Kind::Hook, "", "DebugScreen,Schematica", ""},
    {"HitResultMoveAssign", Kind::Hook, "", "DebugScreen", ""},
    {"HoverRendererRender", Kind::Hook, "AppleSkin", "", ""},
    {"HudCollectionResolve", Kind::Hook, "", "ShulkerPreview:icons", ""},
    {"HudScreenCtor", Kind::Hook, "ArmorHUD,EffectTimer,InventoryEffects,InventoryHUD,OffhandSlot", "", ""},
    {"HungerRendererUpdate", Kind::Hook, "AppleSkin", "", ""},
    {"I18nGet", Kind::Hook, "", "EffectTimer,InventoryEffects,Menu:settings", ""},
    {"InputGather", Kind::Hook, "FreeCamera,ToggleSneakSprint", "ItemScroller", ""},
    {"InventoryContentRead", Kind::Hook, "OffhandSwap", "HandRestock", ""},
    {"InventoryHoveredSlot", Kind::Hook, "FastInventory", "OffhandSwap", ""},
    {"ItemHoverTextBuild", Kind::Hook, "AppleSkin", "DebugScreen", ""},
    {"KeyBindingUnassignConflicts", Kind::Hook, "AllowDuplicateKeys", "", ""},
    {"KeyBindingUnassignOthers", Kind::Hook, "AllowDuplicateKeys", "", ""},
    {"EnchantCommandExecute", Kind::Hook, "ExtendedEnchantLevel", "", ""},
    {"EnchantLevelRangeCheck", Kind::Hook, "ExtendedEnchantLevel", "", ""},
    {"EnchantCanEnchant", Kind::Hook, "ExtendedEnchantLevel", "", ""},
    {"LegacyParticleInsert", Kind::Hook, "", "NoRender:particle", ""},
    {"LevelBuildDispatch", Kind::Hook, "Schematica", "DebugScreen", ""},
    {"MobSwing", Kind::Hook, "", "FastBlockPlacement,Scaffold", ""},
    {"MobEffectsRendererRender", Kind::Hook, "EffectTimer", "", ""},
    {"MobEffectsLayout", Kind::Hook, "", "EffectTimer", ""},
    {"MoveInputHandler", Kind::Hook, "Schematica:page", "FreeCamera,HandRestock,ItemScroller", ""},
    {"MoveIntentFromInput", Kind::Hook, "FreeCamera", "", ""},
    {"NotifyInventoryOpen", Kind::Hook, "FastInventory,HandRestock,OffhandSwap", "", ""},
    {"OreKeyRowsBuild", Kind::Hook, "", "Menu:keys", ""},
    {"PacketSend", Kind::Hook, "FreeCamera,HandRestock,OffhandSwap", "", ""},
    {"PoseDecision", Kind::Hook, "", "CreativeNoClip:pose", ""},
    {"PlayerView", Kind::Hook, kPlayerViewNeeds, "NoRender:particle,ItemScroller", ""},
    {"NetworkSend", Kind::Hook, "", "DebugScreen", ""},
    {"PacketCheckSize", Kind::Hook, "", "DebugScreen", ""},
    {"RenderCurrentFrame", Kind::Hook, "", "DebugScreen", ""},
    {"ServerLevelTick", Kind::Hook, "", "DebugScreen", ""},
    {"StartGameHandle", Kind::Hook, "", "DebugScreen", ""},
    {"LevelChunkTick", Kind::Hook, "", "DebugScreen", ""},
    {"FmodSystemUpdate", Kind::Hook, "", "DebugScreen", ""},
    {"LegacyParticleRender", Kind::Hook, "", "DebugScreen", ""},
    {"SceneStackPush", Kind::Hook, "", "ItemScroller", ""},
    {"SendCommandRequest", Kind::Hook, "ChatCommands", "", ""},
    {"SendComplexTransaction", Kind::Hook, "AutoTool:attack", "", ""},
    {"SetGameMode", Kind::Hook, "ArmorHUD,EffectTimer,InventoryEffects,OffhandSlot,Schematica", "Scaffold", ""},
    {"SetSelectedSlot", Kind::Hook, "AutoTool,HandRestock,OffhandSwap", "Zoom", ""},
    {"SettingsFindComponent", Kind::Hook, "", "Menu:settings", ""},
    {"SettingsGroupRegister", Kind::Hook, "", "Menu:settings", ""},
    {"SettingsProviderCall", Kind::Hook, "", "Menu:settings", ""},
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
    {"UiDefLookup", Kind::Hook, "ArmorHUD,EffectTimer,InventoryEffects,InventoryHUD,OffhandSlot,Schematica:page,ShulkerPreview:icons,ShulkerPreview:tooltip", "ItemScroller", ""},
    {"UiEventDispatch", Kind::Hook, "Schematica:page", "", ""},
    {"UiSliderPublish", Kind::Hook, "", "Schematica:page", ""},
    {"ViewVector", Kind::Hook, "", "FreeCamera,Scaffold", ""},
    {"VisibilityGate", Kind::Hook, "Schematica", "", ""},
    {"buildBlock", Kind::Hook, "FastBlockPlacement,Scaffold", "", ""},
    {"useItem", Kind::Hook, "", "", "FastUseItem"},
    {"useItemTransaction", Kind::Hook, "", "", "FastUseItem"},
    {"PeekMessageW", Kind::Hook, "", "Menu:settings", ""},
    {"PlaySound", Kind::Hook, "", "Menu:sound", ""},
    {"Present", Kind::Hook, "", "Schematica", ""},
    {"NameTagStage", Kind::Hook, "Schematica:boxes", "", ""},
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
    {"NoRender.item", Kind::Patch, "NoRender:item", "", ""},
    {"NoRender.fog", Kind::Patch, "NoRender:fog", "", ""},
    {"NoRender.sky", Kind::Patch, "NoRender:sky", "", ""},
    {"NoRender.particle", Kind::Patch, "NoRender:particle", "", ""},
    {"NoRender.weather", Kind::Patch, "NoRender:weather", "", ""},
    {"NoRender.nameTag", Kind::Patch, "NoRender:nameTag", "", ""},
    {"NoRender.shadow", Kind::Patch, "NoRender:shadow", "", ""},
    {"NoRender.cursor", Kind::Patch, "NoRender:cursor", "", ""},
    {"NoBlockAnimation.Render", Kind::Patch, "NoBlockAnimation", "", ""},
    {"ExtendedFov.RenderClamp", Kind::Patch, "ExtendedFov", "", ""},
    {"ExtendedStructureSize.Screen", Kind::Patch, "ExtendedStructureSize", "", ""},
    {"ExtendedStructureSize.Server", Kind::Patch, "ExtendedStructureSize", "", ""},
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
                   const std::map<std::string, bool>& values)
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
    const std::size_t total = rows.size();
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

std::string render(const std::vector<std::pair<std::string, bool>>& values)
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
    appendSection(out, "hooks", Kind::Hook, map);
    out += ",\n";
    appendSection(out, "patches", Kind::Patch, map);
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
    for (const char* key : {"hooks", "patches"}) {
        if (const auto it = root.find(key); it != root.end() && !it->is_object()) {
            return false;
        }
    }
    const auto read = [&root, invalid](const char* key, std::vector<std::pair<std::string, bool>>& out) {
        const auto it = root.find(key);
        if (it == root.end()) {
            return;
        }
        for (const auto& [name, value] : it->items()) {
            if (value.is_boolean()) {
                out.emplace_back(name, value.get<bool>());
            } else {
                out.emplace_back(name, true);
                if (invalid != nullptr) invalid->push_back(std::string(key) + "." + name);
            }
        }
    };
    read("hooks", hooks);
    read("patches", patches);
    return true;
}

}
