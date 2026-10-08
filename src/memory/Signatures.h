#pragma once

#include <string_view>

namespace tsukuyomi {

enum class Target {
    CameraUpdate,

    CameraUpdateContext,
    PlayerView,
    PacketSend,
    BuildBlock,

    MobSwing,
    GameModeContinueDestroyBlock,
    GameModeDestroyBlock,
    GameModeStopDestroyBlock,
    GameModeStartDestroyWrapper,
    GetDestroySpeed,
    SetSelectedSlot,
    AbilitiesAccess,

    PoseDecision,
    UseItem,
    UseItemTransaction,

    SneakingCheck,
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
    HandleItemStackResponse,
    SendCommandRequest,
    CommandRegistryLoadPacket,

    JsonCommandParamSite,
    StringCommandParamSite,
    IntCommandParamSite,
    FloatCommandParamSite,

    CommandParserCtorSite,
    CommandParseSite,
    CommandParserDtorSite,
    CommandParseErrorParamsSite,

    CommandAutoComplete,
    CommandAutoCompleteFilter,
    ClientGetGuiDataSite,
    GuiDataDisplayClientMessage,
    MakeComplexTransaction,
    MakeInventoryAction,
    DestroyInventoryAction,
    AddInventoryAction,
    SendComplexTransaction,
    InventoryHoveredSlot,
    MoveInputHandler,
    PlayerRotation,
    PlayerHeadRotation,
    PlayerHeadRotationInput,
    ViewPerspective,
    UiDefLookup,
    UiEventDispatch,
    UiSliderPublish,
    OreKeyRowsBuild,
    I18nAnchor,
    SettingsGroupRegister,
    SettingsProviderCall,
    SettingsFindComponent,
    GameAllocate,
    PlaySound,
    MakeOptionElement,
    ViewVector,
    PlayerVtableRef,
    NetManagerVtableRef,
    KeyResetVisibleVtableRef,
    BlockRegistryRef,
    SubChunkSetBlock,
    BlockSourceSetBlock,
    BlockCollisionQuery,
    BlockRenderLookup,
    BlockTessellate,
    OpenHowToPlayScreen,
    HitResultAssign,
    HitResultMoveAssign,
    ChunkVisibilityScan,
    BeRenderLoop,
    VisibilityGate,
    ChunkCoordinatorFrame,
    ChunkBuildLookup,
    ScheduleChunkBuild,
    LevelBuildDispatch,
    ChunkMeshBuild,
    BlockTransform,
    NameTagStage,

    NameTagStageCaller,

    BlockOutlineDraw,

    FogSettingsFetch,
    FogDistanceClamp,

    FogColorDistanceClamp,

    ItemRegistryLookupByName,

    ItemRegistryItemListSite,
    TessellatorBegin,
    TessellatorVertex,
    TessellatorColor,
    RenderMeshImmediately,

    TessellatorEnd,
    TessellatorClear,
    MeshRender,
    MeshDestroy,
    MaterialPtrCtorSite,
    GetActorEffect,

    FogColorNightVisionSite,
    CameraFovStore,
    OpenInventoryScreen,
    MoveIntentFromInput,
    InputGather,

    InputGatherKeyCallSite,

    ContainerSmHandle,
    ContainerSmOffsetSite,
    ContainerMcOffsetSite,
    ContainerGetItem,
    ItemStackIsNull,
    ItemStackMaxStackSize,
    ItemStackMatches,
    ItemStackMatchesWrapper,
    ContainerScreenDtor,
    ContainerScreenTick,
    ContainerScreenCtor,
    Trade2Ctor,
    TradeSelectInvoke,
    TradeSecondaryInvoke,
    TradeHoverInvoke,
    TradeSelParse,
    TradeSelTier,
    TradeSelIndex,
    TradeGetOffer,
    TradeTraderIdLoad,
    CompoundTagGet,
    ItemStorageInfo,
    TradeToggleInvoke,

    TradeCurrentTier,

    TradePossible,

    ItemCreativeCategoryStore,

    SceneStackPush,
    OpenTradingScreenPush,

    LegacyParticleRender,

    ContainerCloseGetId,

    ShulkerContentsText,

    ItemHoverTextBuild,

    HoverRendererRender,
    HoverBoxSizeStore,
    UiDrawItem,
    ShulkerHoverAppend,

    ItemStackHoverName,

    ItemMaxDamageSlotSite,
    ItemStackDamageValue,

    ShaderColorFillSite,
    GlintTintSite,

    AttackCore,
    AttackDamageCalc,
    TargetCategorySite,
    AnnouncedSlotSite,
    ArmorStandVtableSite,

    CompoundTagHash,

    ServerPlayerVtableRef,

    HudScreenCtor,

    HudGetItemSite,

    RenderCurrentFrame,
    ServerLevelTick,
    PacketCheckSize,
    NetworkSend,
    StartGameHandle,
    RegionalDifficultyCondition,
    RegionalDifficultyTail,
    LevelChunkTickCall,

    RenderedActorCountStore,

    ClimateSampleCall,
    OverworldGeneratorCtor,

    PreliminarySurfaceLoop,
    PreliminarySurfaceCall,
    DensityGridEntry,
    BlenderFactoryFlags,
    DBChunkStorageVtable,
    DiscardSetInsert,

    GameButtonActionName,
    GameButtonFindKeymap,
    GameButtonBindAction,
    InputMappingFactoryDtorSite,
    InputMappingChordsField,
    InputMappingFactoryPtrSite,
    GameButtonRegisterDownSite,
    GameButtonInputUpdate,
    GameButtonRebuild,
    GameButtonRegisterUpSite,
    GameButtonPadBind,

    PauseMenuOnFocusLostSite,
    SmoothLightingGetter,
    BoolOptionSet,

    GuiDataClearMessages,
    GuiDataFieldSite,
    GamePauseCallSite,
    EmoteReleased,
    EmoteWheelSelected,
    EmoteWheelCtor,
    EmoteWheelBindings,
    ChatCommandRunSite,

    ProfanityFilterGetter,

    ActorAttachPos,

    HealthAttributeLoad,

    ItemIsFood,

    HungerRendererUpdate,
    HeartRendererUpdate,

    FoodAttributeSite,
    ExhaustionAttributeSite,
    DifficultySite,
    GameRulesSite,

    SelectedItemSlotSite,

    ArmorContainerGetter,
    SimpleContainerGetItem,

    FovOptionCtor,

    FovRenderClamp,

    KeyBindingUnassignConflicts,

    KeyBindingUnassignOthers,

    EnchantCommandExecute,
    EnchantLevelRangeCheckSite,
    ApplyEnchantCanEnchantSite,
    EnchantCanEnchantCheckSite,

    StructureSizeClampServer,

    MobEffectScreenListLoop,
    MobEffectInstanceDisplayName,
    MobEffectDurationText,
    MobEffectIconNameSite,
    MobEffectColorStore,

    MobEffectsRendererRender,
    MobEffectsRendererOwnerSite,
    MobEffectsRendererLoop,
    MobEffectsRendererBackground,
    UiControlPositionStore,
    GuiDataGuiScale,
    MobEffectsLayout,
    MobEffectsLayoutRects,

    TickingTextureRender,

    ItemActorRender,
    Count,
};

inline constexpr std::string_view kEmoteIsValidBindingShape =
    "41 57 41 56 56 57 53 48 83 EC 40 4C 89 C6 48 89 D7 48 8B 05 ? ? ? ? 48 31 E0 48 89 44 24 38 4C 8B 79 08 4D 8D 70 08 4C 89 F1 E8 ? ? ? ? BB FF FF FF FF 84 C0 75 ? 4C 89 F1 E8 ? ? ? ? 84 C0 74 ? 48 8D 15 ? ? ? ? 4C 89 F1 E8 ? ? ? ? 49 89 C6 48 89 C1 E8 ? ? ? ? 84 C0 75 ? 4C 89 F1 E8 ? ? ? ? 84 C0 74 ? 4C 89 F1 31 D2 E8 ? ? ? ?";

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

    {Target::CameraUpdateContext, L"CameraUpdateContext", L"camera entity context on the stack (FreeLook)",
     "0F 28 F2 48 89 D6 48 89 CF 48 83 C2 28 48 8D 4C 24 ? E8 ? ? ? ? 80 7C 24 ? 01"},

    {Target::PlayerView, L"PlayerView", L"player position and view angles",
     "41 57 41 56 56 57 53 48 83 EC 70 4C 89 CE 4C 89 C7 49 89 D7 48 89 CB "
     "F3 0F 10 9C 24 C8 00 00 00"},

    {Target::PacketSend, L"PacketSend", L"packet send (blocked while FreeCamera is on)",
     "48 83 EC 28 44 0F B6 41 28 45 85 C0 74 10 B8 48 00 00 00 41 83 F8 01 74 0A "
     "E8 ? ? ? ? B8 20 00 00 00 4C 8B 01 49 8B 04 00 4C 8B 05 ? ? ? ? "
     "48 83 C4 28 49 FF E0"},

    {Target::BuildBlock, L"buildBlock", L"block placement (FastBlockPlacement)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 58 01 00 00 48 8D AC 24 80 00 00 00 48 "
     "C7 85 D0 00 00 00 FE FF FF FF 44 89 CB 44 88 85 CE 00 00 00 49 89 D7 48 89 CE"},

    {Target::MobSwing, L"MobSwing", L"arm swing (no swing on mod-issued placements)",
     "56 57 53 48 81 EC 90 00 00 00 89 D3 48 89 CE 80 B9 34 04 00 00 01 75 ? "
     "8B BE 10 04 00 00 48 89 F1 E8"},

    {Target::GameModeContinueDestroyBlock, L"GameModeContinueDestroyBlock",
     L"continued block breaking (FastBlockBreak)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC ? ? ? ? 48 8D AC 24 ? ? ? ? "
     "0F 29 7D ? 0F 29 75 ? 48 C7 45 ? FE FF FF FF 4D 89 CF 44 88 45 ? 48 89 D7 "
     "48 89 CE 48 8B 9D ? ? ? ? 48 8B 89 ? ? ? ? 48 8B 01 48 8B 40 ? 48 8D 55 ? "
     "FF 15 ? ? ? ? C6 03 00 48 8B 4E ?"},

    {Target::GameModeDestroyBlock, L"GameModeDestroyBlock",
     L"block destruction (FastBlockBreak)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 83 EC ? 48 8D 6C 24 ? 48 C7 45 ? "
     "FE FF FF FF 44 89 C3 48 89 D7 48 89 CE 48 8B 49 ? 48 8B 91 ? ? ? ? 8B 42 08"},

    {Target::GameModeStopDestroyBlock, L"GameModeStopDestroyBlock",
     L"stop block breaking (FastBlockBreak)",
     "56 48 83 EC 20 48 89 CE 48 8B 89 ? ? ? ? F3 0F 10 56 ? 48 8B 01 48 8B 40 ? "
     "FF 15 ? ? ? ? C7 46 ? 00 00 00 00 48 C7 86 ? ? ? ? 00 00 00 00 48 83 C4 20 5E C3"},

    {Target::GameModeStartDestroyWrapper, L"GameModeStartDestroyWrapper",
     L"start block breaking like a press (FastBlockBreak)",
     "41 56 56 57 53 48 83 EC 28 45 89 C8 48 89 D6 48 89 CF 4C 8B 4C 24 ? 48 8B 01 48 8B 40 ? "
     "FF 15 ? ? ? ? 4C 8B 77 ? 84 C0 74 ? 41 C6 86 ? ? ? ? 01"},

