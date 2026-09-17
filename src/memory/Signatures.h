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
    UiSliderPublish,
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
    KeyBindingIsDefault,
    SettingsActionData,
    SettingsActionQueryUpdate,
    BlockRegistryRef,
    SubChunkSetBlock,
    BlockSourceSetBlock,
    BlockCollisionQuery,
    BlockRenderLookup,
    BlockTessellate,
    BlockTessellateCube,
    BlockTessellateShape0,
    BlockBuildFaces,
    SettingsTabList,
    SettingsAddTab,
    SettingsInvokeAction,
    OpenHowToPlayScreen,
    HitResultAssign,
    ModelPartDraw,
    BeDispatch,
    AlphaBlendName,
    AlphaTestName,
    SubmitDraw,
    ChunkVisibilityScan,
    BeRenderLoop,
    VisibilityGate,
    ChunkBuildLookup,
    ScheduleChunkBuild,
    LevelBuildDispatch,
    ChunkMeshBuild,
    TextureLookup,
    PackStackOperation,
    BlockTransform,
    NameTagStage,

    NameTagStageCaller,

    BlockOutlineDraw,

    FogSettingsFetch,

    ItemRegistryLookupByName,
    TessellatorBegin,
    TessellatorVertex,
    TessellatorColor,
    RenderMeshImmediately,
    MaterialPtrCtorSite,
    GetActorEffect,
    CameraFovStore,
    OpenInventoryScreen,
    HotbarSelectTick,
    MoveVector,
    MoveApply,
    MoveIntentFromInput,
    InputGather,
    BodyPosWrite,
    MoveBox,
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
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 58 01 00 00 48 8D AC 24 80 00 00 00 48 "
     "C7 85 D0 00 00 00 FE FF FF FF 44 89 CB 44 88 85 CE 00 00 00 49 89 D7 48 89 CE"},

    {Target::GetDestroySpeed, L"GetDestroySpeed", L"destroy speed calculation (AutoTool)",
     "55 41 56 56 57 53 48 81 EC C0 00 00 00 48 8D AC 24 80 00 00 00 0F 29 75 30 "
     "48 C7 45 28 FE FF FF FF 48 8B 01 4C 8B 40 08 8B 40 10 4D 8B 50 48 49 8B 50 50"},

    {Target::SetSelectedSlot, L"SetSelectedSlot", L"selected slot control (AutoTool)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 88 02 00 00 48 8D AC 24 80 00 00 00 "
     "48 C7 85 00 02 00 00 FE FF FF FF 89 D6 83 FA 08 0F 87"},

    {Target::AbilitiesAccess, L"AbilitiesAccess", L"ability lookup (CreativeNoClip)",
     "56 57 55 53 48 83 EC 28 48 83 F9 01 0F 84 ? ? ? ? 48 83 F9 02"},

    {Target::UseItem, L"useItem", L"item use (FastRightClick)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 38 03 00 00 48 8D AC 24 80 00 00 00 48 "
     "C7 85 B0 02 00 00 FE FF FF FF 44 88 85 AF 02 00 00 48 89 D6 48 89 CF"},

    {Target::UseItemTransaction, L"useItemTransaction",
     L"item use request sent to the server (FastRightClick)",
     "55 41 57 41 56 41 54 56 57 53 48 81 EC 50 01 00 00 48 8D AC 24 80 00 00 00 48 C7 85 "
     "C8 00 00 00 FE FF FF FF 44 89 C3 49 89 D6 48 89 CF"},

    {Target::SetGameMode, L"SetGameMode", L"game mode change (GameModeSwitch)",
     "41 57 41 56 56 57 53 48 83 EC 40 44 89 C3 89 D7 48 89 CE"},

    {Target::SwapSlots, L"swapSlots", L"inventory slot swap (HandRestock)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC C8 00 00 00 48 8D AC 24 80 00 00 00 48 "
     "C7 45 40 FE FF FF FF 4C 8B A1 98 01 00 00 49 63 C0 48 69 F8 98 00 00 00"},

    {Target::ItemStackCopyCtor, L"ItemStack::ItemStack(const&)",
     L"copy-construct an item stack (ItemStackOps)",
     "55 56 48 83 EC 58 48 8D 6C 24 50 48 C7 45 00 FE FF FF FF 48 89 D6 48 8D 05 ? ? ? ? "
     "48 89 01 48 8D 41 08 48 89 45 D8 0F 57 C0 0F 11 41 08"},

    {Target::ItemStackAssign, L"ItemStack::operator=", L"assign an item stack (ItemStackOps)",
     "56 57 48 83 EC 28 48 89 D7 48 89 CE 0F B6 42 22 88 41 22 0F B7 42 20"},

    {Target::ItemStackDtor, L"ItemStack::~ItemStack", L"destroy an item stack (ItemStackOps)",
     "56 57 48 83 EC 28 48 89 CE 48 8D 05 ? ? ? ? 48 89 01 48 8B 49 78 48 85 C9 74 ? "
     "48 8B 01 48 8B 00 BA 01 00 00 00 FF 15 ? ? ? ? 48 8B 4E 50"},

    {Target::ItemStackNetIdAssign, L"ItemStackNetIdVariant::operator=",
     L"assign an item stack net id (ItemStackOps)",
     "56 57 53 48 83 EC 20 48 8B 32 48 0F BE 46 10 4C 8D 0D ? ? ? ? 49 63 0C 89"},

    {Target::MakeSwapAction, L"makeSwapAction", L"build a swap request action (reserved)",
     "41 56 56 57 53 48 83 EC 28 4C 89 C7 48 89 D3 48 89 CE 48 8B 0D ? ? ? ? 48 8B 01 48 "
     "8B 40 08 BA 68 00 00 00 FF 15 ? ? ? ? 48 85 C0"},

    {Target::MakeTakeAction, L"makeTakeAction", L"build a take request action (ItemStackRequest)",
     "41 57 41 56 56 57 53 48 83 EC 20 4C 89 CF 4C 89 C3 49 89 D6 48 89 CE 48 8B 0D ? ? ? "
     "? 48 8B 01 48 8B 40 08 BA 68 00 00 00 FF 15 ? ? ? ? 48 85 C0 0F 84 ? ? ? ? 41 0F B6 "
     "0E C6 40 08 00 48 8D 15 ? ? ? ? 48 89 10"},

    {Target::MakePlaceAction, L"makePlaceAction", L"build a place request action (ItemStackRequest)",
     "41 57 41 56 56 57 53 48 83 EC 20 4C 89 CF 4C 89 C3 49 89 D6 48 89 CE 48 8B 0D ? ? ? "
     "? 48 8B 01 48 8B 40 08 BA 68 00 00 00 FF 15 ? ? ? ? 48 85 C0 0F 84 ? ? ? ? 41 0F B6 "
     "0E C6 40 08 01 48 8D 15 ? ? ? ? 48 89 10"},

    {Target::AddRequestAction, L"addRequestAction", L"queue a request action (HandRestock/BDS)",
     "55 48 83 EC 40 48 8D 6C 24 40 48 C7 45 F8 FE FF FF FF 48 8B 09 48 8B 02 "
     "48 89 55 F0 48 C7 02 00 00 00 00"},

    {Target::BeginRequest, L"_beginRequest", L"open an item stack request (HandRestock/BDS)",
     "55 41 57 41 56 56 57 53 48 83 EC 68 48 8D 6C 24 60 48 C7 45 00 FE FF FF FF "
     "48 89 CE 48 8B 41 38 48 8B 48 20 48 85 C9 0F 84"},

    {Target::EndRequest, L"endAndSendRequest", L"close and queue the request (HandRestock/BDS)",
     "55 41 56 56 57 53 48 83 EC 40 48 8D 6C 24 40 48 C7 45 F8 FE FF FF FF 48 89 CE 48 8D "
     "55 E8 E8 ? ? ? ? 4C 8B 75 E8"},

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
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 98 01 00 00 48 8D AC 24 80 00 00 00 48 "
     "C7 85 10 01 00 00 FE FF FF FF 48 89 4D 48 C6 41 58 01 48 8B 32"},

    {Target::SendCommandRequest, L"sendCommandRequest", L"run a chat command (GameModeSwitch)",
     "55 41 57 41 56 41 54 56 57 53 48 81 EC 90 01 00 00 48 8D AC 24 80 00 00 00 "
     "48 C7 85 08 01 00 00 FE FF FF FF 45 89 CE 4C 89 C7 48 89 D6 48 89 CB 48 8B 49 10 "
     "4C 89 C2 66 41 B8 40 00"},

    {Target::MakeCommandOrigin, L"makeCommandOrigin",
     L"build a command origin (GameModeSwitch)",
     "55 41 56 56 57 53 48 81 EC 60 01 00 00 48 8D AC 24 80 00 00 00 0F 29 B5 D0 00 00 00 "
     "48 C7 85 C8 00 00 00 FE FF FF FF 89 D7 48 89 CE E8 ? ? ? ? 0F 57 F6 0F 29 75 60"},

    {Target::MakeComplexTransaction, L"makeComplexTransaction",
     L"create an inventory transaction (LegacyTransaction)",
     "55 41 56 56 57 53 48 83 EC 50 48 8D 6C 24 50 0F 29 75 F0 48 C7 45 E8 FE FF FF FF 48 "
     "89 CE 83 FA 04 0F 87 ? ? ? ? 4C 89 C7"},

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
     "55 56 48 81 EC 18 03 00 00 48 8D AC 24 80 00 00 00 48 C7 85 90 02 00 00 FE FF FF FF "
     "48 89 95 88 02 00 00 48 89 CE"},

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
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 88 01 00 00 48 8D AC 24 80 00 00 00 48 "
     "C7 85 00 01 00 00 FE FF FF FF 4D 8B 70 10 48 BB FF FF FF FF FF FF FF 7F 49 39 DE"},

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

    {Target::UiSliderPublish, L"uiSliderPublish", L"slider writes its value to the bag",
     "56 48 81 EC 90 00 00 00 0F 29 BC 24 80 00 00 00 0F 29 74 24 70 "
     "48 89 CE 48 8B 05 ?? ?? ?? ?? 48 31 E0 48 89 44 24 68 "
     "F3 0F 11 4C 24 3C 48 8B 49 08"},

    {Target::UiBindingRead, L"uiBindingRead", L"read a binding value out of a UI property bag",
     "55 41 57 41 56 56 57 53 48 81 EC 18 01 00 00 48 8D AC 24 80 00 00 00 "
     "48 C7 85 90 00 00 00 FE FF FF FF 4C 89 CE 48 89 D7 49 89 CE 49 8B 18 4D 8B 78 08"},

    {Target::KeybindListBuild, L"keybindListBuild", L"build the game's key mapping list",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 58 04 00 00 48 8D AC 24 80 00 00 00 44 "
     "0F 29 B5 C0 03 00 00 44 0F 29 AD B0 03 00 00 44 0F 29 A5 A0 03 00 00"},

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
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 48 02 00 00 48 8D AC 24 80 00 00 00 48 "
     "C7 85 C0 01 00 00 FE FF FF FF 44 89 CF 48 89 D3 48 89 CE"},

    {Target::OreKeyboardInputGroup, L"oreKeyboardInputGroup",
     L"produce the keyboard input group facet",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 48 07 00 00 "
     "48 8D AC 24 80 00 00 00 48 C7 85 C0 06 00 00 FE FF FF FF 48 8B 49 08 0F 57 C0 0F 11 02"},

    {Target::KeyActionName, L"keyActionName", L"map a key action index to its id string",
     "41 57 41 56 41 54 56 57 53 48 83 EC 28 48 89 CE 48 63 C2 48 C1 E0 04 48 8D 0D ? ? ? "
     "? 48 8B 7C 08 08 0F 57 C0 0F 11 46 10"},

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

    {Target::RowDataCandA, L"rowDataCandA", L"row data candidate A (0xA0FC90)",
     "55 56 57 53 48 83 EC 58 48 8D 6C 24 50 48 C7 45 00 FE FF FF FF "
     "48 8B 41 08 48 83 B8 98 01 00 00 00 74 ? 48 89 D7"},
    {Target::RowDataCandB, L"rowDataCandB", L"row data candidate B (0xD43EF0)",
     "48 83 EC 28 48 8B 49 08 48 8B 01 48 8B 80 48 05 00 00 FF 15 ? ? ? ? 48 85 C0 74 ?"},

    {Target::OreKeyNameToIndex, L"oreKeyNameToIndex", L"look up a key action index by name",
     "41 57 41 56 41 55 41 54 56 57 53 48 83 EC 50 0F 57 C0 0F 29 44 24 40 "
     "0F 29 44 24 30 48 8B 71 08 48 85 F6 0F 88"},

    {Target::I18nAnchor, L"i18nAnchor", L"anchor to resolve the localization function",
     "48 B8 67 75 69 2E 64 6F 6E 65 48 89 45 D0 48 8D 0D ? ? ? ? "
     "48 8B 05 ? ? ? ? 48 8B 80 80 00 00 00 48 8D 55 F0"},

    {Target::SettingsActionData, L"settingsActionData",
     L"build the action row data (label and state)",
     "55 41 56 56 57 53 48 83 EC 70 48 8D 6C 24 70 48 C7 45 F8 FE FF FF FF 48 89 D6 48 89 CF "
     "48 8B 02 48 8B 40 28 48 89 D1 FF 15"},

    {Target::SettingsActionQueryUpdate, L"settingsActionQueryUpdate",
     L"settingsActionQuery facet value updater (id -> component -> label/state)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC C8 02 00 00 48 8D AC 24 80 00 00 00 "
     "0F 29 B5 30 02 00 00 48 C7 85 28 02 00 00 FE FF FF FF 48 8B B1 30 01 00 00 0F 57"},

    {Target::KeyBindingIsDefault, L"keyBindingIsDefault",
     L"is this key binding still at its default (1 = same as default)",
     "56 57 48 83 EC 28 48 8B 01 4C 8B 08 4C 8B 40 08 4D 29 C8 49 C1 F8 06 B0 01 4C 39 C2"},

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
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 08 01 00 00 48 8D AC 24 80 00 00 00 48 "
     "C7 85 80 00 00 00 FE FF FF FF 48 89 CE 48 8B B9 30 01 00 00 0F 57 C0 0F 29 45 F0 0F "
     "29 45 E0 4C 8B A1 80 01 00 00 48 83 B9 88 01 00 00 10 72 ? 48 8B 9E 70 01 00 00 EB ? "
     "48 8D 9E 70 01 00 00 4D 85 E4 0F 88 ? ? ? ? 49 83 FC 0F 76 ? 4C 89 E2 48 83 CA 0F 48 "
     "83 FA 17 41 BD 16 00 00 00 4C 0F 43 EA 48 8B 0D ? ? ? ? 48 8B 01 48 8B 40 08 48 81 "
     "FA FF 0F 00 00 72 ? 4D 8D 75 28 4C 89 F2 FF 15 ? ? ? ? 49 89 C7 48 85 C0 0F 84 ? ? ? "
     "? 4D 89 FE 49 83 C6 27 49 83 E6 E0 4D 89 7E F8 EB ? 4C 89 65 F0 48 C7 45 F8 0F 00 00 "
     "00 0F 10 03 0F 29 45 E0 4C 8D 75 E0 EB ? 4D 8D 7D 01 4C 89 FA FF 15 ? ? ? ? 49 89 C6 "
     "48 85 C0 0F 84 ? ? ? ? 4C 89 75 E0 4C 89 65 F0 4C 89 6D F8 4D 8D 44 24 01 4C 89 F1 "
     "48 89 DA E8 ? ? ? ? 4C 89 75 B8 4C 89 65 C0 48 8B 07 48 8B 40 18 48 8D 55 A8 4C 8D "
     "45 B8 48 89 F9 FF 15 ? ? ? ? 80 7D B0 01 75 ? 48 8B 7D A8 0F B6 87 C8 03 00 00 83 F8 "
     "06 72 ? 8D 48 F9"},

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

    {Target::SettingsTabList, L"settingsTabList", L"Ore UI: build the settings tab list",
     "55 56 57 53 48 81 EC 38 03 00 00 48 8D AC 24 80 00 00 00 48 C7 85 B0 02 00 00 "
     "FE FF FF FF 48 8B 71 08 0F 57 C0 0F 11 02 48 C7 42 10 00 00 00 00 48 8D 05 "
     "?? ?? ?? ?? 48 89 85 70 01 00 00"},

    {Target::SettingsAddTab, L"settingsAddTab", L"Ore UI: add one settings tab",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 68 02 00 00 48 8D AC 24 80 00 00 00 48 "
     "C7 85 E0 01 00 00 FE FF FF FF 41 89 D6 41 83 C6 FE 48 BF ED FE FF BE 3F 00 00 00"},

    {Target::SettingsInvokeAction, L"settingsInvokeAction", L"Ore UI: perform a settings action",
     "55 56 57 48 83 EC 70 48 8D 6C 24 70 48 C7 45 F8 FE FF FF FF 48 89 55 F0 "
     "48 89 CE 48 8B 01 48 8B 40 08 FF 15"},

    {Target::OpenHowToPlayScreen, L"openHowToPlayScreen",
     L"open the How to Play screen (borrowed to show our own page)",
     "55 56 57 48 83 EC 60 48 8D 6C 24 60 48 C7 45 F8 FE FF FF FF 48 89 CE 48 8B 49 08 48 "
     "8B 01 48 8B 80 78 07 00 00 48 8D 55 E0 FF 15 ? ? ? ? 48 83 7D E0 00 75 ? 48 8D 05 ? "
     "? ? ? 48 89 44 24 20 48 8D 0D ? ? ? ? 48 8D 15 ? ? ? ? 4C 8D 0D ? ? ? ? 41 B8 2C 01 "
     "00 00 E8 ? ? ? ? 84 C0 74 ? C7 04 25 00 00 00 00 DE C0 AD DE 48 8B 45 E0 80 38 00 75 "
     "? 48 8D 05 ? ? ? ? 48 89 44 24 20 48 8D 0D ? ? ? ? 48 8D 15 ? ? ? ? 4C 8D 0D ? ? ? ? "
     "41 B8 30 01 00 00 E8 ? ? ? ? 84 C0 74 ? C7 04 25 00 00 00 00 DE C0 AD DE 48 8B 7D F0 "
     "48 8B 4E 08 48 8B 01 48 8B 80 28 07 00 00 FF 15 ? ? ? ? 48 8D 55 D0 48 89 C1 45 31 "
     "C0 E8 ? ? ? ? 48 8B 07"},
    {Target::MakeOptionElement, L"makeOptionElement", L"Ore UI: build one option element",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 83 EC 58 48 8D 6C 24 50 48 C7 45 00 FE FF FF "
     "FF 4C 89 CE 4C 89 C7 89 11 48 8D 41 08 48 89 45 D8"},

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
     "48 8D 05 ? ? ? ? 48 89 01 48 8B 49 78 48 85 C9 74 ? F0 FF 49 0C 75 ? 48 8B 01 48 8B "
     "40 08 FF 15 ? ? ? ? 48 8B 5E 68 48 85 DB 74 ?"},

    {Target::KeyResetVisibleVtableRef, L"keyResetVisibleVtableRef",
     L"lea of the reset-button visibility callable vtable",
     "48 8D 05 ?? ?? ?? ?? 48 89 85 10 05 00 00 48 8B 8D 58 05 00 00 48 89"},

    {Target::BlockRegistryRef, L"blockRegistryRef", L"lea of the block name table",
     "0F B7 88 C8 00 00 00 F7 D1 66 03 88 CC 00 00 00 89 8D 34 08 00 00 48 8D 8D D0 03 00 "
     "00 E8 ? ? ? ? C6 44 24 28 00 C7 44 24 20 00 00 00 00 48 8D 0D ? ? ? ? 4C 8D 05 ? ? ? "
     "?"},

    {Target::BlockCollisionQuery, L"BlockSource collision AABB gather",
     L"gathers collision boxes for movement",
     "41 57 41 56 41 55 41 54 56 57 55 53 48 81 EC 98 00 00 00 44 0F 29 8C 24 80 00 00 00 "
     "44 0F 29 44 24 70 0F 29 7C 24 60 0F 29 74 24 50 44 89 CF 45 89 C7 48 89 D6 49 89 CE"},

    {Target::SubChunkSetBlock, L"SubChunk::_setBlock", L"write one block into a sub chunk",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 83 EC 68 48 8D 6C 24 60 48 C7 45 00 FE FF FF "
     "FF 4C 89 CF 44 89 C3 41 89 D6 48 89 CE E8 ? ? ? ?"},

    {Target::BlockRenderLookup, L"block render lookup", L"per-block render layer lookup",
     "56 57 53 48 83 EC 20 4C 89 C6 48 89 D7 48 89 CB E8 ?? ?? ?? ?? 48 8B 4B 68 48 85 C0"},

    {Target::BlockTessellate, L"block tessellate", L"draw one block into the chunk mesh",
     "41 57 41 56 56 57 55 53 48 81 EC B8 00 00 00 0F 29 B4 24 A0 00 00 00 4C 89 CF 4D 89 C6"},

    {Target::BlockTessellateCube, L"block tessellate cube",
     L"draw one ordinary cube into the chunk mesh",
     "41 56 56 57 53 48 83 EC 58 0F 29 74 24 40 4C 89 CF 4D 89 C6 48 89 D3"},

    {Target::BlockTessellateShape0, L"block tessellate shape 0",
     L"the handler for shape 0 (an ordinary cube), called with a face mask",
     "41 57 41 56 41 55 41 54 56 57 55 53 48 81 EC A8 00 00 00 4D 89 CE 4C 89 C6 "
     "48 89 D7 48 89 CB"},

    {Target::BlockBuildFaces, L"block build faces", L"build the faces of one block",
     "41 57 41 56 41 55 41 54 56 57 55 53 48 81 EC 88 00 00 00 44 0F 29 44 24 70 "
     "0F 29 7C 24 60 0F 29 74 24 50 4D 89 CE 4C 89 C6 48 89 D3 48 89 CF"},

    {Target::BlockSourceSetBlock, L"BlockSource::setBlock", L"set a block through the region",
     "41 57 41 56 56 57 55 53 48 83 EC 78 44 89 CD 4D 89 C6 48 89 D7 48 89 CE "
     "48 8B 05 ?? ?? ?? ?? 48 31 E0 48 89 44 24 70"},

    {Target::HitResultAssign, L"HitResult::operator=", L"what the crosshair is on",
     "56 57 48 83 EC 28 48 89 C8 48 8B 4A 30 48 89 48 30 0F 10 02"},

    {Target::ModelPartDraw, L"model part draw", L"draw one model part",
     "41 57 41 56 41 54 56 57 55 53 48 81 EC D0 01 00 00 44 0F 29 BC 24 C0 01 00 00 "
     "44 0F 29 B4 24 B0 01 00 00"},

    {Target::BeDispatch, L"block entity dispatch", L"dispatch one block entity renderer",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 58 01 00 00 48 8D AC 24 80 00 00 00 "
     "48 C7 85 D0 00 00 00 FE FF FF FF 4C 89 CB 4D 89 C7 48 89 D6"},

    {Target::AlphaBlendName, L"entity_alphablend name", L"the HashedString for alphablend",
     "BA 11 00 00 00 E8 ?? ?? ?? ?? 48 B8 1E 1A BE EE 17 9B D6 B6 48 89 05 ?? ?? ?? ??"},

    {Target::AlphaTestName, L"entity_alphatest name", L"the HashedString for alphatest",
     "48 B8 15 34 E8 00 B1 54 4F 0A 48 89 05 ?? ?? ?? ??"},

    {Target::SubmitDraw, L"submit draw", L"push one model part into a draw bucket",
     "48 63 C2 48 8B 51 48 4C 8B 51 50 49 29 D2 49 C1 FA 04 49 39 C2 0F 86"},

    {Target::ChunkVisibilityScan, L"chunk visibility scan",
     L"decide whether a sub chunk has anything to draw",
     "41 57 41 56 41 55 41 54 56 57 55 53 48 81 EC 88 00 00 00 48 89 D6 48 89 CB 8B 41 14 "
     "8B 49 1C 83 C1 08 83 C0 08 C1 F8 04"},

    {Target::BeRenderLoop, L"block entity render loop",
     L"iterate the block entities to draw this frame",
     "41 57 41 56 41 55 41 54 56 57 55 53 48 81 EC 98 00 00 00 44 89 C3 48 89 D6 48 89 CF "
     "E8 ? ? ? ? 49 89 C6 E8 ? ? ? ? 49 81 FE 00 36 6E 01"},

    {Target::VisibilityGate, L"visibility gate",
     L"ask the storages whether a sub chunk is all air",
     "55 41 57 41 56 41 54 56 57 53 48 81 EC ?? ?? ?? ?? 48 8D AC 24 ?? ?? ?? ?? "
     "0F 29 B5 ?? ?? ?? ?? 48 C7 85 ?? ?? ?? ?? FE FF FF FF 48 89 D7 48 89 CE "
     "48 8B 49 58 44 8B 86 80 00 00 00"},

    {Target::ChunkBuildLookup, L"chunk build lookup",
     L"map a chunk coordinate to its render chunk record",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC E8 00 00 00 48 8D AC 24 80 00 00 00 "
     "0F 29 7D 50 0F 29 75 40 48 C7 45 38 FE FF FF FF 44 8B 62 08 44 8B 32 44 8B 7A 04"},

    {Target::ScheduleChunkBuild, L"schedule chunk build",
     L"queue one sub chunk for rebuilding",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 08 04 00 00 48 8D AC 24 80 00 00 00 44 "
     "0F 29 85 70 03 00 00 0F 29 BD 60 03 00 00 0F 29 B5 50 03 00 00 48 C7 85 48 03 00 00 "
     "FE FF FF FF 4C 89 CE 4D 89 C6 48 89 D7"},

    {Target::LevelBuildDispatch, L"level build dispatch",
     L"walk the pending chunk queue and schedule builds",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 48 01 00 00 48 8D AC 24 80 00 00 00 44 "
     "0F 29 BD B0 00 00 00 44 0F 29 B5 A0 00 00 00 44 0F 29 AD 90 00 00 00 44 0F 29 A5 80 "
     "00 00 00 44 0F 29 5D 70 44 0F 29 55 60 44 0F 29 4D 50 44 0F 29 45 40 0F 29 7D 30 0F "
     "29 75 20 48 C7 45 18 FE FF FF FF 48 89 D6 48 89 4D B8"},

    {Target::ChunkMeshBuild, L"chunk mesh build",
     L"build one sub chunk mesh (calls BlockTessellate per block)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 68 0B 00 00 48 8D AC 24 80 00 00 00 "
     "44 0F 29 95 D0 0A 00 00 44 0F 29 8D C0 0A 00 00"},

    {Target::TextureLookup, L"texture lookup by name",
     L"resolve a texture handle from a resource path",
     "55 41 56 56 57 53 48 81 EC A0 00 00 00 48 8D AC 24 80 00 00 00 "
     "48 C7 45 18 FE FF FF FF 4C 89 C0 48 89 CE 0F 57 C0 0F 29 45 C0"},

    {Target::PackStackOperation, L"ResourcePackManager::_doStackOperation",
     L"touch one of the resource pack stacks",
     "55 56 48 83 EC 48 48 8D 6C 24 40 48 C7 45 00 FE FF FF FF 83 FA 03 "
     "4C 89 45 F8 0F 87 B5 00 00 00 89 D0 48 8D 15"},

    {Target::BlockTransform, L"block transform (rotate / mirror)",
     L"rotate a block's states the way structures do",
     "55 41 57 41 56 41 54 56 57 53 48 81 EC A0 00 00 00 48 8D AC 24 80 00 00 00 48 C7 45 "
     "18 FE FF FF FF 89 D3 44 89 C7 48 89 CE"},

    {Target::NameTagStage, L"name tag stage (LevelRendererPlayer vfunc 12)",
     L"draw the name tags of the frame",
     "41 57 41 56 41 54 56 57 53 48 83 EC 28 4D 8B B8 E8 32 00 00 4D 8B A0 F0 32 00 00 4D 39 E7"},

    {Target::BlockOutlineDraw, L"block outline draw",
     L"draw the highlight around the block you are looking at",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 18 02 00 00 48 8D AC 24 80 00 00 00 44 "
     "0F 29 95 80 01 00 00 44 0F 29 8D 70 01 00 00 44 0F 29 85 60 01 00 00 0F 29 BD 50 01 "
     "00 00 0F 29 B5 40 01 00 00 48 C7 85 38 01 00 00 FE FF FF FF 4C 89 CB 4D 89 C7 49 89 "
     "D6"},

    {Target::FogSettingsFetch, L"fog settings fetch",
     L"copy the current fog distance settings",
     "48 89 D0 8B 49 08 48 83 F9 05 77 ? 48 8D 15 ? ? ? ? 48 63 0C 8A 48 01 D1 FF E1 "
     "B9 30 00 00 00"},

    {Target::ItemRegistryLookupByName, L"ItemRegistry::lookupByName",
     L"look up an item by its name (stage CQ-9)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 28 01 00 00 48 8D AC 24 80 00 00 00 48 "
     "C7 85 A0 00 00 00 FE FF FF FF 48 89 D6 49 83 79 08 00 0F 84 ? ? ? ? 48 89 4D 28"},

    {Target::NameTagStageCaller, L"name tag stage caller (diagnostic)",
     L"count whether the frame stage host runs (stage CQ-7)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 98 0C 00 00 48 8D AC 24 80 00 00 00 "
     "44 0F 29 A5 00 0C 00 00"},

    {Target::TessellatorBegin, L"Tessellator::begin", L"start a tessellated mesh",
     "56 57 55 53 48 83 EC 38 48 8B 05 ? ? ? ? 48 31 E0 48 89 44 24 30 80 B9 A0 02 00 00 "
     "00 0F 85 ? ? ? ? 80 B9 55 02 00 00 00"},
    {Target::TessellatorVertex, L"Tessellator::vertex", L"add one vertex",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 98 00 00 00 48 8D AC 24 80 00 00 00 44 "
     "0F 29 4D 00 44 0F 29 45 F0 0F 29 7D E0 0F 29 75 D0 48 C7 45 C8 FE FF FF FF 0F 28 F3 "
     "0F 28 FA 44 0F 28 C1"},
    {Target::TessellatorColor, L"Tessellator::color", L"set the vertex color",
     "80 B9 54 02 00 00 00 0F 85 ? ? ? ? F3 0F 10 05 ? ? ? ? F3 0F 59 C8 F3 0F 2C C1 45 31 "
     "C0 85 C0 41 0F 4E C0 3D FF 00 00 00"},

    {Target::RenderMeshImmediately, L"MeshHelpers::renderMeshImmediately",
     L"draw a tessellated mesh right away",
     "55 41 57 41 56 41 54 56 57 53 48 81 EC 70 04 00 00 48 8D AC 24 80 00 00 00 48 C7 85 "
     "E8 03 00 00 FE FF FF FF 80 BA 55 02 00 00 00 0F 85 ? ? ? ? 4C 89 CF"},

    {Target::MaterialPtrCtorSite, L"MaterialPtr construction site",
     L"make a material from its name",
     "48 B8 6C 4E 34 63 95 6C 13 B1 48 89 45 10 48 8D 15 ? ? ? ? 4C 8D 45 10 48 89 F9 E8 ? "
     "? ? ?"},

    {Target::OpenInventoryScreen, L"openInventoryScreen",
     L"open the player inventory on the client (FastInventory)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 88 01 00 00 48 8D AC 24 80 00 00 00 48 "
     "C7 85 00 01 00 00 FE FF FF FF 48 89 CF 4C 8D 35 ? ? ? ? 4C 89 F1 E8 ? ? ? ?"},

    {Target::CameraFovStore, L"camera fov store", L"the other write of the field of view (Zoom)",
     "F3 0F 10 85 08 01 00 00 4A 8D 04 ED 00 00 00 00 4C 01 E8 C1 E0 05 F3 41 0F 11 44 06 50"},

    {Target::HotbarSelectTick, L"hotbar select tick",
     L"the caller that moves the hotbar (Zoom suppresses it while zooming)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC F8 04 00 00 48 8D AC 24 80 00 00 00 48 "
     "C7 85 70 04 00 00 FE FF FF FF 4C 89 C6 48 89 D3 48 8D 7A 38"},

    {Target::MoveVector, L"move vector",
     L"turns the held input into a move vector (FreeCamera takes it for the camera)",
     "56 48 83 EC 20 48 89 D6 48 8B 49 08 E8 ? ? ? ? 48 85 C0 74 ? 8B 08 0F 57 C0 "
     "84 C9 79 ? F3 0F 10 05 ? ? ? ? F6 C1 01 74 ? F3 0F 58 05 ? ? ? ? "
     "B9 00 00 00 80 8B 50 24 31 CA 33 48 28 89 16 F3 0F 11 46 04 89 4E 08"},

    {Target::InputGather, L"input gather",
     L"builds the held-input bits and move amounts (FreeCamera takes them)",
     "56 57 44 0F B7 49 02 45 89 C8 41 81 E0 80 00 00 00 44 89 C8 25 00 01 00 00 "
     "45 89 CA 41 81 E2 00 02 00 00 41 81 E1 00 04 00 00"},

    {Target::MoveIntentFromInput, L"move intent from input",
     L"turns the held keys into a move amount (FreeCamera zeroes it)",
     "48 89 C8 F3 0F 10 52 04 F3 0F 10 5A 08 0F 57 C0 0F 2E D0 0F 85 ? ? ? ? "
     "0F 8A ? ? ? ? 0F 2E D8 0F 85 ? ? ? ? 0F 8A ? ? ? ? 8B 0A 0F 57 D2"},

    {Target::MoveApply, L"move apply",
     L"applies the move vector to the player (FreeCamera restores the position)",
     "41 57 41 56 41 55 41 54 56 57 53 48 81 EC F0 00 00 00 48 89 CE 80 79 50 00 "
     "75 08 48 89 F1 E8 ? ? ? ? 48 8B 4E 08 48 85 C9 0F 84 ? ? ? ? 48 8B 7E 10 "
     "48 85 FF 0F 84 ? ? ? ? 8B 46 48"},

    {Target::BodyPosWrite, L"body position writes",
     L"the three stores of the player position (FreeCamera freezes the body)",
     "F3 0F 59 C6 F3 0F 58 C3 F3 0F 59 CE F3 0F 58 CC F3 0F 59 D6 F3 0F 58 D5 "
     "F3 0F 11 96 94 05 00 00 F3 0F 11 8E 98 05 00 00 F3 0F 11 86 9C 05 00 00"},

    {Target::MoveBox, L"hitbox move", L"moves an entity hitbox (FreeCamera stops the body)",
     "56 57 53 48 81 EC 90 00 00 00 4C 89 CF 4C 89 C3 48 89 D6 49 8B 40 10 "
     "48 89 42 10 41 0F 10 00 0F 11 02"},

    {Target::GetActorEffect, L"Actor::getEffect", L"look up a mob effect on an actor (Fullbright)",
     "48 83 EC 28 4C 8B 41 10 8B 41 18 49 8B 48 48 4D 8B 48 50 49 29 C9 49 C1 E9 03 "
     "41 FF C9 41 81 E1 50 B5 A1 E6 4E 8D 14 C9 49 8B 48 68"},

};

static_assert(std::size(kTargets) == static_cast<size_t>(Target::Count));

}
