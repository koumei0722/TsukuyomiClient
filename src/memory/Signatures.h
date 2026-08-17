#pragma once

#include <string_view>

namespace tsukuyomi {

enum class Target {
    CameraUpdate,
    PlayerView,
    PacketSend,
    AntiDarkness,
    BuildBlock,
    GetDestroySpeed,
    SetSelectedSlot,
    AbilitiesAccess,
    UseItem,
    UseItemTransaction,
    SetGameMode,
    SwapSlots,
    ItemStackCopyCtor,
    ItemStackAssign,
    ItemStackDtor,
    ItemStackNetIdAssign,
    MakeSwapAction,
    MakeTakeAction,
    MakePlaceAction,
    AddRequestAction,
    BeginRequest,
    EndRequest,
    NotifyInventoryOpen,
    ContainerOpenGetId,
    InventoryContentGetId,
    MobEffectGetId,
    HandleItemStackResponse,
    SendCommandRequest,
    MakeCommandOrigin,
    MakeComplexTransaction,
    MakeInventoryAction,
    DestroyInventoryAction,
    AddInventoryAction,
    SendComplexTransaction,
    InventoryHoveredSlot,
    InventoryHotbarKey,
    MoveInputHandler,
    PlayerRotation,
    PlayerHeadRotation,
    PlayerHeadRotationInput,
    ViewPerspective,
    UiDefLookup,
    OptionRegister,
    UiButtonMappings,
    UiBagLookup,
    UiBagFind,
    UiResolveVar,
    UiEventDispatch,
    UiBindingRead,
    KeybindListBuild,
    ControlsBindingName,
    ControlsRowBindings,
    ControlsSectionSetup,
    OreFacetBind,
    OreKeyboardInputGroup,
    KeyActionName,
    KeyRowListBuild,
    OreKeyRowsBuild,
    KeyBindingLookup,
    OreKeyRowsWrap,
    OreKeyRowsConsume,
    RowDataCandA,
    RowDataCandB,
    OreKeyNameToIndex,
    I18nAnchor,
    KeyDisplayName,
    SettingsGroupRegister,
    SettingsProviderCall,
    SettingsGroupInfoUpdate,
    SettingsFindComponent,
    GameAllocate,
    PlaySound,
    MakeOptionElement,
    ViewVector,
    SenderVtableRef,
    PlayerVtableRef,
    OwnControllerVtableRef,
    NetManagerVtableRef,
    KeyResetVisibleVtableRef,
    Count,
};

struct TargetInfo {
    Target target;
    const wchar_t* name;
    const wchar_t* purpose;
    std::string_view signature;
};

inline constexpr TargetInfo kTargets[] = {

    {Target::CameraUpdate, L"CameraUpdate", L"camera position writes (FreeCamera)",
     "E9 ? ? ? ? C7 ? 50 00 00 00 00 F3 0F 10 ? 40 F3 0F 11 ? 40 "
     "F3 0F 10 ? 44 F3 0F 11 ? 44 F3 0F 10 ? 48 F3 0F 11 ? 48 "
     "F3 0F 10 ? 3C F3 0F 11 ? 3C F3 0F 10 ? 30 F3 0F 11 ? 30"},

    {Target::PlayerView, L"PlayerView", L"player position and view angles",
     "41 57 41 56 56 57 53 48 83 EC 70 4C 89 CE 4C 89 C7 49 89 D7 48 89 CB "
     "F3 0F 10 9C 24 C8 00 00 00"},

    {Target::PacketSend, L"PacketSend", L"packet send (blocked while FreeCamera is on)",
     "48 83 EC 28 44 0F B6 41 28 45 85 C0 74 10 B8 48 00 00 00 41 83 F8 01 74 0A "
     "E8 ? ? ? ? B8 20 00 00 00 4C 8B 01 49 8B 04 00 4C 8B 05 ? ? ? ? "
     "48 83 C4 28 49 FF E0"},

    {Target::AntiDarkness, L"AntiDarkness", L"darkness effect patch site",
     "8B 0C 01 31 C0 3B 0D ? ? ? ? 48 0F 45 C2 48 83 C4 28"},

    {Target::BuildBlock, L"buildBlock", L"block placement (FastBlockPlacement)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 28 01 00 00 48 8D AC 24 80 00 00 00 "
     "48 C7 85 A0 00 00 00 FE FF FF FF 44 89 CB 44 89 C7 49 89 D6"},

    {Target::GetDestroySpeed, L"GetDestroySpeed", L"destroy speed calculation (AutoTool)",
     "55 41 56 56 57 53 48 81 EC C0 00 00 00 48 8D AC 24 80 00 00 00 0F 29 75 30 "
     "48 C7 45 28 FE FF FF FF 48 8B 01 4C 8B 40 08 8B 40 10 4D 8B 50 48 49 8B 50 50"},

    {Target::SetSelectedSlot, L"SetSelectedSlot", L"selected slot control (AutoTool)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 88 02 00 00 48 8D AC 24 80 00 00 00 "
     "48 C7 85 00 02 00 00 FE FF FF FF 89 D6 83 FA 08 0F 87"},

    {Target::AbilitiesAccess, L"AbilitiesAccess", L"ability lookup (CreativeNoClip)",
     "56 57 55 53 48 83 EC 28 48 83 F9 01 0F 84 ? ? ? ? 48 83 F9 02"},

    {Target::UseItem, L"useItem", L"item use (FastRightClick)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 28 03 00 00 48 8D AC 24 80 00 00 00 "
     "48 C7 85 A0 02 00 00 FE FF FF FF 48 89 D6 48 89 CF 48 8D 8D E8 00 00 00 "
     "E8 ? ? ? ? 48 8D 8D 68 01 00 00 E8 ? ? ? ? 0F 57 C0"},

    {Target::UseItemTransaction, L"useItemTransaction",
     L"item use request sent to the server (FastRightClick)",
     "55 41 57 41 56 56 57 53 48 81 EC 28 01 00 00 48 8D AC 24 80 00 00 00 "
     "48 C7 85 A0 00 00 00 FE FF FF FF 48 89 D3 48 89 CE 48 8B 0D ? ? ? ? "
     "48 8B 01 48 8B 40 08 BA 08 01 00 00"},

    {Target::SetGameMode, L"SetGameMode", L"game mode change (GameModeSwitch)",
     "41 57 41 56 56 57 53 48 83 EC 40 44 89 C3 89 D7 48 89 CE"},

    {Target::SwapSlots, L"swapSlots", L"inventory slot swap (HandRestock)",
     "4C 8B 89 98 01 00 00 49 63 C0 48 69 C0 98 00 00 00 4C 01 C8 48 63 CA "
     "48 69 C9 98 00 00 00"},

    {Target::ItemStackCopyCtor, L"ItemStack::ItemStack(const&)",
     L"copy-construct an item stack (ItemStackOps)",
     "55 56 57 48 83 EC 60 48 8D 6C 24 60 48 C7 45 F8 FE FF FF FF 48 89 D6 "
     "48 8D 05 ? ? ? ? 48 89 01 48 8D 41 08 48 89 45 E8 0F 57 C0 0F 11 41 08"},

    {Target::ItemStackAssign, L"ItemStack::operator=", L"assign an item stack (ItemStackOps)",
     "56 57 48 83 EC 28 48 89 D7 48 89 CE 0F B6 42 22 88 41 22 0F B7 42 20"},

    {Target::ItemStackDtor, L"ItemStack::~ItemStack", L"destroy an item stack (ItemStackOps)",
     "56 57 48 83 EC 28 48 89 CE 48 8D 05 ? ? ? ? 48 89 01 48 8B 49 78 48 85 C9 74 ? "
     "48 8B 01 48 8B 00 BA 01 00 00 00 FF 15 ? ? ? ? 48 8B 4E 50"},

    {Target::ItemStackNetIdAssign, L"ItemStackNetIdVariant::operator=",
     L"assign an item stack net id (ItemStackOps)",
     "56 57 53 48 83 EC 20 48 8B 32 48 0F BE 46 10 4C 8D 0D ? ? ? ? 49 63 0C 89"},

    {Target::MakeSwapAction, L"makeSwapAction", L"build a swap request action (reserved)",
     "41 56 56 57 53 48 83 EC 38 4C 89 C3 49 89 D6 48 89 CE 48 8B 0D ? ? ? ? "
     "48 8B 01 48 8B 40 08 BA 68 00 00 00"},

    {Target::MakeTakeAction, L"makeTakeAction", L"build a take request action (ItemStackRequest)",
     "41 57 41 56 56 57 53 48 83 EC 30 4C 89 CF 4D 89 C6 49 89 D7 48 89 CE "
     "48 8B 0D ? ? ? ? 48 8B 01 48 8B 40 08 BA 68 00 00 00 FF 15 ? ? ? ? "
     "48 89 C3 48 85 C0 75 3E 48 8D 05 ? ? ? ? 48 89 44 24 20 "
     "48 C7 44 24 28 68 00 00 00 48 8D 0D ? ? ? ? 48 8D 15 ? ? ? ? "
     "4C 8D 0D ? ? ? ? 41 B8 25 00 00 00 E8 ? ? ? ? 84 C0 74 05 E8 ? ? ? ? "
     "41 0F B6 07 C6 43 08 00"},

    {Target::MakePlaceAction, L"makePlaceAction", L"build a place request action (ItemStackRequest)",
     "41 57 41 56 56 57 53 48 83 EC 30 4C 89 CF 4D 89 C6 49 89 D7 48 89 CE "
     "48 8B 0D ? ? ? ? 48 8B 01 48 8B 40 08 BA 68 00 00 00 FF 15 ? ? ? ? "
     "48 89 C3 48 85 C0 75 3E 48 8D 05 ? ? ? ? 48 89 44 24 20 "
     "48 C7 44 24 28 68 00 00 00 48 8D 0D ? ? ? ? 48 8D 15 ? ? ? ? "
     "4C 8D 0D ? ? ? ? 41 B8 25 00 00 00 E8 ? ? ? ? 84 C0 74 05 E8 ? ? ? ? "
     "41 0F B6 07 C6 43 08 01"},

    {Target::AddRequestAction, L"addRequestAction", L"queue a request action (HandRestock/BDS)",
     "55 48 83 EC 40 48 8D 6C 24 40 48 C7 45 F8 FE FF FF FF 48 8B 09 48 8B 02 "
     "48 89 55 F0 48 C7 02 00 00 00 00"},

    {Target::BeginRequest, L"_beginRequest", L"open an item stack request (HandRestock/BDS)",
     "55 41 57 41 56 56 57 53 48 83 EC 68 48 8D 6C 24 60 48 C7 45 00 FE FF FF FF "
     "48 89 CE 48 8B 41 38 48 8B 48 20 48 85 C9 0F 84"},

    {Target::EndRequest, L"endAndSendRequest", L"close and queue the request (HandRestock/BDS)",
     "55 41 56 56 57 53 48 83 EC 50 48 8D 6C 24 50 48 C7 45 F8 FE FF FF FF 48 89 CE "
     "80 79 08 01 0F 85 ? ? ? ? 48 8B 3E 48 8D 55 F0 48 89 F9 E8 ? ? ? ? 4C 8B 75 F0"},

    {Target::NotifyInventoryOpen, L"notifyInventoryOpen",
     L"tell the server the inventory opened (ItemStackRequest)",
     "55 41 56 56 57 53 48 81 EC 90 00 00 00 48 8D AC 24 80 00 00 00 "
     "48 C7 45 08 FE FF FF FF 89 D7 48 89 CE 48 8B 01 48 8B 80 F8 00 00 00 "
     "FF 15 ? ? ? ? 48 85 C0 0F 84 ? ? ? ? 48 89 C3 48 8B 06"},

    {Target::MoveInputHandler, L"moveInputHandler",
     L"skip the input reset while we tell the server the inventory opened",
     "48 83 EC 28 48 8B 01 48 8B 80 F8 00 00 00 FF 15 ? ? ? ? 48 85 C0 "
     "0F 84 ? ? ? ? 48 8B 50 10 8B 48 18 8B 42 50 4C 8B 42 48 44 29 C0"},

    {Target::PlayerRotation, L"PlayerRotation",
     L"player body pitch/yaw writes (FreeCamera freezes the body)",
     "48 8B B6 28 02 00 00 F3 0F 10 44 24 2C F3 0F 11 46 04 "
     "F3 0F 10 4C 24 28 F3 0F 11 0E F3 0F 10 3D"},

    {Target::PlayerHeadRotation, L"PlayerHeadRotation",
     L"player facing write (FreeCamera freezes the head)",
     "81 E2 FF FF 03 00 83 E2 7F F3 0F 11 0C D0 F3 44 0F 11 4C D0 04 48 89 F1"},

    {Target::PlayerHeadRotationInput, L"PlayerHeadRotationInput",
     L"the other player facing write (FreeCamera freezes the head)",
     "81 E2 FF FF 03 00 83 E2 7F F3 0F 11 34 D0 F3 0F 11 44 D0 04 48 8B 8E D8 01 00 00"},

    {Target::ViewPerspective, L"getViewPerspective",
     L"camera perspective getter (FreeCamera forces third person)",
     "48 83 EC 38 48 8B 05 ? ? ? ? 48 31 E0 48 89 44 24 30 48 8B 01 48 8B 40 08 "
     "48 8D 54 24 28 41 B8 03 00 00 00 FF 15 ? ? ? ? 48 8B 4C 24 28"},

    {Target::ContainerOpenGetId, L"ContainerOpen::getId",
     L"locate the container-open handler (ItemStackRequest)", "B8 2E 00 00 00 C3 CC CC"},

    {Target::InventoryContentGetId, L"InventoryContent::getId",
     L"read the net ids the server sends back (OffhandSwap)", "B8 31 00 00 00 C3 CC CC"},

    {Target::MobEffectGetId, L"MobEffect::getId",
     L"drop the darkness effect on the client only (AntiDarkness)", "B8 1C 00 00 00 C3 CC CC"},

    {Target::HandleItemStackResponse, L"handleItemStackResponse",
     L"read the server's verdict on our request (ItemStackRequest)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC C8 02 00 00 48 8D AC 24 80 00 00 00 "
     "0F 29 B5 30 02 00 00 48 C7 85 28 02 00 00 FE FF FF FF 48 89 8D 90 00 00 00 "
     "C6 41 58 01 48 8B 32 48 8B 42 08 48 89 45 08"},

    {Target::SendCommandRequest, L"sendCommandRequest", L"run a chat command (GameModeSwitch)",
     "55 41 57 41 56 41 54 56 57 53 48 81 EC 90 01 00 00 48 8D AC 24 80 00 00 00 "
     "48 C7 85 08 01 00 00 FE FF FF FF 45 89 CE 4C 89 C7 48 89 D6 48 89 CB 48 8B 49 10 "
     "4C 89 C2 66 41 B8 40 00"},

    {Target::MakeCommandOrigin, L"makeCommandOrigin",
     L"build a command origin (GameModeSwitch)",
     "41 57 41 56 41 55 41 54 56 57 55 53 48 83 EC 28 48 89 CE 4C 8B B2 D8 01 00 00 "
     "48 89 D1 E8 ? ? ? ? 4C 8B 38 48 8D 05 ? ? ? ? 48 89 06 E8 ? ? ? ? 89 C2 0F B6 CC "
     "0F B6 D8 C1 E8 18 48 C1 E0 38 C1 EA 10 0F B6 D2 48 C1 E2 30 48 09 C2 48 C1 E1 28 "
     "48 09 D1 48 C1 E3 20 48 09 CB E8 ? ? ? ? 89 C7 41 89 C4 49 09 DC E8 ? ? ? ? "
     "89 C3 41 89 C5 41 C1 ED 18 89 C5 C1 ED 10 E8 ? ? ? ? 89 C2 81 E2 00 00 00 FF "
     "48 B9 ? ? ? ? ? ? ? ? 4C 21 E1 81 E7 FF FF FF 00 48 09 CF 48 B9 ? ? ? ? ? ? ? ? "
     "48 09 F9 49 C1 E5 38 44 0F B6 C5 49 C1 E0 30 4D 09 E8 0F B6 FF 48 C1 E7 28 "
     "4C 09 C7 44 0F B6 C3 49 C1 E0 20 49 09 F8 4C 09 C2 41 89 C0 41 81 E0 00 00 FF 00 "
     "49 09 D0 0F B6 D0 25 00 0F 00 00 4C 09 C0 81 CA 00 40 00 00 48 09 C2 48 89 56 08 "
     "48 89 4E 10 48 8D 05 ? ? ? ? 48 89 06"},

    {Target::MakeComplexTransaction, L"makeComplexTransaction",
     L"create an inventory transaction (LegacyTransaction)",
     "55 56 57 53 48 83 EC 48 48 8D 6C 24 40 48 C7 45 00 FE FF FF FF 48 89 CE 83 FA 04 "
     "0F 87 ? ? ? ? 89 D0"},

    {Target::MakeInventoryAction, L"makeInventoryAction",
     L"build one inventory action (LegacyTransaction)",
     "55 41 56 56 57 53 48 83 EC 50 48 8D 6C 24 50 48 C7 45 F8 FE FF FF FF 4C 89 CB "
     "48 89 CE 48 8B 7D 50 8B 42 08"},

    {Target::DestroyInventoryAction, L"destroyInventoryAction",
     L"release one inventory action (LegacyTransaction)",
     "56 57 53 48 83 EC 30 48 89 CE 48 8D B9 68 01 00 00 48 8D 1D ? ? ? ? "
     "48 89 99 68 01 00 00"},

    {Target::AddInventoryAction, L"addInventoryAction",
     L"append an action to a transaction (LegacyTransaction)",
     "55 41 57 41 56 41 54 56 57 53 48 81 EC 60 02 00 00 48 8D AC 24 80 00 00 00 "
     "48 C7 85 D8 01 00 00 FE FF FF FF 48 89 D7 48 89 CE 8B 02 41 89 C7 41 C1 E7 10"},

    {Target::SendComplexTransaction, L"sendComplexInventoryTransaction",
     L"send an inventory transaction (LegacyTransaction)",
     "55 56 48 81 EC 08 03 00 00 48 8D AC 24 80 00 00 00 48 C7 85 80 02 00 00 FE FF FF FF "
     "48 89 95 78 02 00 00 48 89 CE 48 8B 89 D8 01 00 00"},

    {Target::InventoryHoveredSlot, L"inventoryHoveredSlot",
     L"locate the inventory screen controller (InventoryScreen)",
     "41 56 56 57 55 53 48 83 EC 30 48 89 D6 48 89 CB 44 8B 05 ? ? ? ? "
     "48 8D 3D ? ? ? ? 48 89 FA"},

    {Target::InventoryHotbarKey, L"inventoryHotbarKey",
     L"number-key swap in the inventory screen (OffhandSwap)",
     "55 41 57 41 56 41 54 56 57 53 48 81 EC F0 00 00 00 48 8D AC 24 80 00 00 00 "
     "48 C7 45 68 FE FF FF FF 44 89 C7 48 89 D3 48 89 CE"},

    {Target::UiDefLookup, L"uiDefLookup", L"look a UI definition up by namespace and name",
     "41 57 41 56 41 55 41 54 56 57 55 53 48 83 EC 38 4C 89 C6 48 89 D3 48 8B 05 "
     "? ? ? ? 48 31 E0 48 89 44 24 30 4C 8B 72 10"},

    {Target::OptionRegister, L"optionRegister", L"register an option name against its id",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 78 01 00 00 48 8D AC 24 80 00 00 00 "
     "48 C7 85 F0 00 00 00 FE FF FF FF 4D 8B 70 10 48 B8 ? ? ? ? ? ? ? ? 49 39 C6"},

    {Target::UiButtonMappings, L"uiButtonMappings", L"parse button_mappings into a container",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 98 02 00 00 48 8D AC 24 80 00 00 00 "
     "48 C7 85 10 02 00 00 FE FF FF FF 4C 89 C6 48 89 95 D8 01 00 00 48 89 8D 78 01 00 00 "
     "48 8B 0D"},

    {Target::UiBagLookup, L"uiBagLookup", L"look up a child slot in a UI value record by key",
     "48 89 5C 24 10 48 89 6C 24 18 56 57 41 54 41 56 41 57 48 81 EC 80 00 00 00 "
     "4C 8B F2 48 8B D9 80 79 08 07 74 21 B2 07 48 8D 4C 24 20"},

    {Target::UiBagFind, L"uiBagFind", L"find a child slot in a UI value record (read-only)",
     "48 89 5C 24 18 48 89 6C 24 20 41 56 48 83 EC 20 80 79 08 07 4C 8B F2 0F 85"},

    {Target::UiResolveVar, L"uiResolveVar", L"resolve a $-prefixed value as a variable",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 83 EC 68 48 8D 6C 24 60 "
     "48 C7 45 00 FE FF FF FF 48 8B 71 08 48 8B 1A 48 8B 7A 08 80 3B 24"},

    {Target::UiEventDispatch, L"uiEventDispatch", L"dispatch a UI event to the handler list",
     "41 56 56 57 55 53 48 83 EC 20 48 89 D6 48 89 CF "
     "4C 8B B1 A8 09 00 00 48 8B 99 B0 09 00 00 49 39 DE"},

    {Target::UiBindingRead, L"uiBindingRead", L"read a binding value out of a UI property bag",
     "55 41 57 41 56 56 57 53 48 81 EC 18 01 00 00 48 8D AC 24 80 00 00 00 "
     "48 C7 85 90 00 00 00 FE FF FF FF 4C 89 CE 48 89 D7 49 89 CE 49 8B 18 4D 8B 78 08"},

    {Target::KeybindListBuild, L"keybindListBuild", L"build the game's key mapping list",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 68 04 00 00 "
     "48 8D AC 24 80 00 00 00 44 0F 29 B5 D0 03 00 00 44 0F 29 AD C0 03 00 00"},

    {Target::ControlsBindingName, L"controlsBindingName",
     L"format the currently bound key for the Controls screen",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC F8 00 00 00 "
     "48 8D AC 24 80 00 00 00 48 C7 45 70 FE FF FF FF 44 89 C6 48 89 CF 48 8B 49 10"},

    {Target::ControlsRowBindings, L"controlsRowBindings",
     L"register the keymapping row bindings for a collection",
     "41 57 41 56 56 57 53 48 81 EC B0 02 00 00 4C 89 C7 48 89 D3 48 89 CE"},

    {Target::ControlsSectionSetup, L"controlsSectionSetup",
     L"build the Controls settings section",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 08 06 00 00 "
     "48 8D AC 24 80 00 00 00 0F 29 B5 70 05 00 00 48 C7 85 68 05 00 00 FE FF FF FF "
     "4C 89 C6 48 89 CB 0F 57 C0"},

    {Target::OreFacetBind, L"oreFacetBind", L"look up an Ore UI facet by name",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 58 02 00 00 "
     "48 8D AC 24 80 00 00 00 48 C7 85 D0 01 00 00 FE FF FF FF 44 89 CF 48 89 D3 48 89 CE"},

    {Target::OreKeyboardInputGroup, L"oreKeyboardInputGroup",
     L"produce the keyboard input group facet",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 48 07 00 00 "
     "48 8D AC 24 80 00 00 00 48 C7 85 C0 06 00 00 FE FF FF FF 48 8B 49 08 0F 57 C0 0F 11 02"},

    {Target::KeyActionName, L"keyActionName", L"map a key action index to its id string",
     "41 57 41 56 41 54 56 57 53 48 83 EC 38 48 89 CE 48 63 C2 48 C1 E0 04 48 8D 0D"},

    {Target::KeyRowListBuild, L"keyRowListBuild", L"fill the key mapping row list",
     "55 41 56 56 57 53 48 81 EC 80 00 00 00 48 8D AC 24 80 00 00 00 "
     "48 C7 45 F8 FE FF FF FF 48 89 D7 48 89 CB 4C 8D 35 ? ? ? ? 4C 89 F1 E8"},

    {Target::OreKeyRowsBuild, L"oreKeyRowsBuild", L"build the Controls screen key rows",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC C8 00 00 00 48 8D AC 24 80 00 00 00 "
     "48 C7 45 40 FE FF FF FF 4C 89 45 C0 48 89 D7 48 89 CB"},

    {Target::KeyBindingLookup, L"keyBindingLookup", L"find a key binding by its id string",
     "55 41 56 56 57 53 48 83 EC 30 48 8D 6C 24 30 48 C7 45 F8 FE FF FF FF "
     "48 8B 71 08 4C 8B 71 10 4C 39 F6 74 ? 48 89 D7 48 8B 5A 10 48"},

    {Target::OreKeyRowsWrap, L"oreKeyRowsWrap", L"wrapper that builds the key row list",
     "55 56 48 83 EC 68 48 8D 6C 24 60 48 C7 45 00 FE FF FF FF 48 89 CE 48 8D 05 "
     "? ? ? ? 48 89 45 C0 4C 8D 45 C0 4C 89"},

    {Target::OreKeyRowsConsume, L"oreKeyRowsConsume", L"consume the key row list",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 68 01 00 00 48 8D AC 24 80 00 00 00 "
     "0F 29 B5 D0 00 00 00 48 C7 85 C8 00 00 00 FE FF FF FF 48 89 D6 48 89 CF "
     "48 8D 5D D0 45 31 F6 4C 8D 7D 30"},

    {Target::RowDataCandA, L"rowDataCandA", L"row data candidate A (0xA0FC90)",
     "55 56 57 53 48 83 EC 58 48 8D 6C 24 50 48 C7 45 00 FE FF FF FF "
     "48 8B 41 08 48 83 B8 98 01 00 00 00 74 ? 48 89 D7"},
    {Target::RowDataCandB, L"rowDataCandB", L"row data candidate B (0xD43EF0)",
     "48 83 EC 28 48 8B 49 08 48 8B 01 48 8B 80 40 05 00 00 FF 15 ? ? ? ? 48 85 C0"},

    {Target::OreKeyNameToIndex, L"oreKeyNameToIndex", L"look up a key action index by name",
     "41 57 41 56 41 55 41 54 56 57 53 48 83 EC 50 0F 57 C0 0F 29 44 24 40 "
     "0F 29 44 24 30 48 8B 71 08 48 85 F6 0F 88"},

    {Target::I18nAnchor, L"i18nAnchor", L"anchor to resolve the localization function",
     "48 B8 67 75 69 2E 64 6F 6E 65 48 89 45 D0 48 8D 0D ? ? ? ? "
     "48 8B 05 ? ? ? ? 48 8B 80 80 00 00 00 48 8D 55 F0"},

    {Target::KeyDisplayName, L"keyDisplayName", L"key code -> display name (vanilla wording)",
     "55 41 57 41 56 41 54 56 57 53 48 81 EC F0 00 00 00 48 8D AC 24 80 00 00 00 "
     "48 C7 45 68 FE FF FF FF 48 89 D6 45 85 C0 74 ? 45 89 C6"},

    {Target::SettingsGroupRegister, L"settingsGroupRegister",
     L"register an Ore UI settings group (id + provider)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 83 EC 68 48 8D 6C 24 60 "
     "48 C7 45 00 FE FF FF FF 0F 57 C0 0F 29 45 F0 0F 29 45 E0 48 8B 5A 08 48 85 DB 0F 88"},

    {Target::SettingsProviderCall, L"settingsProviderCall",
     L"std::function::_Do_call that builds a settings group's item vector",
     "55 41 56 56 57 53 48 81 EC 90 00 00 00 48 8D AC 24 80 00 00 00 "
     "48 C7 45 08 FE FF FF FF 49 89 D1 48 8B 71 08 0F 57 C0"},

    {Target::SettingsGroupInfoUpdate, L"settingsGroupInfoUpdate",
     L"SettingsGroupInfoQuery value updater (id -> component -> name/state)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 18 01 00 00 "
     "48 8D AC 24 80 00 00 00 48 C7 85 90 00 00 00 FE FF FF FF 48 89 CE "
     "48 8B B9 30 01 00 00 0F 57 C0 0F 29 45 00 0F 29 45 F0 "
     "4C 8B A1 80 01 00 00 48 83 B9 88 01 00 00 10 72 09 48 8B 9E 70 01 00 00 "
     "EB 07 48 8D 9E 70 01 00 00 4D 85 E4 0F 88 8B 0C 00 00"},

    {Target::SettingsFindComponent, L"settingsFindComponent",
     L"Settings::IRegistry::find(component by id)",

     "41 57 41 56 41 55 41 54 56 57 55 53 48 83 EC 38 4C 89 44 24 30 "
     "48 89 D6 48 89 CB 8B 05 ?? ?? ?? ?? 8B 0D ?? ?? ?? ??"},

    {Target::GameAllocate, L"gameAllocate", L"Bedrock allocator: allocate(size)",

     "48 89 D1 48 83 FA 01 48 83 D1 00 48 FF 25 ?? ?? ?? ??"},

    {Target::PlaySound, L"playSound", L"Sound player used by the settings UI",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 28 03 00 00 48 8D AC 24 80 00 00 00 "
     "0F 29 BD 90 02 00 00 0F 29 B5 80 02 00 00 48 C7 85 78 02 00 00 FE FF FF FF "
     "0F 28 F3 F3 0F 10 05 ?? ?? ?? ?? 0F 28 CB F3 0F 59 C8"},

    {Target::MakeOptionElement, L"makeOptionElement", L"Ore UI: build one option element",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 83 EC 68 48 8D 6C 24 60 48 C7 45 00 FE FF FF FF "
     "4C 89 CE 4C 89 C7 89 11 48 8D 41"},

    {Target::ViewVector, L"viewVector", L"Actor::getViewVector(partialTick)",
     "41 56 56 57 53 48 81 EC 98 00 00 00 44 0F 29 A4 24 80 00 00 00 44 0F 29 5C 24 70 "
     "44 0F 29 54 24 60 44 0F 29 4C 24 50 44 0F 29 44 24 40 0F 29 7C 24 30 0F 29 74 24 20 "
     "0F 28 F2 48 89 D6 48 8B 51 10 8B 41 18 48 8B 4A 48 4C 8B 42 50 49 29 C8 49 C1 E8 03 "
     "41 FF C8 41 81 E0 B7 36 DF 75 4E 8D 0C"},

    {Target::SenderVtableRef, L"senderVtableRef", L"lea of the command sender vtable",
     "48 8D 05 ?? ?? ?? ?? 48 89 01 48 8B 02 48 89 41 08 48 8D 41 10 48 89"},

    {Target::PlayerVtableRef, L"playerVtableRef", L"lea of the Player (LocalPlayer) vtable",
     "48 8D 05 ?? ?? ?? ?? 49 89 06 49 8D 8E B8 0C 00 00"},

    {Target::OwnControllerVtableRef, L"ownControllerVtableRef",
     L"lea of the inventory controller vtable",
     "1D 00 00 48 8D 0D ?? ?? ?? ?? 48 89 0F 48"},

    {Target::NetManagerVtableRef, L"netManagerVtableRef", L"lea of the net manager vtable",
     "54 56 57 53 48 83 EC 28 89 D7 48 89 CE 48 8D 05 ?? ?? ?? ?? 48 89 01 48 8B 49 78 48 85 C9"},

    {Target::KeyResetVisibleVtableRef, L"keyResetVisibleVtableRef",
     L"lea of the reset-button visibility callable vtable",
     "48 8D 05 ?? ?? ?? ?? 48 89 85 10 05 00 00 48 8B 8D 58 05 00 00 48 89"},
};

static_assert(std::size(kTargets) == static_cast<size_t>(Target::Count),
              "kTargets と Target の要素数が一致していません");

}