    {Target::GetDestroySpeed, L"GetDestroySpeed", L"destroy speed calculation (AutoTool)",
     "55 41 56 56 57 53 48 81 EC C0 00 00 00 48 8D AC 24 80 00 00 00 0F 29 75 30 "
     "48 C7 45 28 FE FF FF FF 48 8B 01 4C 8B 40 08 8B 40 10 4D 8B 50 48 49 8B 50 50"},

    {Target::SetSelectedSlot, L"SetSelectedSlot", L"selected slot control (AutoTool)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 88 02 00 00 48 8D AC 24 80 00 00 00 "
     "48 C7 85 00 02 00 00 FE FF FF FF 89 D6 83 FA 08 0F 87"},

    {Target::AbilitiesAccess, L"AbilitiesAccess", L"ability lookup (CreativeNoClip)",
     "56 57 55 53 48 83 EC 28 48 83 F9 01 0F 84 ? ? ? ? 48 83 F9 02"},

    {Target::PoseDecision, L"PoseDecision", L"pose decision: auto sneak / crawl in low spaces (CreativeNoClip)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 83 EC 48 48 8D 6C 24 40 48 C7 45 00 FE FF FF FF 4C 89 CF "
     "4C 89 C3 48 89 D6 48 8B 8D 90 00 00 00 8B 05 ? ? ? ? 8B 15 ? ? ? ?"},

    {Target::UseItem, L"useItem", L"item use (FastUseItem)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 38 03 00 00 48 8D AC 24 80 00 00 00 48 "
     "C7 85 B0 02 00 00 FE FF FF FF 44 88 85 AF 02 00 00 48 89 D6 48 89 CF"},

    {Target::UseItemTransaction, L"useItemTransaction",
     L"item use request sent to the server (FastUseItem)",
     "55 41 57 41 56 41 54 56 57 53 48 81 EC 50 01 00 00 48 8D AC 24 80 00 00 00 48 C7 85 "
     "C8 00 00 00 FE FF FF FF 44 89 C3 49 89 D6 48 89 CF"},

    {Target::SneakingCheck, L"sneaking check (MoveInputComponent bit 0)",
     L"is the player sneaking (FastUseItem)",
     "48 8B 51 08 8B 41 10 8B 4A 50 4C 8B 42 48 44 29 C1 C1 E9 03 FF C9 81 E1 87 18 8B 01 "
     "4D 8D 0C C8 48 8B 4A 68 66 66 66 2E 0F 1F 84 00 00 00 00 00 4D 8B 01 49 83 F8 FF "
     "0F 84 ? ? ? ? 49 C1 E0 05 4E 8D 0C 01 42 81 7C 01 08 87 18 8B 01 75 E0 4C 01 C1 "
     "48 39 4A 70 74 ? 48 8B 49 10 48 85 C9 74 ? 89 C2 81 E2 FF FF 03 00 41 89 D0 41 C1 "
     "E8 0B 4C 8B 49 08 4C 8B 51 10 4D 29 CA 49 C1 FA 03 4D 39 D0 73 ? 4F 8B 04 C1 4D 85 "
     "C0 74 ? 81 E2 FF 07 00 00 25 00 00 FC FF 41 8B 14 90 31 D0 3D FE FF 03 00 77 ? 48 "
     "8B 41 50 89 D1 C1 E9 04 81 E1 F8 3F 00 00 48 8B 04 08 48 85 C0 74 ? 81 E2 FF FF 03 "
     "00 83 E2 7F 48 6B CA ? 0F B6 04 08 24 01 C3"},

    {Target::SetGameMode, L"SetGameMode", L"game mode change (PlayerContext / GameModeState)",
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

    {Target::MakeSwapAction, L"makeSwapAction", L"build a swap request action",
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

    {Target::HandleItemStackResponse, L"handleItemStackResponse",
     L"read the server's verdict on our request (ItemStackRequest)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 98 01 00 00 48 8D AC 24 80 00 00 00 48 "
     "C7 85 10 01 00 00 FE FF FF FF 48 89 4D 48 C6 41 58 01 48 8B 32"},

    {Target::SendCommandRequest, L"sendCommandRequest", L"run a chat command (ChatCommand hooks it)",
     "55 41 57 41 56 41 54 56 57 53 48 81 EC 90 01 00 00 48 8D AC 24 80 00 00 00 "
     "48 C7 85 08 01 00 00 FE FF FF FF 45 89 CE 4C 89 C7 48 89 D6 48 89 CB 48 8B 49 10 "
     "4C 89 C2 66 41 B8 40 00"},

    {Target::CommandRegistryLoadPacket, L"CommandRegistryLoadPacket", L"load the client command registry",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC B8 02 00 00 48 8D AC 24 80 00 00 00 "
     "0F 29 B5 20 02 00 00 48 C7 85 18 02 00 00 FE FF FF FF 48 89 8D E0 01 00 00 "
     "0F 57 F6 0F 29 B5 C0 00 00 00"},

    {Target::JsonCommandParamSite, L"JsonCommandParamSite", L"the json command parameter type registration (tellraw)",
     "4C 8D 05 ? ? ? ? 4C 8D 0D ? ? ? ? 48 8D BD 90 00 00 00 48 8D 55 D8 48 89 F9 E8 ? ? ? ? 8B 05 ? ? ? ? "
     "8B 0D ? ? ? ? 65 48 8B 14 25 58 00 00 00 48 8B 0C CA 3B 81 04 00 00 00 0F 8F ? ? ? ? 0F B7 05 ? ? ? ? "
     "66 89 45 50 0F 11 74 24 28 C7 44 24 48 FF FF FF FF C6 44 24 40 00 C7 44 24 38 28 00 00 00 "
     "C7 44 24 20 00 00 00 00 4C 8D 05 ? ? ? ?"},

    {Target::StringCommandParamSite, L"StringCommandParamSite", L"the string command parameter type registration (criteria)",
     "4C 8D 05 ? ? ? ? 4C 8D 0D ? ? ? ? 48 8D 9D 00 03 00 00 48 8D 95 80 02 00 00 48 89 D9 E8 ? ? ? ?"},
    {Target::IntCommandParamSite, L"IntCommandParamSite", L"the int command parameter type registration (replaceitem)",
     "4C 8D 05 ? ? ? ? 4C 8D 0D ? ? ? ? 4C 8D B5 10 04 00 00 48 8D 95 10 03 00 00 4C 89 F1 E8 ? ? ? ?"},
    {Target::FloatCommandParamSite, L"FloatCommandParamSite", L"the float command parameter type registration (playsound)",
     "4C 8D 05 ? ? ? ? 4C 8D 0D ? ? ? ? 48 8D BD 70 02 00 00 48 8D 55 D8 48 89 F9 E8 ? ? ? ? "
     "8B 05 ? ? ? ? 8B 0D ? ? ? ? 65 48 8B 14 25 58 00 00 00 48 8B 0C CA 3B 81 04 00 00 00 "
     "0F 8F ? ? ? ? 0F B7 05 ? ? ? ? 66 89 45 50 0F 11 74 24 28 C7 44 24 48 FF FF FF FF "
     "C6 44 24 40 01 C7 44 24 38 1C 01 00 00 C7 44 24 20 00 00 00 00 4C 8D 05 ? ? ? ?"},

    {Target::CommandParserCtorSite, L"CommandParserCtorSite", L"command parser construction in getAutoCompleteOptions",
     "4C 8D 7D B0 4C 89 F9 48 89 FA 41 B8 ? ? ? ? E8 ? ? ? ? 44 89 6C 24 20 C6 44 24 28 01 4C 8D B5 50 01 00 00"},

    {Target::CommandParseSite, L"CommandParseSite", L"parse a command string (stage IE)",
     "48 8D 4D C0 48 8D 95 80 00 00 00 E8 ? ? ? ? 84 C0 74 ? 4C 8B 45 38 4D 85 C0 74 ?"},

    {Target::CommandParserDtorSite, L"CommandParserDtorSite", L"command parser destruction in getAutoCompleteOptions",
     "48 8D 4D B0 E8 ? ? ? ? 48 8B 85 70 01 00 00 48 81 C4 08 02 00 00 5B 5F 5E"},

    {Target::CommandParseErrorParamsSite, L"CommandParseErrorParamsSite", L"build the parse error parameters (stage IE)",
     "48 8D 4D C0 48 8D BD C0 00 00 00 48 89 FA E8 ? ? ? ? 48 8B B5 60 01 00 00 48 39 FE 74 ?"},

    {Target::CommandAutoComplete, L"CommandAutoComplete", L"build chat command completions (stage GB-5)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 08 02 00 00 48 8D AC 24 80 00 00 00 48 C7 85 80 01 00 00 "
     "FE FF FF FF 4C 89 CB 4C 89 C6 48 89 95 70 01 00 00 44 8B AD F0 01 00 00"},

    {Target::CommandAutoCompleteFilter, L"CommandAutoCompleteFilter", L"drop completions the player cannot use (stage GB-5)",
     "55 41 56 56 57 53 48 83 EC 50 48 8D 6C 24 50 48 C7 45 F8 FE FF FF FF 44 89 CE 4C 89 C3 49 89 D0 "
     "48 89 CF 48 8D 55 D8 E8 ? ? ? ? 40 84 F6"},

    {Target::ClientGetGuiDataSite, L"ClientGetGuiDataSite", L"GuiData vtable slot in the chat caller",
     "48 8B 4F 58 48 8B 01 48 8B 80 ? ? ? ? 48 8D 55 E0 FF 15 ? ? ? ? 48 83 7D E0 00 75 ? 48 8D 05 ? ? ? ? 48 89 44 24 20 48 8D 0D ? ? ? ? 48 8D 15 ? ? ? ? 4C 8D 0D ? ? ? ? 41 B8 2C 01 00 00 E8 ? ? ? ? 84 C0 74 ? C7 04 25 00 00 00 00 DE C0 AD DE 48 8B 45 E0 80 38 00 75 ? 48 8D 05 ? ? ? ? 48 89 44 24 20 48 8D 0D ? ? ? ? 48 8D 15 ? ? ? ? 4C 8D 0D ? ? ? ? 41 B8 30 01 00 00 E8 ? ? ? ? 84 C0 74 ? C7 04 25 00 00 00 00 DE C0 AD DE 48 8B 4D F0 48 89 F2 E8 ? ? ? ? 0F 57 C0 48 8B 75 E8 0F 11 45 E0 48 85 F6"},

    {Target::GuiDataDisplayClientMessage, L"GuiDataDisplayClientMessage", L"append a local chat message",
     "55 41 57 41 56 56 57 53 48 81 EC 88 02 00 00 48 8D AC 24 80 00 00 00 48 C7 85 00 02 00 00 FE FF FF FF 4D 89 CE 4C 89 C3 48 89 D7 48 89 CE 0F 57 C0"},

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

    {Target::UiDefLookup, L"uiDefLookup", L"look a UI definition up by namespace and name",
     "41 57 41 56 41 55 41 54 56 57 55 53 48 83 EC 38 4C 89 C6 48 89 D3 48 8B 05 "
     "? ? ? ? 48 31 E0 48 89 44 24 30 4C 8B 72 10"},

    {Target::UiEventDispatch, L"uiEventDispatch", L"dispatch a UI event to the handler list",
     "41 56 56 57 55 53 48 83 EC 20 48 89 D6 48 89 CF "
     "4C 8B B1 A8 09 00 00 48 8B 99 B0 09 00 00 49 39 DE"},

    {Target::UiSliderPublish, L"uiSliderPublish", L"slider writes its value to the bag",
     "56 48 81 EC 90 00 00 00 0F 29 BC 24 80 00 00 00 0F 29 74 24 70 "
     "48 89 CE 48 8B 05 ?? ?? ?? ?? 48 31 E0 48 89 44 24 68 "
     "F3 0F 11 4C 24 3C 48 8B 49 08"},

    {Target::OreKeyRowsBuild, L"oreKeyRowsBuild", L"build the Controls screen key rows",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC C8 00 00 00 48 8D AC 24 80 00 00 00 "
     "48 C7 45 40 FE FF FF FF 4C 89 45 C0 48 89 D7 48 89 CB"},

    {Target::I18nAnchor, L"i18nAnchor", L"anchor to resolve the localization function",
     "48 B8 67 75 69 2E 64 6F 6E 65 48 89 45 D0 48 8D 0D ? ? ? ? "
     "48 8B 05 ? ? ? ? 48 8B 80 80 00 00 00 48 8D 55 F0"},

    {Target::SettingsGroupRegister, L"settingsGroupRegister",
     L"register an Ore UI settings group (id + provider)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 83 EC 68 48 8D 6C 24 60 "
     "48 C7 45 00 FE FF FF FF 0F 57 C0 0F 29 45 F0 0F 29 45 E0 48 8B 5A 08 48 85 DB 0F 88"},

    {Target::SettingsProviderCall, L"settingsProviderCall",
     L"std::function::_Do_call that builds a settings group's item vector",
     "55 41 56 56 57 53 48 81 EC 90 00 00 00 48 8D AC 24 80 00 00 00 "
     "48 C7 45 08 FE FF FF FF 49 89 D1 48 8B 71 08 0F 57 C0"},

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

    {Target::PlayerVtableRef, L"playerVtableRef", L"lea of the Player (LocalPlayer) vtable",
     "48 8D 05 ?? ?? ?? ?? 49 89 06 49 8D 8E B8 0C 00 00"},

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

    {Target::BlockSourceSetBlock, L"BlockSource::setBlock", L"set a block through the region",
     "41 57 41 56 56 57 55 53 48 83 EC 78 44 89 CD 4D 89 C6 48 89 D7 48 89 CE "
     "48 8B 05 ?? ?? ?? ?? 48 31 E0 48 89 44 24 70"},

    {Target::HitResultAssign, L"HitResult::operator=", L"what the crosshair is on",
     "56 57 48 83 EC 28 48 89 C8 48 8B 4A 30 48 89 48 30 0F 10 02"},

    {Target::HitResultMoveAssign, L"HitResult::operator=(&&)", L"what the crosshair is on (entity hits)",
     "56 57 48 83 EC 38 0F 29 74 24 20 48 89 D7 48 89 CE 48 8B 42 30 48 89 41 30 0F 10 02 0F 10 4A 10 "
     "0F 10 52 20"},

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

    {Target::ChunkCoordinatorFrame, L"render chunk coordinator frame",
     L"the coordinator's per-frame work (gives us its address)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 48 01 00 00 48 8D AC 24 80 00 00 00 "
     "0F 29 BD B0 00 00 00 0F 29 B5 A0 00 00 00 48 C7 85 98 00 00 00 FE FF FF FF "
     "48 83 B9 98 00 00 00 00 0F 84 ?? ?? ?? ?? 48 89 CE"},

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

    {Target::FogDistanceClamp, L"fog distance clamp",
     L"clamp of the fog start/end handed to the shaders (NoRender fog)",
     "41 C7 84 24 ? ? 00 00 00 00 80 3F F3 41 0F 10 8C 24 ? ? 00 00 F3 41 0F 5D 8C 24 ? ? 00 00 "
     "F3 41 0F 11 8C 24 ? ? 00 00 F3 41 0F 10 8C 24 ? ? 00 00 F3 41 0F 10 94 24 ? ? 00 00 "
     "0F 28 D9 F3 0F C2 DA 01 F3 0F 5F D0 0F 54 CB 0F 55 DA 0F 56 D9 F3 41 0F 11 9C 24 ? ? 00 00"},

    {Target::ItemRegistryLookupByName, L"ItemRegistry::lookupByName",
     L"look up an item by its name (stage CQ-9)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 28 01 00 00 48 8D AC 24 80 00 00 00 48 "
     "C7 85 A0 00 00 00 FE FF FF FF 48 89 D6 49 83 79 08 00 0F 84 ? ? ? ? 48 89 4D 28"},

    {Target::ItemRegistryItemListSite, L"ItemRegistry item list site",
     L"find the list of every registered item (stage GB-4)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 98 00 00 00 48 8D AC 24 80 00 00 00 "
     "0F 29 75 00 48 C7 45 F8 FE FF FF FF 48 89 CE 48 8B 79 ? 48 8B 59 ? 48 39 DF 75 ? EB ?"},

    {Target::NameTagStageCaller, L"name tag stage caller",
     L"render schematics and apply NoRender at the frame stage host",
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

    {Target::TessellatorEnd, L"Tessellator::end", L"turn the tessellated vertices into a mesh",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 98 03 00 00 48 8D AC 24 80 00 00 00 44 0F 29 "
     "A5 00 03 00 00 44 0F 29 9D F0 02 00 00 44 0F 29 95 E0"},
    {Target::TessellatorClear, L"Tessellator buffers clear", L"empty the tessellator after end",
     "C6 01 00 48 8B 41 08 48 3B 41 10 74 04 48 89 41 10 48 8B 41 20 48 3B 41 28 74 04 48 89 41 "
     "28 48 8B 41 38 48 3B 41 40 74 04 48 89 41 40 48 8B 41 68"},
    {Target::MeshRender, L"mce::Mesh::render", L"draw a kept mesh",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC E8 04 00 00 48 8D AC 24 80 00 00 00 0F 29 B5 "
     "50 04 00 00 48 C7 85 48 04 00 00 FE FF FF FF 4C 89 8D"},
    {Target::MeshDestroy, L"mce::Mesh::~Mesh", L"free a kept mesh",
     "55 56 57 53 48 83 EC 38 48 8D 6C 24 30 48 C7 45 00 FE FF FF FF 48 89 CE E8 ? ? ? ? 48 8B 8E "
     "60 02 00 00 48 85 C9 74 49 48 8B 96 70 02 00 00"},

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

    {Target::InputGather, L"input gather",
     L"builds the held-input bits and move amounts (FreeCamera takes them)",
     "56 57 44 0F B7 49 02 45 89 C8 41 81 E0 80 00 00 00 44 89 C8 25 00 01 00 00 "
     "45 89 CA 41 81 E2 00 02 00 00 41 81 E1 00 04 00 00"},

    {Target::InputGatherKeyCallSite, L"input gather key call site",
     L"the call that passes the raw key bits to InputGather (ToggleSneakSprint edits only these)",
     "41 0F 10 04 24 0F 29 44 24 60 48 8D 54 24 60 4C 89 F9 E8 ? ? ? ? 44 8B 7C 24 60"},

    {Target::MoveIntentFromInput, L"move intent from input",
     L"turns the held keys into a move amount (FreeCamera zeroes it)",
     "48 89 C8 F3 0F 10 52 04 F3 0F 10 5A 08 0F 57 C0 0F 2E D0 0F 85 ? ? ? ? "
     "0F 8A ? ? ? ? 0F 2E D8 0F 85 ? ? ? ? 0F 8A ? ? ? ? 8B 0A 0F 57 D2"},

    {Target::GetActorEffect, L"Actor::getEffect", L"look up a mob effect on an actor (Fullbright)",
     "48 83 EC 28 4C 8B 41 10 8B 41 18 49 8B 48 48 4D 8B 48 50 49 29 C9 49 C1 E9 03 "
     "41 FF C9 41 81 E1 50 B5 A1 E6 4E 8D 14 C9 49 8B 48 68"},

    {Target::FogColorDistanceClamp, L"fog color distance clamp",
     L"second clamp of the fog start/end inside the fog color update (NoRender fog)",
     "C7 86 ? ? 00 00 00 00 80 3F F3 0F 10 8E ? ? 00 00 F3 0F 10 96 ? ? 00 00 F3 0F 5D 96 ? ? 00 00 "
     "F3 0F 11 96 ? ? 00 00 F3 0F 10 96 ? ? 00 00 0F 28 D9 F3 0F C2 DA 01 F3 0F 5F D0 0F 28 C3 0F 55 C2 "
     "0F 54 D9 0F 56 D8 F3 0F 11 9E ? ? 00 00"},

    {Target::FogColorNightVisionSite, L"fog color night vision site",
     L"getEffect(night vision) calls inside the fog color update (Fullbright leaves the sky alone)",
     "48 83 3D ? ? ? ? 00 0F 84 ? ? ? ? BA 10 00 00 00 E8 ? ? ? ? 48 85 C0 0F 84 ? ? ? ? "
     "48 89 D9 BA 10 00 00 00 E8 ? ? ? ? 48 89 C7 48 89 D9 BA 1A 00 00 00 E8"},

    {Target::ContainerSmHandle, L"containerSmHandle", L"container screen state machine (ItemScroller)",
     "41 57 41 56 41 55 41 54 56 57 55 53 48 83 EC 48 4D 89 CF 44 89 C5 89 D7 48 89 CB "
     "48 8B 05 ? ? ? ? 48 31 E0 48 89 44 24 40 C7 81 48 01 00 00 00 00 00 00"},

    {Target::ContainerSmOffsetSite, L"containerSmOffsetSite", L"where the state machine sits in the screen controller",
     "56 48 83 EC 30 48 8B 44 24 60 48 8B 71 08 8B 12 45 8B 00 8B 00 48 8D 8E ? ? ? ? "
     "89 44 24 20 E8"},

    {Target::ContainerMcOffsetSite, L"containerMcOffsetSite", L"where the container manager sits in the screen controller",
     "48 8B 8B ? ? ? ? 48 8B 01 48 8B 40 40 4C 89 FA FF 15"},

    {Target::ContainerGetItem, L"containerGetItem", L"read one slot of a screen collection",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 83 EC 38 48 8D 6C 24 30 48 C7 45 00 FE FF FF FF "
     "44 89 C6 48 89 D7 48 8B 5A 10 48 83 7A 18 10"},

    {Target::ItemStackIsNull, L"itemStackIsNull", L"is the item stack empty",
     "41 57 41 56 56 57 53 48 83 EC 20 40 B6 01 80 79 23 01 0F 85"},

    {Target::ItemStackMaxStackSize, L"itemStackMaxStackSize", L"max stack size of an item stack",
     "55 56 57 53 48 83 EC 38 48 8D 6C 24 30 48 C7 45 00 FE FF FF FF 48 8B 51 08 B0 FF 48 85 D2 74"},

    {Target::ItemStackMatches, L"itemStackMatches", L"compare two item stacks",
     "56 57 53 48 83 EC 20 48 89 CE 48 8B 49 08 48 85 C9 74 ? 48 8B 09 48 85 C9 74 ? "
     "4C 8B 4A 08 4D 85 C9 74 ? 49 39 09 74"},

    {Target::ItemStackMatchesWrapper, L"itemStackMatchesWrapper", L"item stack comparison flags",
     "56 57 48 83 EC 28 48 89 D6 48 89 CF 4C 8D 05 ? ? ? ? E8 ? ? ? ? 84 C0 74 ? 66 81 7F 20 FF 7F"},

    {Target::ContainerScreenDtor, L"containerScreenDtor", L"container screen controller destructor",
     "55 41 56 56 57 53 48 83 EC 30 48 8D 6C 24 30 48 C7 45 F8 FE FF FF FF 48 89 CE 48 8D 05 ? ? ? ? "
     "48 89 01 48 8D 05 ? ? ? ? 48 89 81 C8 0B 00 00 48 8B B9 48 0D 00 00"},

    {Target::ContainerScreenTick, L"containerScreenTick", L"container screen controller tick",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC C8 00 00 00 48 8D AC 24 80 00 00 00 0F 29 75 30 "
     "48 C7 45 28 FE FF FF FF 48 89 CE 48 8D B9 ? ? 00 00 C6 45 F8 00 48 81 C1 ? ? 00 00 E8"},

    {Target::ContainerScreenCtor, L"containerScreenCtor", L"container screen controller constructor",
     "55 41 57 41 56 41 55 41 54 56 57 53 B8 48 2D 00 00 E8 ? ? ? ? 48 29 C4 48 8D AC 24 80 00 00 00 "
     "0F 29 B5 B0 2C 00 00 48 C7 85 A8 2C 00 00 FE FF FF FF 44 89 C7 48 89 CE"},

    {Target::Trade2Ctor, L"trade2Ctor", L"trade screen controller constructor",
     "55 41 57 41 56 41 55 41 54 56 57 53 B8 58 11 00 00 E8 ? ? ? ? 48 29 C4 48 8D AC 24 80 00 00 00 "
     "48 C7 85 D0 10 00 00 FE FF FF FF 48 89 CE 4C 89 85 10 10 00 00"},

    {Target::TradeSelectInvoke, L"tradeSelectInvoke", L"trade_select button handler",
     "55 41 57 41 56 56 57 53 48 81 EC 68 01 00 00 48 8D AC 24 80 00 00 00 0F 29 B5 D0 00 00 00 "
     "48 C7 85 C8 00 00 00 FE FF FF FF 48 8B 71 08 48 8B 3A 0F 57 C0 0F 29 85 90 00 00 00 "
     "48 C7 85 A0 00 00 00 00 00 00 00 0F 29 85 B0 00 00 00 48 C7 85 C0 00 00 00 00 00 00 00 "
     "48 8B 8E ? ? ? ? 48 8D"},

    {Target::TradeSecondaryInvoke, L"tradeSecondaryInvoke", L"trade_secondary_select button handler",
     "55 56 57 53 48 81 EC 68 01 00 00 48 8D AC 24 80 00 00 00 0F 29 B5 D0 00 00 00 "
     "48 C7 85 C8 00 00 00 FE FF FF FF 48 8B 59 08 48 8B 12 48 8D 4D B0 E8 ? ? ? ? 0F 57 C0"},

    {Target::TradeHoverInvoke, L"tradeHoverInvoke", L"trade_toggle_hovered button handler",
     "55 41 57 41 56 41 54 56 57 53 48 81 EC 80 00 00 00 48 8D AC 24 80 00 00 00 "
     "48 C7 45 F8 FE FF FF FF 4C 8B 79 08 41 83 BF ? ? 00 00 03 0F 85 ? ? 00 00 48 8B 12 48 8D 4D A0 E8"},

    {Target::TradeSelParse, L"tradeSelParse", L"read the trade row selection from a property bag",
     "55 56 57 48 83 EC 40 48 8D 6C 24 40 48 C7 45 F8 FE FF FF FF 48 89 D7 48 89 CE 48 83 C7 08 "
     "48 8D 4D E8 31 D2 E8 ? ? ? ? 48 89 F9 E8 ? ? ? ? 84 C0 75 28 48 89 F9 E8 ? ? ? ? 84 C0 74"},

    {Target::TradeSelTier, L"tradeSelTier", L"tier index of a trade row selection",
     "55 56 48 83 EC 58 48 8D 6C 24 50 48 C7 45 00 FE FF FF FF 48 89 CE 0F B6 49 20 84 C9 74 11 "
     "F6 C1 01 0F 84 ? ? 00 00 8B 46 1C E9"},

    {Target::TradeSelIndex, L"tradeSelIndex", L"trade index of a trade row selection",
     "55 56 48 83 EC 58 48 8D 6C 24 50 48 C7 45 00 FE FF FF FF 48 89 CE 0F B6 49 28 84 C9 74 11 "
     "F6 C1 01 0F 84 ? ? 00 00 8B 46 24 E9"},

    {Target::TradeGetOffer, L"tradeGetOffer", L"trade offer by tier and index",
     "55 41 57 41 56 56 57 53 48 83 EC 38 48 8D 6C 24 30 48 C7 45 00 FE FF FF FF 89 D7 0F 57 C0 "
     "0F 29 45 F0 48 8B 91 ? ? 00 00 48 85 D2 0F 84 ? ? 00 00 44 89 C6 8B 42 08"},

    {Target::TradeTraderIdLoad, L"tradeTraderIdLoad", L"load of the trader's ActorUniqueID in the trade model",
     "48 8B 97 ? ? 00 00 48 8B 08 4C 8B 89 ? ? 00 00 48 89 C1 45 31 C0 4C 89 C8 FF 15 ? ? ? ? "
     "48 85 C0 74 31 48 8D 55 B0 48 89 C1 E8"},

    {Target::CompoundTagGet, L"compoundTagGet", L"CompoundTag::get(string_view) any type",
     "41 57 41 56 41 55 41 54 56 57 53 48 83 EC 20 48 89 D6 4C 8B 71 08 4D 8B 7E 08 41 80 7F 19 00 "
     "4C 89 F3 74 0D 80 7B 19 00 74 66 31 C0 E9 B4 00 00 00"},

    {Target::TradeToggleInvoke, L"tradeToggleInvoke", L"trade row toggle-changed handler",
     "55 56 57 48 83 EC 70 48 8D 6C 24 70 48 C7 45 F8 FE FF FF FF 80 7A 08 01 75 ? 48 8B 52 10 48 8B 71 08 "
     "48 8D 7D B8 48 89 F9 E8"},

    {Target::ItemStorageInfo, L"itemStorageInfo", L"storage (bundle) fill of an item stack",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 83 EC 78 48 8D 6C 24 70 48 C7 45 00 FE FF FF FF 48 89 CE "
     "48 8B 42 08 48 85 C0 0F 84 ? ? ? ? 48 8B 08 48 85 C9 0F 84 ? ? ? ? 4C 89 C7 48 89 D3 48 8B 01 48 8B 40 48"},

    {Target::TradeCurrentTier, L"tradeCurrentTier", L"the trader's current tier",
     "56 57 53 48 83 EC 20 48 89 CE 48 8B 49 30 48 8B 91 D0 01 00 00 8B 42 08 0F 1F 84 00 00 00 00 00 "
     "44 8D 40 01 F0 44 0F B1 42 08 75 F4 48 8B B9 C8 01 00 00 48 8B 99 D0 01 00 00 48 85 DB 74 2B F0 "
     "FF 4B 08 75 25 48 8B 03 48 8B 00 48 89 D9 FF 15 ? ? ? ? F0 FF 4B 0C 75 10 48 8B 03 48 8B 40 08 "
     "48 89 D9 FF 15 ? ? ? ? 48 8B 07 48 8B 40 60 48 89 F9 FF 15 ? ? ? ? 48 8B 08 48 8B 91 58 01 00 "
     "00 48 89 C1 48 89 D0 FF 15 ? ? ? ? 48 8B 96 98 01 00 00 48 8B 08 4C 8B 89 F0 01 00 00 48 89 C1 "
     "45 31 C0 4C 89 C8 FF 15 ? ? ? ? 48 85 C0 0F 84 ? ? ? ? 48 8B 80 28 01 00 00"},

    {Target::TradePossible, L"tradePossible", L"whether the player can pay for a trade",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC A8 00 00 00 48 8D AC 24 80 00 00 00 0F 29 75 10 "
     "48 C7 45 08 FE FF FF FF 48 89 D6 48 89 CF 0F 57 C0 0F 29 45 D0 48 8B 89 ? ? 00 00 48 85 C9 74 ? "
     "8B 41 08"},

    {Target::ItemCreativeCategoryStore, L"itemCreativeCategoryStore", L"store of an item's creative category",
     "48 8D 05 ? ? ? ? 48 89 85 80 00 00 00 48 C7 85 88 00 00 00 11 00 00 00 48 89 F9 4C 89 FA E8 ? ? ? ? "
     "48 85 C0 74 06 0F B6 40 08 EB 02 31 C0 41 88 84 24 ? ? ? ?"},

    {Target::SceneStackPush, L"sceneStackPush", L"push a screen onto the scene stack",
     "55 56 48 83 EC 78 48 8D 6C 24 70 48 C7 45 00 FE FF FF FF C6 45 D8 00 0F 57 C0 0F 29 45 E0 "
     "48 8B 42 08 48 85 C0 74 ? F0 FF 40 08 48 8B 42 08 EB ? 31 C0 48 89 55 F8 48 8B 12 48 89 55 E0 "
     "48 89 45 E8 48 8D 55 E0 4C 8D 4D B0 E8"},

    {Target::OpenTradingScreenPush, L"openTradingScreenPush", L"push of the trade screen inside openTrading",
     "48 8B 5D D8 48 8B 8F ? ? 00 00 48 8B 01 48 8B 80 ? ? 00 00 FF 15 ? ? ? ? 48 8D 55 A8 48 89 C1 "
     "49 89 F8 49 89 F1 E8 ? ? ? ? 48 8B 03 48 8B 80 ? ? 00 00 48 8D 55 A8 48 89 D9 45 31 C0 "
     "FF 15 ? ? ? ? E9"},

    {Target::LegacyParticleRender, L"legacyParticleRender", L"submit the legacy particles (NoRender Particle)",
     "55 41 56 56 57 53 48 81 EC 80 00 00 00 48 8D AC 24 80 00 00 00 48 C7 45 F8 FE FF FF FF 4C 89 C6 "
     "49 89 D6 48 89 CF 48 8B 45 50 F3 0F 10 45 58 F3 0F 59 C0 4C 89 45 B0 49 8B 88 ? ? 00 00"},

    {Target::ContainerCloseGetId, L"ContainerClose::getId",
     L"build the inventory-close packet without a copy (HandRestock)", "B8 2F 00 00 00 C3 CC CC"},

    {Target::ShulkerContentsText, L"shulkerContentsText", L"the contents list in a shulker box tooltip (ShulkerPreview)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC ? ? 00 00 48 8D AC 24 80 00 00 00 48 C7 85 ? ? 00 00 FE FF FF FF "
     "0F 57 C0 0F 11 01 48 C7 41 10 00 00 00 00 48 89 8D ? ? 00 00 48 C7 41 18 0F 00 00 00 48 85 D2 0F 84 ? ? ? ? "
     "48 89 D6 48 8D 3D ? ? ? ? 48 89 7D C0 48 C7 45 C8 05 00 00 00"},

    {Target::ItemHoverTextBuild, L"itemHoverTextBuild", L"build the whole item tooltip text (DebugKeys F3+H)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC A8 01 00 00 48 8D AC 24 80 00 00 00 0F 29 B5 10 01 00 00 48 C7 85 08 01 00 00 "
     "FE FF FF FF 44 88 CE 4C 89 45 38 48 89 D0 48 89 4D 60 0F 57 C0 0F 11 02 48 C7 42 10 00 00 00 00 48 C7 42 18 0F 00 00 00 C6 42 40 00"},

    {Target::HoverRendererRender, L"HoverRendererRender", L"draw the item tooltip box (AppleSkin)",
     "55 41 57 41 56 56 57 53 48 81 EC 78 01 00 00 48 8D AC 24 80 00 00 00 44 0F 29 85 E0 00 00 00 "
     "0F 29 BD D0 00 00 00 0F 29 B5 C0 00 00 00 48 C7 85 B8 00 00 00 FE FF FF FF 48 83 79 20 00 "
     "0F 84 ? ? ? ? 48 89 CF 48 83 79 68 00 0F 84 ? ? ? ? 4C 89 C3 F3 44 0F 10 47 08 "
     "F3 0F 10 77 50 F3 0F 10 7F 54 F3 0F 58 7F 5C 48 8B 72 10 F3 0F 58 77 58 48 8B 46 30"},

    {Target::HoverBoxSizeStore, L"HoverBoxSizeStore", L"the tooltip box size fields (ShulkerPreview)",
     "F3 0F 11 7E 60 F3 0F 11 76 64 48 8D 4D C0 E8"},

    {Target::UiDrawItem, L"UiDrawItem", L"draw one item inside a UI renderer (ShulkerPreview)",
     "55 41 57 41 56 41 54 56 57 53 48 81 EC 60 01 00 00 48 8D AC 24 80 00 00 00 44 0F 29 85 D0 00 00 00 "
     "0F 29 BD C0 00 00 00 0F 29 B5 B0 00 00 00 48 C7 85 A8 00 00 00 FE FF FF FF 49 8B 40 08 48 85 C0 "
     "0F 84 ? ? ? ? 48 8B 18 48 85 DB 0F 84 ? ? ? ? 0F 28 F3 4C 89 C7 48 89 CE 44 8B A5 50 01 00 00 "
     "F3 44 0F 10 85 48 01 00 00 F3 0F"},

    {Target::ShulkerHoverAppend, L"ShulkerHoverAppend", L"the shulker box tooltip text builder (ShulkerPreview)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 88 00 00 00 48 8D AC 24 80 00 00 00 48 C7 45 00 FE FF FF FF "
     "4C 89 CE 48 89 D7 0F B6 45 70 88 44 24 20 E8 ? ? ? ? 48 8B 57 10 48 8D 7D B0 48 89 F9 E8 ? ? ? ? "
     "48 8B 5D C0 48 85 DB 0F 84"},

    {Target::ItemStackHoverName, L"ItemStackHoverName", L"the display name of an item stack (ShulkerPreview)",
     "55 41 57 41 56 41 54 56 57 53 48 83 EC 70 48 8D 6C 24 70 48 C7 45 F8 FE FF FF FF 48 89 D6 48 8D 55 B0 E8 ? ? ? ? "
     "0F B6 45 F0 0F 57 C0 0F 11 46 10"},

    {Target::ItemMaxDamageSlotSite, L"ItemMaxDamageSlotSite", L"Item::getMaxDamage vtable slot (ArmorHUD)",
     "48 8B 01 48 8B 80 ? ? ? ? FF 15 ? ? ? ? 98 0F 57 F6 F3 0F 2A F0 F3 0F 11 B5 ? ? ? ? 48 8B 47 10"},

    {Target::ItemStackDamageValue, L"ItemStackDamageValue", L"ItemStackBase::getDamageValue (ArmorHUD)",
     "56 57 48 83 EC 38 48 8B 05 ? ? ? ? 48 31 E0 48 89 44 24 30 48 8B 41 08 48 85 C0 74 ? "
     "48 83 38 00 74 ? 48 8B 71 10 48 85 F6 74 ? 48 8D 3D ? ? ? ? 48 89 7C 24 20 "
     "48 C7 44 24 28 06 00 00 00"},

    {Target::ShaderColorFillSite, L"ShaderColorFillSite", L"the UI shader color written by fillRectangle (ShulkerPreview)",
     "48 8B 4A 30 0F 10 00 0F 11 01 C6 41 10 01"},

    {Target::GlintTintSite, L"GlintTintSite", L"where the glint command reads the UI shader color (ShulkerPreview)",
     "48 8B 4B 20 48 8B 53 30 48 8B 89 50 01 00 00 48 8B 49 40 F3 0F 10 01"},

    {Target::AttackCore, L"AttackCore", L"GameMode::attack body (AutoTool)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC F8 01 00 00 48 8D AC 24 80 00 00 00 48 C7 85 70 01 00 00 "
     "FE FF FF FF 4C 89 CB 45 89 C6 49 89 D7 48 89 CF 48 8B 41 08"},

    {Target::AttackDamageCalc, L"AttackDamageCalc", L"melee damage of the selected item (AutoTool)",
     "41 56 56 57 53 48 83 EC 68 44 0F 29 44 24 50 0F 29 7C 24 40 0F 29 74 24 30 4C 89 C7 48 89 D3 48 89 CE "
     "48 8B 05 ? ? ? ? 48 31 E0 48 89 44 24 28 F3 41 0F 10 38 41 80 78 04 01"},

    {Target::TargetCategorySite, L"TargetCategorySite", L"actor category bits field (AutoTool)",
     "80 7F 06 01 75 ? F6 83 ? ? ? ? 02 74 ? 48 89 D9 48 89 F2 E8"},

    {Target::AnnouncedSlotSite, L"AnnouncedSlotSite", L"the hotbar slot last announced to the server (AutoTool)",
     "48 8B 86 ? ? ? ? 44 0F B6 A0 B0 00 00 00 8B 58 10 4C 8D B6 ? ? ? ? 0F B6 86 ? ? ? ? 41 3A 47 22 75 ? "
     "4C 89 F1 4C 89 FA E8 ? ? ? ? 84 C0 74 ? 39 9E ? ? ? ?"},

    {Target::ArmorStandVtableSite, L"ArmorStandVtableSite", L"the armor stand vtable (AutoTool)",
     "81 89 10 02 00 00 02 00 08 00 48 C7 81 ? ? 00 00 FF FF FF FF 48 8D 05 ? ? ? ? 48 89 01 48 C7 81 ? ? 00 00 "
     "00 00 00 00 C7 81"},

    {Target::CompoundTagHash, L"CompoundTagHash", L"CompoundTag::hash (ShulkerPreview)",
     "41 57 41 56 41 55 41 54 56 57 55 53 48 83 EC 28 48 8B 79 08 4C 8B 27 49 39 FC 0F 84 ? ? ? ? "
     "48 BB 25 23 22 84 E4 9C F2 CB 49 BE B3 01 00 00 00 01 00 00 41 BF B9 79 37 9E 31 F6 E9"},

    {Target::ServerPlayerVtableRef, L"serverPlayerVtableRef", L"lea of the ServerPlayer vtable (GameData)",
     "48 8D 0D ?? ?? ?? ?? 49 89 0C 24 41 89 84 24 B8 0C 00 00"},

    {Target::HudScreenCtor, L"hudScreenCtor", L"HUD screen controller constructor (InventoryHUD)",
     "55 41 57 41 56 41 55 41 54 56 57 53 B8 B8 13 00 00 E8 ? ? ? ? 48 29 C4 48 8D AC 24 80 00 00 00 "
     "0F 29 B5 20 13 00 00 48 C7 85 18 13 00 00 FE FF FF FF 48 89 CE 0F 57 C0 0F 29 85 80 09 00 00 "
     "48 8B 42 08 48 85 C0 74 0A F0 FF 40 08"},

    {Target::HudGetItemSite, L"hudGetItemSite", L"where the HUD binding resolver reads its container manager (InventoryHUD)",
     "49 8B 8E ? ? ? ? 4C 89 FA 41 89 D8 E8 ? ? ? ? BB FF FF FF FF 80 78 23 01"},

    {Target::RenderCurrentFrame, L"renderCurrentFrame", L"start of one rendered frame (DebugScreen fps)",
     "55 41 57 41 56 41 55 41 54 56 57 53 B8 ? 36 00 00 E8 ? ? ? ? 48 29 C4 48 8D AC 24 80 00 00 00 "
     "44 0F 29 BD ? 35 00 00 44 0F 29 B5 ? 35 00 00 44 0F 29 AD ? 35 00 00 44 0F 29 A5 ? 35 00 00"},

    {Target::ServerLevelTick, L"serverLevelTick", L"integrated server level tick (DebugScreen tps)",
     "56 48 83 EC 30 48 8B 41 30 48 85 C0 0F 84 F1 00 00 00 80 38 01 0F 85 E8 00 00 00 48 89 CE 48 8B "
     "49 40 48 8B 01 48 8B 80 ? ? 00 00 FF 15 ? ? ? ? 84 C0 0F 85 C9 00 00 00 48 83 7E 30 00 75 3B"},

    {Target::PacketCheckSize, L"packetCheckSize", L"incoming packet size check (DebugScreen rx)",
     "56 57 55 53 48 81 EC 88 00 00 00 44 89 CD 4C 89 C3 48 89 D6 48 89 CF 4C 89 44 24 28 48 8B 01 48 "
     "8B 40 18 FF 15"},

    {Target::RenderedActorCountStore, L"renderedActorCountStore", L"store of the rendered entity count (DebugScreen E)",
     "44 01 E7 89 3D ?? ?? ?? ?? 48 8B 85 00 02 00 00 48 8B 40 28 48 8B 70 18 48 8D 4E 40 C6 46 78 01"},

    {Target::ClimateSampleCall, L"climateSampleCall", L"call of the climate sampler in BiomeSource3d::getBiome (DebugScreen)",
     "48 8B 0E 8B 47 08 89 45 18 48 8B 07 48 89 45 10 48 8D 55 A8 4C 8D 45 10 E8 ? ? ? ? 80 BE C0 00 00 00 01"},

    {Target::OverworldGeneratorCtor, L"overworldGeneratorCtor", L"OverworldGenerator constructor (vtable and BiomeSource field)",
     "48 8D 05 ? ? ? ? 48 89 07 48 8D 05 ? ? ? ? 48 89 47 70 48 8D 87 78 02 00 00 48 89 85 ? ? ? ? C6 87 ? ? ? ? 00 "
     "48 8D 87 ? ? ? ? 48 89 85"},

    {Target::PreliminarySurfaceLoop, L"preliminarySurfaceLoop", L"search loop of the preliminary surface level (DebugScreen PS)",
     "BA 27 00 00 00 F3 0F 10 15 ?? ?? ?? ?? F3 44 0F 10 25 ?? ?? ?? ?? 0F 57 E4 4C 8D 05 ?? ?? ?? ?? F3 0F 10 2D ?? ?? ?? ?? "
     "F3 0F 10 35 ?? ?? ?? ?? F3 0F 10 3D ?? ?? ?? ?? F3 44 0F 10 05 ?? ?? ?? ?? F3 44 0F 10 0D ?? ?? ?? ?? "
     "F3 44 0F 10 15 ?? ?? ?? ?? F3 44 0F 10 1D ?? ?? ?? ?? EB"},

    {Target::PreliminarySurfaceCall, L"preliminarySurfaceCall", L"column offset/factor call in the preliminary surface level (DebugScreen PS)",
     "48 8D 8B ?? ?? 00 00 48 8D 55 D4 49 89 E8 E8 ?? ?? ?? ?? 4C 8B 8B ?? ?? 00 00 48 8B 83 ?? ?? 00 00 48 89 44 24 20 "
     "48 8D 4D E0 4C 8D 45 D4 4C 89 F2 E8"},

    {Target::DensityGridEntry, L"densityGridEntry", L"density grid of one chunk in OverworldGenerator (DebugScreen N)",
     "55 41 57 41 56 56 57 53 48 83 EC 78 48 8D 6C 24 70 48 C7 45 00 FE FF FF FF 4C 89 C7 48 89 D6 48 89 CB 49 8B 10 "
     "4C 8B 89 ?? ?? 00 00 48 8B 81 ?? ?? 00 00 4C 8D 81 ?? ?? 00 00 48 89 44 24 20 48 8D 4D C8 E8"},

    {Target::BlenderFactoryFlags, L"blenderFactoryFlags", L"ChunkBlenderFactory construction (DebugScreen N safety flag)",
     "48 C7 40 40 00 00 00 00 40 88 78 48 48 C7 40 50 00 00 00 00 66 44 89 70 58 40 84 FF"},

    {Target::DBChunkStorageVtable, L"dbChunkStorageVtable", L"DBChunkStorage constructor (DebugScreen Chunks[S])",
     "48 8D 05 ?? ?? ?? ?? 48 8B 4D 10 48 89 01 C7 41 70 00 00 00 00 0F 57 C0 0F 11 41 78"},

    {Target::DiscardSetInsert, L"discardSetInsert", L"insert into the discarding chunk set (DebugScreen Chunks[S] unload)",
     "48 B8 FF FF FF FF FF FF FF 07 48 39 86 ?? ?? 00 00 0F 84 ?? ?? ?? ?? 48 8D 86 ?? ?? 00 00 48 89 45 E0"},
    {Target::NetworkSend, L"networkSend", L"outgoing packet send (DebugScreen tx)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 08 01 00 00 48 8D AC 24 80 00 00 00 48 C7 85 80 00 "
     "00 00 FE FF FF FF 45 89 CD 4C 89 C6 48 89 D7 48 89 CB"},

    {Target::StartGameHandle, L"StartGameHandle", L"StartGame packet handler (DebugScreen server version)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC ? ? 00 00 48 8D AC 24 80 00 00 00 0F 29 B5 ? ? 00 00 "
     "48 C7 85 ? ? 00 00 FE FF FF FF 48 89 95 ? ? 00 00 48 89 CE 48 83 79 48 00 4C 89 85"},

    {Target::RegionalDifficultyCondition, L"RegionalDifficultyCondition", L"Level regional difficulty vtable slot (DebugScreen)",
     "56 57 48 83 EC 48 0F 29 7C 24 30 0F 29 74 24 20 4C 89 C6 48 89 CF 48 8B 02 48 8B 40 28 48 89 D1 "
     "FF 15 ? ? ? ? 0F 28 F0 F3 0F 10 7F 10 48 8B 4E 08 8B 96 ? ? 00 00 48 8B 01 48 8B 80 ? ? 00 00"},

    {Target::RegionalDifficultyTail, L"RegionalDifficultyTail", L"Level regional difficulty return tail (DebugScreen)",
     "F3 0F 58 D3 0F 57 DB F3 0F 2A DE F3 0F 59 DA F3 0F 10 15 ? ? ? ? 0F 2E D3 77 24 "
     "0F 2E 1D ? ? ? ? 0F 28 C1 77 18 F3 0F 58 1D ? ? ? ? F3 0F 59 1D ? ? ? ? 0F 28 C3"},

    {Target::LevelChunkTickCall, L"LevelChunkTickCall", L"call of LevelChunk tick (DebugScreen)",
     "48 8B 0F 48 89 4D 20 4C 89 75 28 4C 89 65 E0 48 89 4D E8 48 89 75 F0 48 89 5D 18 48 89 F2 "
     "4C 8B 45 D8 49 89 D9 E8 ? ? ? ? 4D 85 F6"},

    {Target::GameButtonActionName, L"gameButtonActionName", L"input action id -> name (hotkeys as game buttons)",
     "41 57 41 56 41 54 56 57 53 48 83 EC 28 48 89 CE 48 63 C2 48 C1 E0 04 48 8D 0D"},

    {Target::GameButtonFindKeymap, L"gameButtonFindKeymap", L"keymapping lookup by action name (hotkeys as game buttons)",
     "55 41 56 56 57 53 48 83 EC 30 48 8D 6C 24 30 48 C7 45 F8 FE FF FF FF 48 8B 71 08 4C 8B 71 10 4C 39 F6 74 ? "
     "48 89 D7 48 8B 5A 10 48 83 7A 18 10"},

    {Target::GameButtonBindAction, L"gameButtonBindAction", L"bind one input action to a button (hotkeys as game buttons)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 78 01 00 00 48 8D AC 24 80 00 00 00 0F 29 BD E0 00 00 00 "
     "0F 29 B5 D0 00 00 00 48 C7 85 C8 00 00 00 FE FF FF FF 4C 89 4D 70 4C 89 45 30 48 89 95 C0 00 00 00"},

    {Target::InputMappingFactoryDtorSite, L"inputMappingFactoryDtorSite", L"the two mapping tables of the input mapping factory (hotkey chords)",
     "48 8D 4E ? 48 8B 56 ? E8 ? ? ? ? 48 8B 4E ? BA 00 03 00 00 E8 ? ? ? ? 48 8B 4E ? 48 85 C9 74 ? 48 8B 56 ? 48 29 CA "
     "48 81 FA 00 10 00 00 72 ? 48 8B 41 F8 48 83 C1 F8 48 29 C1 48 83 F9 20 73 ? 48 83 C2 27 48 89 C1 48 8D 5E ? E8 ? ? ? ? "
     "0F 57 C0 0F 11 03 48 C7 43 10 00 00 00 00 48 8D 4E ? 48 8B 56 ? E8 ? ? ? ? 48 8B 4E ? BA 00 03 00 00"},

    {Target::InputMappingChordsField, L"inputMappingChordsField", L"chord vector field of InputMapping (hotkey chords)",
     "48 8B 45 E0 48 8D 88 ? ? ? ? 0F 11 B0 ? ? ? ? 48 C7 80 ? ? ? ? 00 00 00 00 4D 8D 48 08 49 8B 50 08 49 2B 10 48 C1 FA 06"},

    {Target::InputMappingFactoryPtrSite, L"inputMappingFactoryPtrSite", L"owner -> input mapping factory field (hotkey chords)",
     "48 8B 08 48 39 C1 75 ? 49 8B 5D ? 48 8B 43 ? 48 85 C0 0F 84 ? ? ? ? 4C 8B 73 ? 48 8B 4B ? 48 C1 E9 03 48 39 C1"},

    {Target::GameButtonRegisterDownSite, L"gameButtonRegisterDownSite", L"call of InputHandler button-down registration (hotkeys as game buttons)",
     "48 8D 95 50 19 00 00 4C 8D 85 30 09 00 00 48 89 F9 45 31 C9 E8 ? ? ? ?"},

    {Target::GameButtonInputUpdate, L"gameButtonInputUpdate", L"client input handler update (hotkeys as game buttons)",
     "55 41 57 41 56 56 57 53 48 83 EC 38 48 8D 6C 24 30 48 C7 45 00 FE FF FF FF 4C 89 CB 4D 89 C6 48 89 D6 "
     "48 89 CF 48 8B 0D ? ? ? ? 48 8B 01 48 8B 40 10 B2 13 FF 15"},

    {Target::GameButtonRebuild, L"gameButtonRebuild", L"rebuild the input mappings (hotkeys as game buttons)",
     "55 41 56 56 57 53 48 81 EC 80 00 00 00 48 8D AC 24 80 00 00 00 48 C7 45 F8 FE FF FF FF 48 89 CE "
     "48 8B 49 08 48 8B 01 48 8B 80 78 09 00 00 FF 15"},

    {Target::GameButtonRegisterUpSite, L"gameButtonRegisterUpSite", L"call of InputHandler button-up registration (game button states)",
     "C7 85 B8 16 00 00 6D 6F 74 65 48 8D 95 B0 16 00 00 45 31 C9 E8 ? ? ? ?"},

    {Target::GameButtonPadBind, L"gameButtonPadBind", L"controller input mapping row binding (hotkeys)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 88 00 00 00 48 8D AC 24 80 00 00 00 0F 29 75 F0 48 C7 45 E8 FE FF FF FF 4C 89 C6 48 89 D7 48 89 CB C7 45 D8 00 00 80 BF C7 45 E0 00 00 80 BF"},

    {Target::PauseMenuOnFocusLostSite, L"pauseMenuOnFocusLostSite", L"read of options.pauseMenuOnFocusLost (DebugKeys F3+P)",
     "48 8B 4D 18 48 8B 01 48 8B 80 78 05 00 00 FF 15 ? ? ? ? 48 8B 08 4C 8B 89 D8 00 00 00 48 8D 55 F0 48 89 C1 41 B8 2E 03 00 00"},

    {Target::SmoothLightingGetter, L"smoothLightingGetter", L"options.smooth_lighting getter (DebugKeys F3+A)",
     "48 83 EC 38 48 8B 05 ? ? ? ? 48 31 E0 48 89 44 24 30 48 8B 01 48 8B 40 08 48 8D 54 24 28 41 B8 29 00 00 00 FF 15 ? ? ? ? 48 8B 4C 24 28"},

    {Target::BoolOptionSet, L"boolOptionSet", L"BoolOption::set (DebugKeys F3+P)",
     "56 57 53 48 83 EC 30 48 8B 05 ? ? ? ? 48 31 E0 48 89 44 24 28 48 8B 41 08 48 83 B8 38 02 00 00 00 74 1F"},

    {Target::GuiDataClearMessages, L"guiDataClearMessages", L"clear the vanilla chat (DebugKeys F3+D)",
     "56 57 53 48 83 EC 20 48 89 CE 48 8B B9 50 01 00 00 48 8B 99 58 01 00 00 48 39 DF 74 ? 0F 1F 00 48 89 F9 E8 ? ? ? ? 48 81 C7 10 01 00 00"},

    {Target::GuiDataFieldSite, L"guiDataFieldSite", L"ClientInstance GuiData field (DebugKeys F3+D)",
     "48 8B B9 ? ? ? ? 48 85 FF 0F 84 ? ? ? ? 0F 57 C0 0F 11 02 48 C7 42 10 00 00 00 00 48 8B 47 38 48 85 C0"},

    {Target::GamePauseCallSite, L"gamePauseCallSite", L"pause model calls (DebugKeys F3+Esc)",
     "48 8B 89 ? ? 00 00 B2 01 E8 ? ? ? ? 48 8B 8E ? ? 00 00 B2 01 E8 ? ? ? ? 48 8B 8E ? ? 00 00 E8"},

    {Target::EmoteReleased, L"emoteReleased", L"vanilla emote button release (DebugKeys F3+F4)",
     "55 41 56 56 57 53 48 83 EC 60 48 8D 6C 24 60 48 C7 45 F8 FE FF FF FF 48 89 CF E8 ? ? ? ? 84 C0 0F 85 ? ? ? ? 48 8B 07 48 8B 80 F8 00 00 00 48 89 F9 FF 15 ? ? ? ? 48 85 C0 0F 84 ? ? ? ? 48 89 C6 48 8B 07 48 8B 80 E8 04 00 00"},

    {Target::EmoteWheelSelected, L"emoteWheelSelected", L"emote wheel selection (DebugKeys F3+F4)",
     "41 57 41 56 56 57 53 48 83 EC 50 89 D7 48 89 CE 48 8B 99 48 0D 00 00 48 83 7B 48 00 75"},

    {Target::EmoteWheelCtor, L"emoteWheelCtor", L"identify own emote wheel (DebugKeys F3+F4)",
     "55 56 57 48 81 EC D0 03 00 00 48 8D AC 24 80 00 00 00 48 C7 85 48 03 00 00 FE FF FF FF 4C 89 85 40 03 00 00 48 89 CE 0F 57 C0 0F 29 85 F0 02 00 00 48 8B 42 08 48 85 C0 74 0A F0 FF 40 08 48 8B 42 08 EB 02 31 C0 48 89 95 38 03 00 00 48 8B 0A 48 89 8D F0 02 00 00 48 89 85 F8 02 00 00 48 8D 95 F0 02 00 00 48 89 F1 E8 ? ? ? ? 48 8D 05 ? ? ? ?"},

    {Target::EmoteWheelBindings, L"emoteWheelBindings", L"emote binding vtables (DebugKeys F3+F4)",
     "48 8D 05 ? ? ? ? 48 89 85 20 02 00 00 48 89 8D 28 02 00 00 48 8D 9A 00 0B 00 00 C7 85 F4 03 00 00 8C D4 3E 3C"},

    {Target::ChatCommandRunSite, L"chatCommandRunSite", L"run a chat command like the chat screen (DebugKeys F3+F4 / F3+N)",
     "48 8B 48 48 48 89 4D F0 48 89 5D F8 48 8B 40 58 48 89 45 00 48 85 C9 0F 84 ? ? ? ? 80 39 00 0F 84 ? ? ? ? 48 8D 55 F0 48 89 F9 E8 ? ? ? ?"},

    {Target::ProfanityFilterGetter, L"profanityFilterGetter", L"options.filter_profanity getter (DebugScreen F3+D)",
     "48 83 EC 38 48 8B 05 ? ? ? ? 48 31 E0 48 89 44 24 30 48 8B 01 48 8B 40 08 48 8D 54 24 28 41 B8 34 02 00 00"},

    {Target::ActorAttachPos, L"actorAttachPos", L"Actor::getAttachPos (DebugScreen F3+B)",
     "41 57 41 56 41 55 41 54 56 57 55 53 48 83 EC 78 0F 29 74 24 60 44 89 C7 48 89 D6 8D 47 FF 83 F8 01 77 ? 48 8B 81 20 02 00 00 F3 0F 10 40 14"},

    {Target::HealthAttributeLoad, L"healthAttributeLoad", L"health attribute global (PlayerList hearts)",
     "80 FA 03 75 ? 4C 8D 05 ? ? ? ? 48 8D 54 24 20 E8 ? ? ? ? 48 8B 44 24 20 48 85 C0 74 ?"},

    {Target::ItemIsFood, L"ItemIsFood", L"Item::isFood (AppleSkin getFood slot)",
     "48 83 EC 28 48 8B 01 48 8B 80 C8 00 00 00 FF 15 ? ? ? ? 48 85 C0 0F 95 C0 48 83 C4 28 C3"},

    {Target::HungerRendererUpdate, L"HungerRendererUpdate", L"HUD hunger bar update (AppleSkin)",
     "55 41 57 41 56 56 57 53 48 81 EC 28 05 00 00 48 8D AC 24 80 00 00 00 0F 29 B5 90 04 00 00 48 C7 85 88 04 00 00 "
     "FE FF FF FF 48 89 CE FF 41 10 48 89 D7 80 79 14 00"},

    {Target::HeartRendererUpdate, L"HeartRendererUpdate", L"HUD hearts update (AppleSkin)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC B8 0A 00 00 48 8D AC 24 80 00 00 00 0F 29 B5 20 0A 00 00 48 C7 85 "
     "18 0A 00 00 FE FF FF FF 48 89 D3 48 89 CE 48 8D 4D B0"},

    {Target::FoodAttributeSite, L"FoodAttributeSite", L"hunger / saturation attributes and Actor::getEffect (AppleSkin)",
     "48 89 D9 BA 11 00 00 00 E8 ? ? ? ? 48 85 C0 0F 95 46 16 4C 8D 05 ? ? ? ? 48 8D 55 B0 48 89 D9"},

    {Target::ExhaustionAttributeSite, L"ExhaustionAttributeSite", L"exhaustion attribute (AppleSkin)",
     "48 89 F1 E8 ? ? ? ? 84 C0 74 ? 0F 28 75 40 48 81 C4 D8 00 00 00 5E 5D C3 4C 8D 05 ? ? ? ?"},

    {Target::DifficultySite, L"DifficultySite", L"Actor level and Level::getDifficulty slot (AppleSkin)",
     "48 8B 8B D8 01 00 00 48 8B 01 48 8B 80 20 01 00 00 FF 15 ? ? ? ? 85 C0 74 ? 4C 8D 05 ? ? ? ? 48 8D 54 24 20 48 89 D9 E8 ? ? ? ? "
     "48 8B 44 24 20 F3 0F 10 40 78 0F 2E 40 7C 77 ? 41 80 7E 7C 01 75 ? 48 8B 47 08"},

    {Target::GameRulesSite, L"GameRulesSite", L"Level::getGameRules slot (AppleSkin natural regeneration)",
     "48 8B 46 20 48 8B 88 D8 01 00 00 48 8B 01 48 8B 80 B0 0A 00 00 FF 15 ? ? ? ? 48 8B 48 18 45 31 ED"},

    {Target::SelectedItemSlotSite, L"SelectedItemSlotSite", L"selected item vtable slot (HeldItem (FastBlockPlacement))",
     "48 8B 4E 08 80 FB 01 74 ? 45 85 E4 48 8B 01 48 8B 80 ? ? ? ? FF 15"},

    {Target::ArmorContainerGetter, L"ArmorContainerGetter", L"armor container field of ActorEquipmentComponent (ArmorHUD)",
     "48 83 EC 28 48 8B 51 08 8B 41 10 48 8B 4A 48 4C 8B 42 50 49 29 C8 49 C1 E8 03 41 FF C8 41 81 E0 A9 41 61 B0 "
     "4E 8D 0C C1 48 8B 4A 68 0F 1F 40 00 4D 8B 01 49 83 F8 FF 0F 84 ? ? ? ? 49 C1 E0 05 4E 8D 0C 01 42 81 7C 01 08 "
     "A9 41 61 B0 75 E0 4C 01 C1 48 39 4A 70 74 ? 48 8B 49 10 48 85 C9 74 ? 89 C2 81 E2 FF FF 03 00 41 89 D0 41 C1 E8 0B "
     "4C 8B 49 08 4C 8B 51 10 4D 29 CA 49 C1 FA 03 4D 39 D0 73 ? 4F 8B 04 C1 4D 85 C0 74 ? 81 E2 FF 07 00 00 25 00 00 FC FF "
     "41 8B 14 90 31 D0 3D FE FF 03 00 77 ? 48 8B 41 50 89 D1 C1 E9 04 81 E1 F8 3F 00 00 48 8B 04 08 48 85 C0 74 ? "
     "81 E2 FF FF 03 00 83 E2 7F C1 E2 ? 48 8B 44 10 ? 48 83 C4 28 C3"},

    {Target::SimpleContainerGetItem, L"SimpleContainerGetItem", L"SimpleContainer::getItem item vector (ArmorHUD)",
     "85 D2 75 ? 80 79 08 ? 75 ? 48 8B 89 ? ? ? ? 48 8B 01 48 8B 80 ? ? ? ? 48 8B 15 ? ? ? ? 48 FF E2 48 8D 05 ? ? ? ? "
     "85 D2 78 ? 4C 8B 81 ? ? ? ? 48 8B 89 ? ? ? ? 4C 29 C1 48 C1 E9 03 69 C9 ? ? ? ? 39 CA 7D ? 89 D0 48 69 C0 ? ? ? ? "
     "49 01 C0 4C 89 C0 C3"},

    {Target::FovOptionCtor, L"FovOptionCtor", L"field of view FloatOption vtable and range (ExtendedFov)",
     "BA 2F 00 00 00 45 31 C0 41 B9 10 00 00 00 E8 ? ? ? ? 48 8D 05 ? ? ? ? 48 89 03 48 B8 00 00 F0 41 00 00 DC 42 "
     "48 89 43 10 C7 43 20 6F 12 83 3A 48 B8 00 00 70 42 00 00 70 42 48 89 43 18"},

    {Target::FovRenderClamp, L"FovRenderClamp", L"upper clamp of the rendered field of view (ExtendedFov)",
     "F3 0F 5E FA 0F 28 C7 F3 0F 5F 05 ? ? ? ? F3 0F 10 0D ? ? ? ? 0F 28 F1 F3 0F C2 F7 01 0F 54 CE 0F 55 F0 0F 56 F1"},

    {Target::KeyBindingUnassignConflicts, L"KeyBindingUnassignConflicts",
     L"unassign other key bindings that use the key just bound (AllowDuplicateKeys)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 83 EC 68 48 8D 6C 24 60 48 C7 45 00 FE FF FF FF 48 89 D7 48 89 CB "
     "48 8D 55 C8 E8 ? ? ? ? 48 8B 75 C8 4C 8B 65 D0 4C 39 E6 0F 84 ? ? ? ? 4C 8D 7D E0 EB ? C7 02 00 00 00 00 "
     "48 83 40 28 04 48 8B 4B 10 4C 89 FA E8"},

    {Target::KeyBindingUnassignOthers, L"KeyBindingUnassignOthers",
     L"unassign other key bindings after resetting a binding (AllowDuplicateKeys)",
     "55 41 57 41 56 56 57 53 48 83 EC 68 48 8D 6C 24 60 48 C7 45 00 FE FF FF FF 48 89 D7 48 89 CB 48 8D 55 E8 "
     "E8 ? ? ? ? 48 8B 75 E8 4C 8B 7D F0 4C 39 FE 74 ? 4C 8D 75 D0 EB ? 0F 1F 44 00 00 48 83 C6 10 4C 39 FE 74 ? "
     "48 39 3E 74 ? 0F 10 06 0F 29 45 D0 48 89 D9 4C 89 F2 E8"},

    {Target::EnchantCommandExecute, L"EnchantCommandExecute", L"EnchantCommand::execute (ExtendedEnchantLevel)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 98 04 00 00 48 8D AC 24 80 00 00 00 0F 29 B5 00 04 00 00 "
     "48 C7 85 F8 03 00 00 FE FF FF FF 4D 89 C5 48 89 CB 44 8B B1 E8 00 00 00 41 80 FE 2B 75"},

    {Target::EnchantLevelRangeCheckSite, L"EnchantLevelRangeCheckSite",
     L"/enchant's level range check call (ExtendedEnchantLevel)",
     "48 8B 01 48 8B 40 28 FF 15 ? ? ? ? 8B 8B F0 00 00 00 BA 01 00 00 00 41 89 C0 4D 89 E9 E8 ? ? ? ? 84 C0 0F 84"},

    {Target::ApplyEnchantCanEnchantSite, L"ApplyEnchantCanEnchantSite",
     L"EnchantUtils::applyEnchant's can-enchant call (ExtendedEnchantLevel)",
     "48 8B 3F 48 89 7D F8 48 89 EA 4C 89 F1 49 89 F8 41 89 D9 E8 ? ? ? ? B8 04 00 00 00"},

    {Target::EnchantCanEnchantCheckSite, L"EnchantCanEnchantCheckSite",
     L"EnchantUtils::canEnchant's can-enchant call (ExtendedEnchantLevel)",
     "4C 89 C7 48 89 CE 4C 8D 75 A8 48 89 D1 4C 89 F2 E8 ? ? ? ? 4C 8B 07 4C 89 F1 48 89 F2 41 89 D9 E8 ? ? ? ?"},

    {Target::StructureSizeClampServer, L"StructureSizeClampServer",
     L"integrated server's 64-block clamp of the structure block size (ExtendedStructureSize)",
     "41 29 C9 41 83 FA 40 41 BB 40 00 00 00 45 0F 4D D3"},

    {Target::MobEffectScreenListLoop, L"MobEffectScreenListLoop",
     L"vanilla mob effect screen's effect list walk (InventoryEffects)",
     "41 FF C4 0F B6 47 26 41 88 46 F8 8B 05 ? ? ? ? 4C 8D 45 C8 44 8B 75 BC 0F 1F 00 48 81 C7 90 00 00 00 48 39 DF "
     "0F 84 ? ? ? ? 8B 0F 48 83 F9 25 77 ? 39 C1 74 ? 49 8B 4C CD 00 48 85 C9 74 ? 83 B9 A0 00 00 00 00 78 ? "
     "8B 4F 04 85 C9 74 ? 83 F9 FF"},

    {Target::MobEffectInstanceDisplayName, L"MobEffectInstanceDisplayName",
     L"MobEffectInstance::getDisplayName (InventoryEffects)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC F8 00 00 00 48 8D AC 24 80 00 00 00 48 C7 45 70 FE FF FF FF "
     "48 89 D6 48 89 CB 8B 05 ? ? ? ? 8B 0D ? ? ? ? 65 48 8B 14 25 58 00 00 00 48 8B 0C CA 3B 81 04 00 00 00 "
     "0F 8F ? ? ? ? 0F 57 C0 0F 29 45 B0 8B 03 48 83 F8 25 76 09 4C 8D 05 ? ? ? ? EB 20 48 8D 0D ? ? ? ? "
     "48 8B 04 C1 48 8D 88 80 00 00 00 48 85 C0 4C 8D 05 ? ? ? ? 4C 0F 45 C1 48 8D 0D ? ? ? ? 48 8B 05 ? ? ? ? "
     "48 8B 80 80 00 00 00 48 8D 7D F8 4C 8D 4D B0 48 89 FA FF 15 ? ? ? ? 8B 43 20 FF C8 83 F8 04"},

    {Target::MobEffectDurationText, L"MobEffectDurationText", L"mob effect duration as m:ss (InventoryEffects)",
     "55 56 57 53 48 81 EC 98 00 00 00 48 8D AC 24 80 00 00 00 48 C7 45 10 FE FF FF FF 48 89 CE 48 63 C2 "
     "48 69 C8 67 66 66 66 48 89 CA 48 C1 EA 3F 48 C1 E9 20 C1 F9 03 01 D1 48 69 F8 B5 81 4E 1B"},

    {Target::MobEffectIconNameSite, L"MobEffectIconNameSite", L"mob effect icon name fields (InventoryEffects)",
     "8B 04 01 48 83 F8 25 0F 87 ? ? ? ? 48 8D 0D ? ? ? ? 48 8B 04 C1 48 85 C0 0F 84 ? ? ? ? 48 8D 0D ? ? ? ? "
     "48 89 4D B0 48 C7 45 B8 0C 00 00 00 31 C9 48 83 B8 E0 00 00 00 00 0F 95 C1 C1 E1 05 48 8B 94 01 C0 00 00 00 "
     "48 83 BC 01 C8 00 00 00 10"},

    {Target::MobEffectColorStore, L"MobEffectColorStore", L"MobEffect colour fields (InventoryEffects)",
     "F3 0F 2A D7 F3 0F 5E D1 F3 0F 2A D9 F3 0F 5E D9 F3 0F 11 46 10 F3 0F 11 56 14 F3 0F 11 5E 18 "
     "C7 46 1C 00 00 80 3F"},

    {Target::MobEffectsRendererRender, L"MobEffectsRendererRender", L"HUD mob effect icons renderer (EffectTimer)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 98 06 00 00 48 8D AC 24 80 00 00 00 44 0F 29 BD 00 06 00 00 "
     "44 0F 29 B5 F0 05 00 00 44 0F 29 AD E0 05 00 00 44 0F 29 A5 D0 05 00 00 44 0F 29 9D C0 05 00 00 44 0F 29 95 B0 05 00 00 "
     "44 0F 29 8D A0 05 00 00 44 0F 29 85 90 05 00 00 0F 29 BD 80 05 00 00 0F 29 B5 70 05 00 00 48 C7 85 68 05 00 00 FE FF FF FF "
     "4C 89 CF 4D 89 C5 48 89 D3 49 89 CF 48 8D 05 ? ? ? ? 48 89 85 E0 03 00 00 48 C7 85 E8 03 00 00 25 00 00 00"},

    {Target::MobEffectsRendererOwnerSite, L"MobEffectsRendererOwnerSite", L"GuiData slot and owner position in the effect renderer (EffectTimer)",
     "49 8B 45 00 48 8B 80 C8 07 00 00 4C 89 E9 FF 15 ? ? ? ? 49 89 C7 F6 47 18 01 74 ? 48 89 F9 E8 ? ? ? ? "
     "F3 0F 2C 47 10 41 89 47 44"},

    {Target::MobEffectsRendererLoop, L"MobEffectsRendererLoop", L"effect layout records walk in the effect renderer (EffectTimer)",
     "44 89 C8 49 8B 78 18 49 8B 48 20 48 29 F9 48 C1 F9 02 49 0F AF CA 48 39 C1 0F 86 ? ? ? ? 4C 8B 32 48 8B 4A 08 4C 29 F1 "
     "48 C1 F9 04 49 0F AF CA 48 39 C1 0F 86 ? ? ? ? 41 83 F9 25 77 ? 49 8B 1C C3 48 85 DB 74 ? 4C 8D 24 C0 4C 89 E0 48 C1 E0 04 "
     "49 01 C6 8B 05 ? ? ? ? 41 39 06 74 ? 83 BB A0 00 00 00 00 78"},

    {Target::MobEffectsRendererBackground, L"MobEffectsRendererBackground", L"effect background rect field in the effect renderer (EffectTimer)",
     "4E 8D 24 A7 49 8D 54 24 14 F3 0F 11 5C 24 38 F3 0F 11 54 24 30 F3 0F 11 4C 24 28 F3 0F 11 44 24 20 "
     "C7 44 24 48 00 00 00 00 C7 44 24 40 00 00 00 00 48 8B 8D ? ? ? ? E8"},

    {Target::UiControlPositionStore, L"UiControlPositionStore", L"UIControl absolute position fields (EffectTimer)",
     "F3 0F 58 7E 44 F3 0F 58 76 40 F3 0F 11 76 10 F3 0F 11 7E 14 80 66 18 FE"},

    {Target::GuiDataGuiScale, L"GuiDataGuiScale", L"GuiData::getGuiScale (EffectTimer)",
     "55 56 57 53 48 83 EC 58 48 8D 6C 24 50 0F 29 75 F0 48 C7 45 E8 FE FF FF FF 48 89 CE 80 B9 F8 0C 00 00 01 "
     "0F 85 ? ? ? ? 48 8B 8E 98 00 00 00 48 8B 01 48 8B 80 10 01 00 00"},

    {Target::MobEffectsLayout, L"MobEffectsLayout", L"HUD mob effect icon layout (EffectTimer columns)",
     "41 57 41 56 41 55 41 54 56 57 55 53 48 81 EC F8 00 00 00 44 0F 29 BC 24 E0 00 00 00 44 0F 29 B4 24 D0 00 00 00 "
     "44 0F 29 AC 24 C0 00 00 00 44 0F 29 A4 24 B0 00 00 00 44 0F 29 9C 24 A0 00 00 00 44 0F 29 94 24 90 00 00 00 "
     "44 0F 29 8C 24 80 00 00 00 44 0F 29 44 24 70 0F 29 7C 24 60 0F 29 74 24 50 48 89 CE 48 8B 05 ? ? ? ? 48 31 E0 "
     "48 89 44 24 48 48 8B 09 48 8B 01 48 8B 80 F8 00 00 00 FF 15 ? ? ? ? 48 85 C0 0F 84 ? ? ? ? 80 B8 ? ? 00 00 00 "
     "0F 85 ? ? ? ? 48 89 C7 C7 46 08 FF FF 7F 7F C7 46 14 00 00 80 00"},

    {Target::MobEffectsLayoutRects, L"MobEffectsLayoutRects", L"HUD mob effect rect fields written by the layout (EffectTimer columns)",
     "F3 0F 11 4F 14 F3 0F 11 5F 18 F3 0F 11 6F 1C F3 0F 11 57 20 8B 4E 38 8B 56 3C 44 8D 04 02 0F 57 D2 F3 41 0F 2A D0 "
     "01 CA 0F 57 DB F3 0F 2A DA F3 0F 58 D8 0F 57 E4 F3 0F 2A E0 0F 57 ED F3 0F 2A E9 F3 0F 58 E8 F3 0F 11 6F 04 "
     "F3 0F 11 5F 08 F3 0F 11 67 0C F3 0F 11 57 10"},

    {Target::TickingTextureRender, L"TickingTextureRender", L"TickingTextureStage::render (NoBlockAnimation)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 78 01 00 00 48 8D AC 24 80 00 00 00 0F 29 B5 E0 00 00 00 "
     "48 C7 85 D8 00 00 00 FE FF FF FF 48 89 D3 48 89 CF 8B 05 ? ? ? ? 8B 0D ? ? ? ? 65 48 8B 14 25 58 00 00 00"},

    {Target::ItemActorRender, L"ItemActorRender", L"ItemRenderer::render for dropped items (NoRender Item)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 28 01 00 00 48 8D AC 24 80 00 00 00 44 0F 29 9D 90 00 00 00 "
     "44 0F 29 95 80 00 00 00 44 0F 29 4D 70 44 0F 29 45 60 0F 29 7D 50 0F 29 75 40 48 C7 45 38 FE FF FF FF "
     "49 8B 30 48 85 F6 0F 84 ? ? ? ? 4D 89 C7 48 89 D7 48 89 CB 48 89 F1 BA 40 00 00 00 E8 ? ? ? ? 84 C0 "
     "0F 84 ? ? ? ? 80 BE D3 03 00 00 01 0F 85 ? ? ? ? 48 8B 86 B8 03 00 00 48 85 C0 0F 84 ? ? ? ? "
     "48 83 38 00 0F 84 ? ? ? ? 4C 8D B6 B0 03 00 00"},

};

static_assert(std::size(kTargets) == static_cast<size_t>(Target::Count));

}
