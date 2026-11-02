#pragma once

#include <string_view>

namespace tsukuyomi {

enum class Target {
    CameraUpdate,
    CameraProjectionTangent,
    CameraProjectionTangentAlt,

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

    CraftFromOutput,

    ContainerTransfer,
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
    PlayerCommandParamSite,

    PositionCommandParamSite,
    BlockPositionCommandParamSite,
    UpdateAbilitiesPacketSite,

    CommandParserCtorSite,
    CommandParseSite,
    CommandParserDtorSite,
    CommandParseErrorParamsSite,

    CommandAutoComplete,
    CommandAutoCompleteFilter,
    ClientGetGuiDataSite,
    GuiDataDisplayClientMessage,
    GuiDataSetActionBar,
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
    SettingsGroupResolve,
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

    BlockTessellate,
    OpenHowToPlayScreen,
    HitResultAssign,

    HitResultPick,
    HitResultMoveAssign,

    BeRenderLoop,

    LevelBuildDispatch,

    BlockTransform,
    NameTagStage,

    NameTagStageCaller,

    BlockOutlineDraw,

    FogSettingsFetch,
    FogColorFetch,
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
    OpenInventoryScreen,
    MoveIntentFromInput,
    InputGather,

    InputGatherKeyCallSite,

    ContainerSmHandle,
    ContainerSmOffsetSite,
    ContainerMcOffsetSite,
    ContainerGetItem,
    BlockAsItemInstance,
    BlockGetPlacementBlock,
    BlockUseListenerSite,

    BlockUse,
    ItemStackBaseDtor,
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

    ItemStorageComponentSite,
    TradeToggleInvoke,

    TradeCurrentTier,

    TradePossible,

    ScenePopCall,

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

    CompoundTagHash,

    ServerPlayerVtableRef,

    HudScreenCtor,

    HudGetItemSite,

    RenderCurrentFrame,
    ServerLevelTick,
    PacketCheckSize,
    NetworkSend,
    StartGameHandle,
    TransferHandle,
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

    GameButtonActionTableSite,
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

    BuildBlockPlaceCheckSite,

    BuildBlockKeptClickSite,

    PlayerGameModeSite,

    ArmorContainerGetter,
    SimpleContainerGetItem,

    GuiScaleCompute,
    GuiScaleProviderVtableSite,
    ClientRefreshScreenSize,
    AccessibilitySettingsBuild,
    VideoSettingsBuild,

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

    GetBlockTessellator,
    BlockRenderLayerOf,
    TerrainMaterialRegistrySite,
    AirBlockLookupSite,

    BlockTypeIsValidSpawn,

    InvisibleLayerBarrier,
    InvisibleLayerLight,
    InvisibleLayerStructureVoid,
    SpectatorViewerFlag,
    InvisibleTaskBarrier,
    InvisibleTaskLight,
    InvisibleTaskStructureVoid,

    BlockSourceSetBlock,

    SubChunkRequestSite,

    LocatorBarRendererRender,
    LocatorBarWaypointList,
    LocatorBarWaypointFields,
    LocatorBarWaypointVisible,
    LocatorBarWaypointColor,
    LocatorBarIconCall,

    NameTagVisibleGate,
    NameTagSneakCheck,
    NameTagSelfSkip,
    NameTagBuild,
    NameTagDrawCall,
    NameTagFontWidthCall,

    SkyDescriptionDimensionCall,
    SkyStateEndCheckCalls,
    DimensionTimeOfDayBase,
    SkyColorBiomeCall,
    SkyFogBrightnessCall,
    OverworldFogBrightness,
    DimensionSunAngleBase,

    WeatherTickLevels,
    SkyDarkenWeatherLoad,

    CameraDirectLookTurn,
    CameraOrbitTurn,

    VehicleOrbitFollow,
    VehicleLookFollow,

    DismountButton,
    HotbarScrollButton,
    HudHotbarScrollNext,
    HudHotbarScrollPrev,

    ReloadUiDefinitions,
    Count,
};

inline constexpr std::string_view kEmoteIsValidBindingShape =
    "41 57 41 56 56 57 53 48 83 EC 40 4C 89 C6 48 89 D7 48 8B 05 ? ? ? ? 48 31 E0 48 89 44 24 38 4C 8B 79 08 4D 8D 70 08 4C 89 F1 E8 ? ? ? ? BB FF FF FF FF 84 C0 75 ? 4C 89 F1 E8 ? ? ? ? 84 C0 74 ? 48 8D 15 ? ? ? ? 4C 89 F1 E8 ? ? ? ? 49 89 C6 48 89 C1 E8 ? ? ? ? 84 C0 75 ? 4C 89 F1 E8 ? ? ? ? 84 C0 74 ? 4C 89 F1 31 D2 E8 ? ? ? ?";

inline constexpr std::string_view kGameSessionCatchupTickShape =
    "49 8B 4F 40 E8 ? ? ? ? 90 49 8B 4F 48 48 85 C9 74 ? 48 8B 41 10 48 83 C1 10 48 8B 40 20 FF 15 ? ? ? ? 90 49 8B 4F 50 48 85 C9 74 ? 48 8B 01 48 8B 40 20 FF 15 ? ? ? ? 90 0F 1F 44 00 00 4C 8B BF 98 00 00 00 4D 85 FF 0F 84 ? ? ? ? E8 ? ? ? ?";

struct TargetInfo {
    Target target;
    const wchar_t* name;
    const wchar_t* purpose;
    std::string_view signature;
};

inline constexpr TargetInfo kTargets[] = {

    {Target::CameraDirectLookTurn, L"CameraDirectLookTurn", L"turn the first person camera by the mouse (AngleSnap)",
     "56 48 83 EC 30 0F 29 74 24 20 4C 89 C6 F3 0F 10 05 ? ? ? ? F3 41 0F 10 49 04 F3 0F 59 C8 F3 41 0F 10 50 08 "
     "F3 0F 5C D1 F3 41 0F 11 50 08"},

    {Target::CameraOrbitTurn, L"CameraOrbitTurn", L"turn the third person camera by the mouse (AngleSnap)",
     "56 48 83 EC 30 0F 29 74 24 20 4C 89 C6 F3 41 0F 10 09 0F 28 15 ? ? ? ? 41 80 78 20 00 74 ? 0F 57 CA F3 41 0F 10 41 04"},

    {Target::VehicleOrbitFollow, L"VehicleOrbitFollow", L"third person camera follows and is limited by the vehicle (FreeCamera)",
     "F3 0F 10 47 34 F3 41 0F 58 C0 F3 44 0F 10 65 F8 F3 41 0F 5C C4 F3 44 0F "
     "10 0D ? ? ? ? F3 41 0F 58 C1 48 8B 35 ? ? ? ? F3 0F 10 35 ? ? "
     "? ? 0F 28 CE FF ? 0F 28 C8 F3 0F 58 CE 45 0F 57 D2 0F 28 D0 F3 41 0F "
     "C2 D2 01 0F 54 CA 0F 55 D0 0F 56 D1 F3 44 0F 10 1D ? ? ? ? F3 41 0F "
     "58 D3 F3 0F 58 FA F3 0F 10 4D FC 0F 28 15 ? ? ? ? 0F 57 D1 0F 28 C1 "
     "F3 0F C2 C7 01 F3 0F 5F FA 0F 28 D0 0F 55 D7 0F 54 C1 0F 56 C2 F3 41 0F "
     "58 C4 F3 41 0F 58 C1 0F 28 CE FF ? 0F 28 D0 F3 0F 58 D6 0F 28 C8 F3 41 "
     "0F C2 CA 01 0F 54 D1 0F 55 C8 0F 56 CA F3 41 0F 58 CB F3 41 0F 5C C8 F3 "
     "41 0F 58 C9 0F 28 C1 0F 28 CE FF ? F3 0F 58 F0 0F 28 C8 F3 41 0F C2 CA "
     "01 0F 54 F1 0F 55 C8 0F 56 CE F3 41 0F 58 CB F3 0F 11 4F 34 0F 28 75 20 "
     "0F 28 7D 30"},

    {Target::VehicleLookFollow, L"VehicleLookFollow", L"first person camera follows and is limited by the vehicle (FreeCamera)",
     "48 8D 34 92 F3 44 0F 10 05 ? ? ? ? F3 0F 10 45 F8 F3 41 0F 58 C0 F3 "
     "44 0F 10 0D ? ? ? ? F3 41 0F 58 C1 48 8B 1D ? ? ? ? F3 0F 10 35 "
     "? ? ? ? 0F 28 CE FF ? 0F 28 C8 F3 0F 58 CE 45 0F 57 D2 44 0F 28 E0 "
     "F3 45 0F C2 E2 01 41 0F 54 CC 44 0F 55 E0 44 0F 56 E1 F3 45 0F 58 E0 F3 "
     "0F 10 04 B7 44 0F 28 1D ? ? ? ? 41 0F 57 C3 F3 41 0F 5C C4 F3 41 0F "
     "58 C1 0F 28 CE FF ? 0F 28 C8 F3 0F 58 CE 0F 28 D0 F3 41 0F C2 D2 01 0F "
     "54 CA 0F 55 D0 0F 56 D1 F3 41 0F 58 D0 F3 0F 5C 54 B7 08 F3 0F 58 FA F3 "
     "0F 10 4D FC 0F 28 C1 F3 0F C2 C7 01 0F 28 D0 0F 54 C1 41 0F 57 CB F3 0F "
     "5F F9 0F 55 D7 0F 56 C2 F3 41 0F 58 C4 F3 41 0F 58 C1 0F 28 CE FF ? F3 "
     "0F 58 F0 0F 28 C8 F3 41 0F C2 CA 01 0F 54 F1 0F 55 C8 0F 56 CE F3 41 0F "
     "58 C8 41 0F 57 CB F3 0F 5C 0C B7 F3 0F 11 4C B7 08 E9 ? ? ? ? 48 8B "
     "8D F8 00 00 00"},

    {Target::DismountButton, L"DismountButton", L"button.dismount handler: stop riding the vehicle (FreeCamera)",
     "48 83 EC 28 48 8B 01 48 8B 80 F0 00 00 00 FF 15 ? ? ? ? 48 85 C0 0F 84 ? ? ? ? 48 8B "
     "50 10 8B 48 18 4C 8B 42 48 4C 8B 4A 50 4D 29 C1"},

    {Target::HotbarScrollButton, L"HotbarScrollButton", L"scroll the hotbar selection by the wheel (FreeCamera)",
     "41 56 56 57 53 48 83 EC 38 89 D7 48 89 CE 48 8B 01 48 8B 80 F0 00 00 00 FF 15 ? ? ? "
     "? 48 85 C0"},

    {Target::HudHotbarScrollNext, L"HudHotbarScrollNext", L"HUD selects the next hotbar slot by the wheel (FreeCamera)",
     "56 57 48 83 EC 28 48 8B 71 08 31 FF 83 BE 94 0D 00 00 00 0F 8E ? ? ? "
     "? 48 8B 86 48 0D 00 00 48 8B 48 48 48 85 C9 0F 84 ? ? ? ? 80 39 00 "
     "75 ? C7 04 25 00 00 00 00 DE C0 AD DE 48 8B 48 58 48 8B 01 48 8B 80 18 "
     "05 00 00 FF 15 ? ? ? ? 84 C0 0F 85 ? ? ? ? 48 8B 86 48 0D 00 00 "
     "48 8B 48 48 48 85 C9 75 ? C7 04 25 00 00 00 00 DE C0 AD DE 80 39 00 75 "
     "? C7 04 25 00 00 00 00 DE C0 AD DE 48 8B 48 58 48 8B 01 48 8B 80 F0 00 "
     "00 00 FF 15 ? ? ? ? 31 C9 48 85 C0 0F 95 C1 48 8B 86 48 0D 00 00 48 "
     "8B 50 48 48 85 D2 75 ? C7 04 25 00 00 00 00 DE C0 AD DE 8D 3C CD 01 00 "
     "00 00 80 3A 00 75 ? C7 04 25 00 00 00 00 DE C0 AD DE 48 8B 48 58 48 8B "
     "01 48 8B 80 F0 00 00 00 FF 15 ? ? ? ? 48 8B 80 E0 05 00 00 44 0F B6 "
     "80 B0 00 00 00 8B 40 10 FF C0 99 F7 FF 48 89 F1 E8 ? ? ? ? BF 01 00 "
     "00 00 89 F8 48 83 C4 28"},

    {Target::HudHotbarScrollPrev, L"HudHotbarScrollPrev", L"HUD selects the previous hotbar slot by the wheel (FreeCamera)",
     "56 57 48 83 EC 28 48 8B 71 08 31 FF 83 BE 94 0D 00 00 00 0F 8E ? ? ? "
     "? 48 8B 86 48 0D 00 00 48 8B 48 48 48 85 C9 0F 84 ? ? ? ? 80 39 00 "
     "75 ? C7 04 25 00 00 00 00 DE C0 AD DE 48 8B 48 58 48 8B 01 48 8B 80 18 "
     "05 00 00 FF 15 ? ? ? ? 84 C0 0F 85 ? ? ? ? 48 8B 86 48 0D 00 00 "
     "48 8B 48 48 48 85 C9 75 ? C7 04 25 00 00 00 00 DE C0 AD DE 80 39 00 75 "
     "? C7 04 25 00 00 00 00 DE C0 AD DE 48 8B 48 58 48 8B 01 48 8B 80 F0 00 "
     "00 00 FF 15 ? ? ? ? 31 C9 48 85 C0 0F 95 C1 48 8B 86 48 0D 00 00 48 "
     "8B 50 48 48 85 D2 75 ? C7 04 25 00 00 00 00 DE C0 AD DE 8D 3C CD 01 00 "
     "00 00 80 3A 00 75 ? C7 04 25 00 00 00 00 DE C0 AD DE 48 8B 48 58 48 8B "
     "01 48 8B 80 F0 00 00 00 FF 15 ? ? ? ? 48 8B 80 E0 05 00 00 44 0F B6 "
     "80 B0 00 00 00 8B 40 10 85 C0 0F 4F F8 FF CF 48 89 F1 89 FA E8 ? ? ? "
     "? BF 01 00 00 00 89 F8 48 83 C4 28"},

    {Target::NameTagVisibleGate, L"NameTagVisibleGate", L"only the targeted actor gets a name tag (NameTags)",
     "80 7C 50 01 00 75 ? 66 90 48 8B 85 98 01 00 00 48 8B 88 A0 11 00 00 48 "
     "8B 01 48 8B 80 78 0A 00 00 FF 15 ? ? ? ? 90 48 89 C1 E8 ? ? ? ? "
     "90 48 39 C3 0F 85 ? ? ? ? 48 89 D9 4C 89 E2 E8 ? ? ? ? 90 48 83 "
     "BD 00 01 00 00 00 0F 84 ? ? ? ?"},

    {Target::NameTagSneakCheck, L"NameTagSneakCheck", L"sneaking actors get a depth tested name tag (NameTags)",
     "48 89 F9 BA 01 00 00 00 E8 ? ? ? ? 90 84 C0 74 ? F3 0F 10 05 ? ? "
     "? ? EB ? 48 8B B5 F8 00 00 00 48 85 F6 74 ? F0 FF 4E 08 75 ? 48 8B "
     "06 48 8B 00 48 89 F1 FF 15 ? ? ? ? F0 FF 4E 0C 75 ? 48 8B 06 48 8B "
     "40 08 48 89 F1 FF 15 ? ? ? ? 0F 57 C0 41 0F 11 06 49 C7 46 10 00 00 "
     "00 00 4C 89 F0 E9 ? ? ? ? 48 89 F9 BA 81 00 00 00 E8 ? ? ? ? 90 "
     "84 C0 0F 84 ? ? ? ? F3 0F 10 85 0C 01 00 00 48 8B 95 48 01 00 00"},

    {Target::NameTagSelfSkip, L"NameTagSelfSkip", L"the local player gets no name tag (NameTags)",
     "48 8B 1E 48 3B 9D B8 00 00 00 74 ? 48 8B 03 48 8B 80 F8 00 00 00 48 89 "
     "D9 FF 15 ? ? ? ? 90 84 C0 75 ? 48 8B 4B 10 8B 43 18 48 8B 51 48"},

    {Target::NameTagBuild, L"NameTagBuild", L"build one name tag from an actor and its text (NameTags)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC ? ? ? ? 48 8D AC 24 ? ? ? ? 44 0F 29 95 ? ? ? ? 44 0F 29 8D ? ? ? ? "
     "44 0F 29 85 ? ? ? ? 0F 29 BD ? ? ? ? 0F 29 B5 ? ? ? ? 48 C7 85 ? ? ? ? FE FF FF FF 4C 89 8D ? ? ? ? 4D 89 C7 "
     "49 89 D6 48 89 CB 48 8B BD ? ? ? ? 48 8B 07 48 8B 80 ? ? ? ? 48 89 F9 FF 15 ? ? ? ? 48 8B 10 B9"},

    {Target::NameTagDrawCall, L"NameTagDrawCall", L"draw one name tag inside the name tag stage (Waypoints)",
     "49 8B 86 ? ? ? ? 48 8B 80 ? ? ? ? 48 89 44 24 20 48 89 D9 48 89 FA 4D 89 F8 49 89 F1 E8 ? ? ? ? 49 81 C7 90 00 00 00"},

    {Target::NameTagFontWidthCall, L"NameTagFontWidthCall", L"measure a name tag line with the font (Waypoints)",
     "48 8B 06 48 8B 40 30 48 89 F1 48 8D 55 28 0F 28 D6 45 31 C9 FF 15 ? ? "
     "? ? 90 48 85 DB"},

    {Target::LocatorBarRendererRender, L"LocatorBarRendererRender", L"HUD locator bar renderer (LocatorBar)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC D8 06 00 00 48 8D AC 24 80 "
     "00 00 00 44 0F 29 BD 40 06 00 00 44 0F 29 B5 30 06 00 00 44 0F 29 AD 20 "
     "06 00 00 44 0F 29 A5 10 06 00 00 44 0F 29 9D 00 06 00 00 44 0F 29 95 F0 "
     "05 00 00 44 0F 29 8D E0 05 00 00 44 0F 29 85 D0 05 00 00 0F 29 BD C0 05 "
     "00 00 0F 29 B5 B0 05 00 00 48 C7 85 A8 05 00 00 FE FF FF FF 4C 89 CB 4C "
     "89 C6 49 89 D4 48 89 CF 49 8B 00 48 8B 80 F0 00 00 00 4C 89 C1 FF 15 ? "
     "? ? ? 48 85 C0 0F 84 ? ? ? ?"},

    {Target::LocatorBarWaypointList, L"LocatorBarWaypointList", L"local player's waypoint list in the locator bar (LocatorBar)",
     "4D 8B AD 00 0F 00 00 4D 8B 65 00 4D 39 EC 74 ? 4C 8D 75 08 EB ? 48 89 02 48 83 85 "
     "18 05 00 00 08 4D 8B 24 24 4D 39 EC 74 ? 49 8B 44 24 20 48 85 C0 74 ? 48 89 45 08 "
     "48 8B 95 18 05 00 00"},

    {Target::LocatorBarWaypointFields, L"LocatorBarWaypointFields", L"waypoint position and active flag in the locator bar (LocatorBar)",
     "49 8B 45 00 80 78 60 00 74 ? F3 44 0F 10 60 0C F3 0F 10 40 10 F3 44 0F 10 58 08 44 "
     "0F 14 D8 80 78 74 01 45 0F 28 F4 45 0F 28 EB 75 ? F3 44 0F 10 70 6C F3 0F 10 40 70 "
     "F3 44 0F 10 68 68"},

    {Target::LocatorBarWaypointVisible, L"LocatorBarWaypointVisible", L"waypoint visible flag in the locator bar (LocatorBar)",
     "4D 8B 65 00 41 80 7C 24 50 01 0F 85 ? ? ? ? 0F 29 85 90 05 00 00 48 8B 06 48 8B "
     "80 38 03 00 00 48 89 F1 48 8D 95 40 04 00 00"},

    {Target::LocatorBarWaypointColor, L"LocatorBarWaypointColor", L"waypoint colour fields in the locator bar (LocatorBar)",
     "48 8B 00 80 78 28 00 48 8D 48 18 48 8D 15 ? ? ? ? 48 0F 44 CA 0F 10 01 0F 29 85 "
     "30 05 00 00 F3 44 0F 10 68 5C F3 0F 10 05 ? ? ? ?"},

    {Target::LocatorBarIconCall, L"LocatorBarIconCall", L"call that draws one locator bar icon (LocatorBar)",
     "66 0F 7E 4C 24 68 C7 84 24 80 00 00 00 00 00 80 3F C7 44 24 78 00 00 80 "
     "3F C7 44 24 70 FF FF FF FF C7 44 24 58 00 00 80 3F C7 44 24 40 00 00 80 "
     "3F C7 44 24 38 00 00 00 00 C7 44 24 30 00 00 80 3F C7 44 24 28 00 00 00 "
     "00 48 8D 55 08 4C 8D 85 B0 04 00 00 E8 ? ? ? ? 90 44 0F 28 B5 90 05 "
     "00 00"},

    {Target::CameraProjectionTangent, L"camera projection tangent", L"perspective projection from the camera FOV (Zoom)",
     "F3 0F 10 43 50 F3 0F 59 05 ? ? ? ? FF 15 ? ? ? ? F3 44 0F 59 D0 0F 28 CE F3 41 0F 5E CA 0F 28 D6 F3 0F 5E D0"},
    {Target::CameraProjectionTangentAlt, L"camera projection tangent alternate", L"alternate perspective projection from the camera FOV (Zoom)",
     "F3 0F 10 47 50 F3 0F 59 05 ? ? ? ? FF 15 ? ? ? ? F3 44 0F 59 D0 0F 28 CE F3 41 0F 5E CA F3 0F 5E F0"},

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
     "C7 85 D0 00 00 00 FE FF FF FF 44 89 CB 45 89 C6 49 89 D7 48 89 CE"},

    {Target::MobSwing, L"MobSwing", L"arm swing (no swing on mod-issued placements)",
     "56 57 53 48 83 EC 20 44 89 C3 48 89 CE 80 B9 5D 04 00 00 01 75 ? 8B BE 30 04 00 00 "
     "0F B6 96 5C 04 00 00 48 89 F1"},

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
     "56 48 83 EC 30 0F 29 74 24 20 48 8B 01 48 8B 50 08 8B 40 10 4C 8B 42 48 4C 8B 4A 50 "
     "4D 29 C1"},

    {Target::SetSelectedSlot, L"SetSelectedSlot", L"selected slot control (AutoTool)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 88 02 00 00 48 8D AC 24 80 00 00 00 48 "
     "C7 85 00 02 00 00 FE FF FF FF 45 84 C0 75 ? 83 FA 08"},

    {Target::AbilitiesAccess, L"AbilitiesAccess", L"LayeredAbilities getter call (CreativeNoClip)",
     "48 89 F1 E8 ? ? ? ? B9 C8 04 00 00 80 B8 94 05 00 00 01 75 ? B9 D8 03 00 00 "
     "80 B8 A4 04 00 00 01 75 ?"},

    {Target::PoseDecision, L"PoseDecision", L"pose decision: auto sneak / crawl in low spaces (CreativeNoClip)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 83 EC 58 48 8D 6C 24 50 48 C7 45 00 FE FF FF "
     "FF 4C 89 CF 4C 89 45 F8 48 89 D3 4C 8B B5 90 00 00 00"},

    {Target::UseItem, L"useItem", L"item use (FastUseItem)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 38 03 00 00 48 8D AC 24 80 00 00 00 48 "
     "C7 85 B0 02 00 00 FE FF FF FF 45 88 C7 48 89 D6 48 89 CF"},

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
     "4C 8B 89 98 01 00 00 49 63 C0 48 69 C0 98 00 00 00 4C 01 C8 48 63 CA 48 69 C9 98 00 00 00 4C 01 "
     "C9 48 89 C2 E9 ? ? ? ?"},

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

    {Target::CraftFromOutput, L"craftFromOutput", L"craft from the crafting output (ItemScroller mass craft)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 68 04 00 00 48 8D AC 24 80 00 00 00 0F "
     "29 B5 D0 03 00 00 48 C7 85 C8 03 00 00 FE FF FF FF 4C 89 CB 4C 89 C6 48 89 95 88 03 "
     "00 00 48 89 CF"},

    {Target::ContainerTransfer, L"containerTransfer", L"move items between the cursor and a slot (ItemScroller grid fill)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC F8 02 00 00 48 8D AC 24 80 00 00 00 48 C7 85 70 02 "
     "00 00 FE FF FF FF 4C 89 CE 4C 89 C3 41 89 D6 48 89 CF 4C 89 C9 4C 89 C2 E8 ? ? ? ? 84 C0 74 08 "
     "45 31 E4 E9"},

    {Target::NotifyInventoryOpen, L"notifyInventoryOpen",
     L"tell the server the inventory opened (ItemStackRequest)",
     "55 41 56 56 57 53 48 81 EC 90 00 00 00 48 8D AC 24 80 00 00 00 48 C7 45 08 FE FF FF "
     "FF 89 D7 48 89 CE 48 8B 01 48 8B 80 F0 00 00 00 FF 15 ? ? ? ?"},

    {Target::MoveInputHandler, L"moveInputHandler",
     L"skip the input reset while we tell the server the inventory opened",
     "48 83 EC 28 48 8B 01 48 8B 80 F0 00 00 00 FF 15 ? ? ? ? 48 85 C0 0F 84 ? ? ? ? 48 8B "
     "50 10 8B 48 18 8B 42 50 4C 8B 42 48 44 29 C0"},

    {Target::PlayerRotation, L"PlayerRotation",
     L"player body pitch/yaw writes (FreeCamera freezes the body)",
     "48 8B B6 50 02 00 00 F2 0F 10 4C 24 28 F3 0F 10 3D ? ? ? ? F3 0F 10 "
     "44 24 2C F3 0F 58 C7 F2 0F 11 0E 48 8B 3D ? ? ? ? F3 0F 10 35 ? ? "
     "? ? 0F 28 CE FF ? 0F 28 C8 F3 0F 58 CE 45 0F 57 C0 44 0F 28 D0 F3 45 "
     "0F C2 D0 01 41 0F 54 CA 44 0F 55 D0 44 0F 56 D1 F3 44 0F 10 0D ? ? ? "
     "? F3 45 0F 58 D1 41 0F 28 C2 F3 0F 5C 46 04 F3 0F 58 46 0C F3 0F 58 C7 "
     "0F 28 CE FF ? 0F 28 C8 F3 0F 58 CE 0F 28 D0 F3 41 0F C2 D0 01 0F 54 CA "
     "0F 55 D0 0F 56 D1 F3 41 0F 58 D1 F3 0F 11 56 0C F3 44 0F 11 56 04 F3 0F "
     "10 06 F3 0F 58 C7"},

    {Target::PlayerHeadRotation, L"PlayerHeadRotation",
     L"player facing write (FreeCamera freezes the head)",
     "81 E2 FF FF 03 00 83 E2 7F 0F 13 04 D0 E9 ? ? ? ? 89 7D E8 48 8D 55 "
     "E8 48 89 C1 E8 ? ? ? ? F3 0F 11 78 04 48 8B 4E 10 8B 46 18 48 8B 51 "
     "48 4C 8B 41 50 49 29 D0 49 C1 E8 03 41 FF C8 41 81 E0 11 72 BE BA 4E 8D "
     "0C C2 48 8B 51 68 66 66 66 2E 0F 1F 84 00 00 00 00 00 4D 8B 01 49 83 F8 "
     "FF 0F 84 ? ? ? ? 49 C1 E0 05 4E 8D 0C 02 42 81 7C 02 08 11 72 BE BA "
     "75 ? 4C 01 C2 48 39 51 70 74 ? 48 8B 4A 10 48 85 C9 74 ? 89 C2 81 E2 "
     "FF FF 03 00 41 89 D0 41 C1 E8 0B 4C 8B 49 08 4C 8B 51 10 4D 29 CA 49 C1 "
     "FA 03 4D 39 D0 73 ? 4F 8B 04 C1 4D 85 C0 74 ? 81 E2 FF 07 00 00 25 00 "
     "00 FC FF 41 8B 14 90 31 D0 3D FE FF 03 00 77 ? 48 8B 41 50 89 D1 C1 E9 "
     "04 81 E1 F8 3F 00 00 48 8B 04 08 48 85 C0 74 ? 81 E2 FF FF 03 00 83 E2 "
     "7F F3 0F 11 3C D0 F3 0F 11 7C D0 04 48 89 F1 E8 ? ? ? ?"},

    {Target::PlayerHeadRotationInput, L"PlayerHeadRotationInput",
     L"the other player facing write (FreeCamera freezes the head)",
     "81 E2 FF FF 03 00 83 E2 7F F3 0F 11 34 D0 F3 0F 11 44 D0 04 48 8B 8E 00 02 00 00 48 "
     "8B 01 48 8B 80 10 06 00 00"},

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
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 88 01 00 00 48 8D AC 24 80 00 00 00 48 "
     "C7 85 00 01 00 00 FE FF FF FF 48 89 4D 38 C6 41 60 01 48 8B 32"},

    {Target::SendCommandRequest, L"sendCommandRequest", L"run a chat command (ChatCommand hooks it)",
     "55 41 57 41 56 41 54 56 57 53 48 81 EC 90 01 00 00 48 8D AC 24 80 00 00 00 "
     "48 C7 85 08 01 00 00 FE FF FF FF 45 89 CE 4C 89 C7 48 89 D6 48 89 CB 48 8B 49 10 "
     "4C 89 C2 66 41 B8 40 00"},

    {Target::CommandRegistryLoadPacket, L"CommandRegistryLoadPacket", L"load the client command registry",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC C8 02 00 00 48 8D AC 24 80 00 00 00 0F "
     "29 B5 30 02 00 00 48 C7 85 28 02 00 00 FE FF FF FF 48 89 8D F0 01 00 00 0F 57 F6 0F "
     "29 B5 C0 00 00 00"},

    {Target::JsonCommandParamSite, L"JsonCommandParamSite", L"the json command parameter type registration (tellraw)",
     "48 8D 05 ? ? ? ? 48 89 05 ? ? ? ? C7 05 ? ? ? ? 4A 00 10 00 C3"},

    {Target::StringCommandParamSite, L"StringCommandParamSite", L"the string command parameter type registration (criteria)",
     "48 8D 05 ? ? ? ? 48 89 05 ? ? ? ? C7 05 ? ? ? ? 38 00 10 00 C3"},

    {Target::IntCommandParamSite, L"IntCommandParamSite", L"the int command parameter type registration (replaceitem)",
     "48 8D 05 ? ? ? ? 48 89 05 ? ? ? ? C7 05 ? ? ? ? 01 00 10 00 C3"},

    {Target::PlayerCommandParamSite, L"PlayerCommandParamSite", L"the player selector command parameter type registration (hud)",
     "48 8D 05 ? ? ? ? 48 89 05 ? ? ? ? C7 05 ? ? ? ? 08 00 10 00 C3 CC CC CC CC CC CC CC "
     "48 8D 05 ? ? ? ? 48 89 05 ? ? ? ? C7 05 ? ? ? ? 5A 00 10 00"},

    {Target::UpdateAbilitiesPacketSite, L"UpdateAbilitiesPacketSite", L"the UpdateAbilitiesPacket construction in RequestAbilityPacket handling",
     "41 38 C4 0F 84 ? ? ? ? 48 89 F9 E8 ? ? ? ? 48 8B 00 48 89 45 50 41 0F B6 07 88 45 58 41 "
     "0F B6 47 01 88 45 59 0F 57 C0 0F 11 45 60 48 C7 45 70 00 00 00 00 48 8D 05 ? ? ? ? 48 89 45 "
     "B0 48 8D 45 50 48 89 45 B8 48 8D 55 B0 48 89 55 E8 4C 89 F9 E8 ? ? ? ? 90 48 8B 4D E8 48 85 "
     "C9 74 ? 48 8D 45 B0 48 39 C1 0F 95 C2 48 8B 01 48 8B 40 20 FF 15 ? ? ? ? 48 B8 02 00 00 00 "
     "01 00 00 00 48 89 45 F8 66 C7 45 00 00 00 0F 57 C0 0F 11 45 08 C7 45 18 00 00 00 00 48 8D 1D ? "
     "? ? ?"},

    {Target::FloatCommandParamSite, L"FloatCommandParamSite", L"the float command parameter type registration (playsound)",
     "48 8D 05 ? ? ? ? 48 89 05 ? ? ? ? C7 05 ? ? ? ? 03 00 10 00 C3"},

    {Target::PositionCommandParamSite, L"PositionCommandParamSite", L"the position command parameter type registration (tp)",
     "48 8D 05 ? ? ? ? 48 89 05 ? ? ? ? C7 05 ? ? ? ? 41 00 10 00 C3"},

    {Target::BlockPositionCommandParamSite, L"BlockPositionCommandParamSite", L"the block position command parameter type registration (execute if block)",
     "48 8D 05 ? ? ? ? 48 89 05 ? ? ? ? C7 05 ? ? ? ? 40 00 10 00 C3"},

    {Target::CommandParserCtorSite, L"CommandParserCtorSite", L"command parser construction in getAutoCompleteOptions",
     "4C 8D 7D B0 4C 89 F9 48 89 FA 41 B8 37 00 00 00 E8 ? ? ? ? 90 44 89 6C 24 20"},

    {Target::CommandParseSite, L"CommandParseSite", L"parse a command string (stage IE)",
     "48 8D 4D C0 48 8D 95 80 00 00 00 E8 ? ? ? ? 90 84 C0 74 ? 4C 8B 45 38 4D 85 C0"},

    {Target::CommandParserDtorSite, L"CommandParserDtorSite", L"command parser destruction in getAutoCompleteOptions",
     "48 8D 4D B0 E8 ? ? ? ? 48 8B 85 70 01 00 00 48 81 C4 08 02 00 00 5B 5F 5E"},

    {Target::CommandParseErrorParamsSite, L"CommandParseErrorParamsSite", L"build the parse error parameters (stage IE)",
     "48 8D 4D C0 48 8D BD C0 00 00 00 48 89 FA E8 ? ? ? ? 90 48 8B B5 60 01 00 00"},

    {Target::CommandAutoComplete, L"CommandAutoComplete", L"build chat command completions (stage GB-5)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 08 02 00 00 48 8D AC 24 80 00 00 00 48 C7 85 80 01 00 00 "
     "FE FF FF FF 4C 89 CB 4C 89 C6 48 89 95 70 01 00 00 44 8B AD F0 01 00 00"},

    {Target::CommandAutoCompleteFilter, L"CommandAutoCompleteFilter", L"drop completions the player cannot use (stage GB-5)",
     "55 41 56 56 57 53 48 83 EC 50 48 8D 6C 24 50 48 C7 45 F8 FE FF FF FF 45 89 CE 4C 89 "
     "C3 49 89 D0 48 89 CF"},

    {Target::ClientGetGuiDataSite, L"ClientGetGuiDataSite", L"GuiData vtable slot in the chat caller",
     "48 8B 49 58 48 8B 01 48 8B 80 C0 06 00 00 48 8D 55 E8 FF 15 ? ? ? ? 48 8B 45 E8 48 85 C0 0F "
     "84 ? ? ? ? 80 38 00 75 ? C7 04 25 00 00 00 00 DE C0 AD DE 48 8B 4D F8 48 89 F2"},

    {Target::GuiDataDisplayClientMessage, L"GuiDataDisplayClientMessage", L"append a local chat message",
     "55 41 57 41 56 56 57 53 48 81 EC 88 02 00 00 48 8D AC 24 80 00 00 00 48 C7 85 00 02 00 00 FE FF FF FF 4D 89 CE 4C 89 C3 48 89 D7 48 89 CE 0F 57 C0"},

    {Target::GuiDataSetActionBar, L"GuiDataSetActionBar", L"show a message in the actionbar",
     "55 56 57 48 81 EC D0 00 00 00 48 8D AC 24 80 00 00 00 48 C7 45 48 FE FF FF FF 4C 89 "
     "C7 49 89 D0 48 89 CE 48 8B 89 98 00 00 00"},

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
     "56 57 55 53 48 83 EC 28 48 89 D6 48 89 CB 44 8B 05 ? ? ? ? 48 8D 3D ? ? ? ? 48 89 FA "
     "E8 ? ? ? ?"},

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

    {Target::SettingsGroupResolve, L"settingsGroupResolve", L"resolve a cached Ore UI settings group",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC E8 00 00 00 48 8D AC 24 80 00 00 00 48 C7 45 60 FE FF FF FF "
     "4C 89 C3 48 89 D6 48 89 CF 8B 05 ? ? ? ? 8B 0D ? ? ? ?"},

    {Target::SettingsGroupRegister, L"settingsGroupRegister", L"register an Ore UI settings group (id + provider)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 83 EC 68 48 8D 6C 24 60 48 C7 45 "
     "00 FE FF FF FF 0F 57 C0 0F 29 45 E0 48 8B 5A 08 48 85 DB 0F 88 ? ? ? "
     "? 4C 89 C6 48 89 CF 48 83 C7 08 4C 8B 32"},

    {Target::SettingsProviderCall, L"settingsProviderCall",
     L"std::function::_Do_call that builds a settings group's item vector",
     "55 41 56 56 57 53 48 81 EC 90 00 00 00 48 8D AC 24 80 00 00 00 "
     "48 C7 45 08 FE FF FF FF 49 89 D1 48 8B 71 08 0F 57 C0"},

    {Target::SettingsFindComponent, L"settingsFindComponent",
     L"Settings::IRegistry::find(component by id)",

     "41 57 41 56 41 55 41 54 56 57 55 53 48 83 EC 38 4C 89 44 24 30 "
     "48 89 D6 48 89 CB 8B 05 ?? ?? ?? ?? 8B 0D ?? ?? ?? ??"},

    {Target::GameAllocate, L"gameAllocate", L"current Bedrock allocator call site",
     "48 8B 0D ? ? ? ? 48 8B 01 48 8B 40 08 48 81 FA FF 0F 00 00 72 ? 4D "
     "8D 7D 28 4C 89 FA FF 15 ? ? ? ? 48 85 C0 0F 84 ? ? ? ? 49 89 C7 "
     "49 83 C7 27 49 83 E7 E0 49 89 47 F8 EB ? 4D 8D 65 01 4C 89 E2 FF 15 ? "
     "? ? ? 49 89 C7 48 85 C0 0F 84 ? ? ? ? 4C 89 7D E0 48 89 5D F0 4C "
     "89 6D F8 4C 89 F9 4C 89 F2 49 89 D8 E8 ? ? ? ? 41 C6 04 1F 00 48 8D "
     "55 D0 4C 8D 45 E0 48 89 F9"},

    {Target::PlaySound, L"playSound", L"Sound player used by the settings UI",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 28 03 00 00 48 8D AC 24 80 00 00 00 0F "
     "29 BD 90 02 00 00 0F 29 B5 80 02 00 00 48 C7 85 78 02 00 00 FE FF FF FF 0F 28 F3 F3 "
     "0F 10 0D ? ? ? ? F3 0F 59 CB"},

    {Target::OpenHowToPlayScreen, L"openHowToPlayScreen",
     L"open the How to Play screen (borrowed to show our own page)",
     "55 56 57 48 83 EC 50 48 8D 6C 24 50 48 C7 45 F8 FE FF FF FF 48 89 CF 48 "
     "8B 49 08 48 8B 01 48 8B 80 60 07 00 00 48 8D 55 E0 FF 15 ? ? ? ? 48 "
     "8B 45 E0 48 85 C0 0F 84 ? ? ? ? 80 38 00 75 ? C7 04 25 00 00 00 00 "
     "DE C0 AD DE 48 8B 75 F0 48 8B 4F 08 48 8B 01 48 8B 80 18 07 00 00 FF 15 "
     "? ? ? ? 90 48 8D 55 D0 48 89 C1 45 31 C0 E8 ? ? ? ? 90"},
    {Target::MakeOptionElement, L"makeOptionElement", L"Ore UI: build one option element",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 83 EC 58 48 8D 6C 24 50 48 C7 45 00 FE FF FF "
     "FF 4C 89 CE 4C 89 C7 89 11 48 8D 41 08 48 89 45 D8"},

    {Target::ViewVector, L"viewVector", L"Actor::getViewVector(partialTick)",
     "41 56 56 57 53 48 81 EC 98 00 00 00 44 0F 29 A4 24 80 00 00 00 44 0F 29 5C 24 70 "
     "44 0F 29 54 24 60 44 0F 29 4C 24 50 44 0F 29 44 24 40 0F 29 7C 24 30 0F 29 74 24 20 "
     "0F 28 F2 48 89 D6 48 8B 51 10 8B 41 18 48 8B 4A 48 4C 8B 42 50 49 29 C8 49 C1 E8 03 "
     "41 FF C8 41 81 E0 B7 36 DF 75 4E 8D 0C"},

    {Target::PlayerVtableRef, L"playerVtableRef", L"lea of the Player (LocalPlayer) vtable",
     "48 8D 05 ? ? ? ? 49 89 06 49 8D 8E E0 0C 00 00 49 8B 86 E0 05 00 00 4C 8B 80 B8 00 "
     "00 00 48 89 4D 50"},

    {Target::NetManagerVtableRef, L"netManagerVtableRef", L"lea of the net manager vtable",
     "48 8D 05 ? ? ? ? 48 89 01 48 8B 89 80 00 00 00 48 85 C9 74 ? F0 FF 49 0C 75 ? 48 8B 01 48 "
     "8B 40 08 FF 15 ? ? ? ? 48 8B 5E 70"},

    {Target::KeyResetVisibleVtableRef, L"keyResetVisibleVtableRef",
     L"lea of the reset-button visibility callable vtable",
     "48 8D 05 ? ? ? ? 48 89 85 00 05 00 00 48 89 9D 08 05 00 00 48 8B 8D 18 06 00 00 "
     "48 89 8D 10 05 00 00 4C 89 A5 38 05 00 00 4C 8D AD A8 00 00 00"},

    {Target::BlockRegistryRef, L"blockRegistryRef", L"lea of the block name table",
     "44 8B B0 C8 00 00 00 41 F7 D6 66 44 03 B0 CC 00 00 00 48 8D 8D 40 04 00 00 E8 ? ? "
     "? ? 90 C7 44 24 20 00 00 00 00 48 8D 0D ? ? ? ? 4C 8D 05 ? ? ? ? 48 8D 95 "
     "B8 06 00 00 45 31 C9 E8 ? ? ? ? 90 48 8B 85 B8 06 00 00 48 89 85 B8 06 00 00 C7 "
     "44 24 20 00 00 00 00 48 8D 0D ? ? ? ?"},

    {Target::SubChunkSetBlock, L"SubChunk::_setBlock", L"write one block into a sub chunk",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 88 00 00 00 48 8D AC 24 80 00 00 00 48 "
     "C7 45 00 FE FF FF FF 4C 89 CF 45 89 C4 41 89 D6 48 89 CE"},

    {Target::BlockTessellate, L"block tessellate", L"draw one block into the chunk mesh",
     "41 57 41 56 41 55 41 54 56 57 55 53 48 81 EC B8 00 00 00 0F 29 B4 24 A0 00 00 00 4C "
     "89 CF 4D 89 C6 48 89 D3"},

    {Target::HitResultAssign, L"HitResult::operator=", L"what the crosshair is on",
     "56 57 48 83 EC 28 48 89 C8 48 8B 4A 30 48 89 48 30 0F 10 02"},

    {Target::HitResultMoveAssign, L"HitResult::operator=(&&)", L"what the crosshair is on (entity hits)",
     "56 57 48 83 EC 38 0F 29 74 24 20 48 89 D7 48 89 CE 48 8B 42 30 48 89 41 30 0F 10 02 0F 10 4A 10 "
     "0F 10 52 20"},

    {Target::HitResultPick, L"HitResultPick", L"decide what the crosshair is on (FastBlockPlacement re-pick)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 28 06 00 00 48 8D AC 24 80 00 00 00 44 "
     "0F 29 8D 90 05 00 00 44 0F 29 85 80 05 00 00 0F 29 BD 70 05 00 00"},

    {Target::BeRenderLoop, L"block entity render loop",
     L"iterate the block entities to draw this frame",
     "41 57 41 56 41 55 41 54 56 57 55 53 48 81 EC 98 00 00 00 44 89 C3 48 89 D6 48 89 CF "
     "E8 ? ? ? ? 49 89 C6 E8 ? ? ? ? 49 81 FE 00 36 6E 01"},

    {Target::LevelBuildDispatch, L"level build dispatch",
     L"walk the pending chunk queue and schedule builds",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 38 01 00 00 48 8D AC 24 80 00 00 00 44 "
     "0F 29 B5 A0 00 00 00 44 0F 29 AD 90 00 00 00 44 0F 29 A5 80 00 00 00"},

    {Target::BlockTransform, L"block transform (rotate / mirror)",
     L"rotate a block's states the way structures do",
     "55 41 57 41 56 41 54 56 57 53 48 81 EC A0 00 00 00 48 8D AC 24 80 00 00 00 48 C7 45 "
     "18 FE FF FF FF 89 D3 44 89 C7 48 89 CE"},

    {Target::NameTagStage, L"name tag stage (LevelRendererPlayer vfunc 12)",
     L"draw the name tags of the frame",
     "41 57 41 56 41 54 56 57 53 48 83 EC 28 4D 8B B8 C8 32 00 00 4D 8B A0 D0 32 00 00 4D "
     "39 E7 74 ?"},

    {Target::BlockOutlineDraw, L"block outline draw",
     L"draw the highlight around the block you are looking at",
     "55 41 57 41 56 41 54 56 57 53 48 81 EC 20 02 00 00 48 8D AC 24 80 00 00 00 44 0F 29 "
     "85 90 01 00 00 0F 29 BD 80 01 00 00 0F 29 B5 70 01 00 00"},

    {Target::FogSettingsFetch, L"fog settings fetch",
     L"copy the current fog distance settings",
     "48 89 D0 8B 49 08 48 83 F9 05 77 ? 48 8D 15 ? ? ? ? 48 63 0C 8A 48 "
     "01 D1 FF ? 49 83 C0 30 41 80 78 48 01 74 ? EB ? 49 81 C0 60 01 00 00 "
     "41 80 78 48 01 74 ? EB ? 49 81 C0 C8 00 00 00 41 80 78 48 01 74 ? EB "
     "? 49 81 C0 14 01 00 00 41 80 78 48 01 74 ? EB ? 49 83 C0 7C 41 80 78 "
     "48 01 74 ? EB ? 49 81 C0 AC 01 00 00 41 80 78 48 01 75 ? 41 0F 10 00 "
     "41 0F 10 48 0C 0F 11 48 0C 0F 11 00 B1 01 88 48 1C C3 4C 8D 05 ? ? ? "
     "? 41 80 78 48 01 74 ? 31 C9 88 48 1C C3 CC CC"},

    {Target::FogColorFetch, L"fog color fetch", L"sample the current fog RGB curves",
     "56 57 53 48 83 EC 50 44 0F 29 44 24 40 0F 29 7C 24 30 0F 29 74 24 20 48 89 D0 "
     "8B 51 08 48 83 FA 04 77 ? 4C 8D 0D ? ? ? ? 49 63 14 91 4C 01 CA FF ? 49 81 C0 F8 01 00 00"},

    {Target::FogDistanceClamp, L"fog distance clamp",
     L"clamp of the fog start/end handed to the shaders (NoRender fog)",
     "C7 86 30 04 00 00 00 00 80 3F F3 0F 10 96 34 04 00 00 F3 0F 5D 96 10 04 00 00 F3 0F "
     "11 96 34 04 00 00 F3 0F 10 96 0C 04 00 00 F3 0F 10 9E 38 04 00 00 0F 28 E2 F3 0F C2 "
     "E3 01 F3 0F 5F D9 0F 54 D4 0F 55 E3 0F 56 E2 F3 0F 11 A6 38 04 00 00 48 8B 75 28 0F "
     "11 45 20 48 85 F6 74 ? F0 FF 4E 08"},

    {Target::ItemRegistryLookupByName, L"ItemRegistry::lookupByName",
     L"look up an item by its name (stage CQ-9)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 18 01 00 00 48 8D AC 24 80 00 00 00 48 "
     "C7 85 90 00 00 00 FE FF FF FF 48 89 D6 49 83 79 08 00 0F 84 ? ? ? ? 48 89 4D 20"},

    {Target::ItemRegistryItemListSite, L"ItemRegistry item list site",
     L"find the list of every registered item (stage GB-4)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 88 00 00 00 48 8D AC 24 80 00 00 00 0F "
     "29 75 F0 48 C7 45 E8 FE FF FF FF 48 89 CE 48 8B 79 38 48 8B 59 40 48 39 DF"},

    {Target::NameTagStageCaller, L"name tag stage caller",
     L"render schematics and apply NoRender at the frame stage host",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 78 0C 00 00 48 8D AC 24 80 00 00 00 44 "
     "0F 29 85 E0 0B 00 00 0F 29 BD D0 0B 00 00 0F 29 B5 C0 0B 00 00"},

    {Target::TessellatorBegin, L"Tessellator::begin", L"start a tessellated mesh",
     "56 57 55 53 48 83 EC 38 48 8B 05 ? ? ? ? 48 31 E0 48 89 44 24 30 80 B9 A0 02 00 00 "
     "00 0F 85 ? ? ? ? 48 89 CE"},
    {Target::TessellatorVertex, L"Tessellator::vertex", L"add one vertex",
     "41 57 41 56 41 55 41 54 56 57 55 53 48 83 EC 78 44 0F 29 4C 24 60 44 0F 29 44 24 50 "
     "0F 29 7C 24 40 0F 29 74 24 30 8B 81 98 02 00 00 3B 81 9C 02 00 00 0F 84 ? ? ? ?"},
    {Target::TessellatorColor, L"Tessellator::color", L"set the vertex color",
     "80 B9 54 02 00 00 00 0F 85 ? ? ? ? F3 0F 10 05 ? ? ? ? F3 0F 59 C8 F3 0F 2C C1 45 31 "
     "C0 85 C0 41 0F 4E C0 3D FF 00 00 00"},

    {Target::RenderMeshImmediately, L"MeshHelpers::renderMeshImmediately",
     L"draw a tessellated mesh right away",
     "55 41 57 41 56 41 54 56 57 53 48 81 EC 50 04 00 00 48 8D AC 24 80 00 00 00 48 C7 85 "
     "C8 03 00 00 FE FF FF FF 80 BA 55 02 00 00 00 0F 85 ? ? ? ? 4C 89 CF 48 89 D6 48 "
     "89 CB 4D 89 C6 E8 ? ? ? ? 48 8D 0D ? ? ? ? 48 89 8D B8 03 00 00 48 C7 85 C0 "
     "03 00 00 15 00 00 00 66 89 44 24 20 4C 8D BD 18 01 00 00 4C 8D 8D B8 03 00 00 48 89 "
     "F1 4C 89 FA 41 B8 02 00 00 00 E8 ? ? ? ? 48 83 C3 10 C6 85 B0 03 00 00 00 4C 8D "
     "65 D0 41 B8 48 01 00 00 4C 89 E1 31 D2 E8 ? ? ? ? 90 48 8D 85 78 03 00 00 48 89 "
     "44 24 40 48 89 7C 24 30 4C 89 F9 48 89 DA 4D 89 F0 4D 89 E1 E8 ? ? ? ? 90 48 8D "
     "8D 18 01 00 00 E8 ? ? ? ? C7 86 98 02 00 00 00 00 00 00 C6 86 55 02 00 00 00 C6 "
     "86 A0 02 00 00 00 C6 86 CA 01 00 00 00 48 8D 4E 08 E8 ? ? ? ? C6 06 00 48 C7 86 "
     "70 01 00 00 00 00 00 00"},

    {Target::TessellatorEnd, L"Tessellator::end", L"turn the tessellated vertices into a mesh",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 88 03 00 00 48 8D AC 24 80 00 00 00 44 "
     "0F 29 AD F0 02 00 00 66 44 0F 7F A5 E0 02 00 00 44 0F 29 9D D0 02 00 00"},
    {Target::TessellatorClear, L"Tessellator buffers clear", L"empty the tessellator after end",
     "C6 01 00 48 8B 41 08 48 3B 41 10 74 04 48 89 41 10 48 8B 41 20 48 3B 41 28 74 04 48 89 41 "
     "28 48 8B 41 38 48 3B 41 40 74 04 48 89 41 40 48 8B 41 68"},
    {Target::MeshRender, L"mce::Mesh::render", L"draw a kept mesh",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 38 04 00 00 48 8D AC 24 80 00 00 00 0F "
     "29 B5 A0 03 00 00 48 C7 85 98 03 00 00 FE FF FF FF 4D 89 CC 4D 89 C6 48 89 D3"},

    {Target::MeshDestroy, L"mce::Mesh::~Mesh", L"free a kept mesh",
     "56 57 48 83 EC 28 48 89 CE E8 ? ? ? ? 48 8B 8E 48 02 00 00 48 85 C9 74 ? 48 8B "
     "96 58 02 00 00 48 29 CA 48 81 FA 00 10 00 00 72 ? 48 8B 41 F8"},

    {Target::MaterialPtrCtorSite, L"MaterialPtr construction site",
     L"make a material from its name",
     "48 B8 6C 4E 34 63 95 6C 13 B1 48 89 85 E0 02 00 00 48 8D 15 ? ? ? ? 4C 8D 85 E0 "
     "02 00 00 4C 89 F1 E8 ? ? ? ? 90 48 8B 85 00 03 00 00 48 83 F8 10"},

    {Target::OpenInventoryScreen, L"openInventoryScreen",
     L"open the player inventory on the client (FastInventory)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 88 01 00 00 48 8D AC 24 80 00 00 00 48 "
     "C7 85 00 01 00 00 FE FF FF FF 48 89 CF 4C 8D 35 ? ? ? ? 4C 89 F1 E8 ? ? ? ?"},

    {Target::InputGather, L"input gather",
     L"builds the held-input bits and move amounts (FreeCamera takes them)",
     "0F B7 41 02 84 C0 0F 88 ? ? ? ? A9 00 01 00 00 0F 85 ? ? ? ? A9 "
     "00 02 00 00 0F 85 ? ? ? ? A9 00 04 00 00 74 ? 45 31 C0 A9 00 01 00 "
     "00 41 0F 94 C0 45 31 C9"},

    {Target::InputGatherKeyCallSite, L"input gather key call site",
     L"the call that passes the raw key bits to InputGather (ToggleSneakSprint edits only these)",
     "0F 10 45 00 0F 29 44 24 70 48 8D 54 24 70 4C 89 F9 E8 ? ? ? ? 44 8B "
     "7C 24 70 48 8B 94 24 40 01 00 00 44 89 3A"},

    {Target::MoveIntentFromInput, L"move intent from input",
     L"turns the held keys into a move amount (FreeCamera zeroes it)",
     "48 83 EC 18 0F 29 34 24 48 89 C8 F2 0F 10 4A 04 0F 57 C0 0F 2E C8 0F 85 "
     "? ? ? ? 0F 8A ? ? ? ? 0F 28 D1 0F C6 D1 55 0F 2E D0 0F 85 ? ? "
     "? ? 0F 8A ? ? ? ? 66 0F 6E 02 66 0F 70 D8 00"},

    {Target::GetActorEffect, L"Actor::getEffect", L"look up a mob effect on an actor (Fullbright)",
     "48 83 EC 28 4C 8B 41 10 8B 41 18 49 8B 48 48 4D 8B 48 50 49 29 C9 49 C1 E9 03 "
     "41 FF C9 41 81 E1 50 B5 A1 E6 4E 8D 14 C9 49 8B 48 68"},

    {Target::FogColorDistanceClamp, L"fog color distance clamp",
     L"second clamp of the fog start/end inside the fog color update (NoRender fog)",
     "C7 86 30 04 00 00 00 00 80 3F F3 0F 10 96 34 04 00 00 F3 0F 5D 96 10 04 00 00 F3 0F "
     "11 96 34 04 00 00 F3 0F 10 96 0C 04 00 00 F3 0F 10 9E 38 04 00 00 0F 28 E2 F3 0F C2 "
     "E3 01 F3 0F 5F D9 0F 28 CC 0F 55 CB 0F 54 E2 0F 56 E1 F3 0F 11 A6 38 04 00 00 0F 11 "
     "45 D0 48 85 FF 74 ? F0 FF 4F 08 75 ?"},

    {Target::FogColorNightVisionSite, L"fog color night vision site",
     L"getEffect(night vision) calls inside the fog color update (Fullbright leaves the sky alone)",
     "48 89 F9 BA 10 00 00 00 E8 ? ? ? ? 48 85 C0 0F 84 ? ? ? ? 48 89 "
     "F9 BA 10 00 00 00 E8 ? ? ? ? 48 89 C3 48 89 F9 BA 1A 00 00 00 E8 ? "
     "? ? ? 0F 57 C9 48 89 D9 48 09 C1 0F 84 ? ? ? ? 48 85 DB 74 ?"},

    {Target::ContainerSmHandle, L"containerSmHandle", L"container screen state machine (ItemScroller)",
     "41 57 41 56 41 55 41 54 56 57 55 53 48 83 EC 48 4D 89 CC 44 89 C5 89 D7 48 89 CB 48 "
     "8B 05 ? ? ? ?"},

    {Target::ContainerSmOffsetSite, L"containerSmOffsetSite", L"where the state machine sits in the screen controller",
     "56 48 83 EC 30 48 8B 44 24 60 48 8B 71 08 8B 12 45 8B 00 8B 00 48 8D 8E ? ? ? ? "
     "89 44 24 20 E8"},

    {Target::ContainerMcOffsetSite, L"containerMcOffsetSite", L"where the container manager sits in the screen controller",
     "48 8B 8B ? ? ? ? 48 8B 01 48 8B 40 40 4C 89 FA FF 15"},

    {Target::ContainerGetItem, L"containerGetItem", L"read one slot of a screen collection",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 83 EC 38 48 8D 6C 24 30 48 C7 45 00 FE FF FF "
     "FF 44 89 C6 48 89 D3 4C 8B 72 10 48 83 7A 18 10"},

    {Target::BlockAsItemInstance, L"blockAsItemInstance", L"pick item virtual slot",
     "55 41 57 41 56 41 54 56 57 53 48 83 EC 40 48 8D 6C 24 40 48 C7 45 F8 FE FF FF FF 4C "
     "89 CF 4C 89 C6 49 89 D4 48 89 CB 44 0F B6 7D 60 4C 8B 71 68 49 8B 00 48 8B 80 40 01 "
     "00 00 4C 89 C1 4C 89 CA FF 15 ? ? ? ? 48 85 C0 74 ? 0F B6 4F 08 80 E1 0F 0F B7 "
     "57 04 66 2B 56 3A 44 0F B6 07 41 80 E0 0F 44 88 45 F4 88 4D F5 66 89 55 F6 48 8D 55 "
     "F4 48 89 C1 E8 ? ? ? ? 49 89 C1 EB ? 45 31 C9 49 8B 06 48 8B 80 70 02 00 00"},

    {Target::BlockGetPlacementBlock, L"blockGetPlacementBlock", L"placement block base virtual function",
     "41 57 41 56 41 55 41 54 56 57 53 48 83 EC 50 44 89 CB 4C 89 C6 48 89 D7 49 89 CE 0F "
     "B7 94 24 B8 00 00 00 48 8B 05 ? ? ? ? 48 31 E0 48 89 44 24 48 E8 ? ? ? ? 49 89 C0 48 "
     "85 C0 75 ? 4D 8B 86 40 02 00 00 4D 85 C0 0F 84 ? ? ? ? 4D 8B BE 48 02 00 00 4D 8B A6 "
     "50 02 00 00 4D 39 E7 74 ? 4C 8B AC 24 B0 00 00 00 4C 8D 74 24 40 90 4D 85 C0 74 ? 49 "
     "8B 0F 48 8B 01"},

    {Target::BlockUseListenerSite, L"blockUseListenerSite", L"where Block::use looks up the use handler",
     "B9 ? ? ? ? 48 03 4B 68 B2 04 E8 ? ? ? ? 48 89 C3 48 85 C0"},

    {Target::BlockUse, L"blockUse", L"ask the game whether a click uses the block",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 08 02 00 00 48 8D AC 24 80 00 00 00 48 "
     "C7 85 80 01 00 00 FE FF FF FF 45 89 CE 4C 89 C7 48 89 D6"},

    {Target::ItemStackBaseDtor, L"itemStackBaseDtor", L"destroy picked item without deleting storage",
     "56 57 48 83 EC 28 48 89 CE 48 8D 05 ? ? ? ? 48 89 01 48 8B 49 78 48 85 C9 74 ? "
     "48 8B 01 48 8B 00 BA 01 00 00 00 FF 15"},

    {Target::ItemStackIsNull, L"itemStackIsNull", L"is the item stack empty",
     "41 56 56 57 53 48 83 EC 28 B0 01 80 79 23 01 0F 85 ? ? ? ? 48 8B 51 08 48 85 D2 0F 84 ? ? ? ? 48 8B 32 48 85 F6 0F 84 ? ? ? ? 48 8B 96 20 01 00 00 48 3B 15 ? ? ? ? 0F 85 ? ? ? ?"},

    {Target::ItemStackMaxStackSize, L"itemStackMaxStackSize", L"max stack size of an item stack",
     "55 56 57 53 48 83 EC 38 48 8D 6C 24 30 48 C7 45 00 FE FF FF FF 48 8B 51 08 B0 FF 48 85 D2 74"},

    {Target::ItemStackMatches, L"itemStackMatches", L"compare two item stacks",
     "56 57 53 48 83 EC 20 48 89 CE 48 8B 49 08 48 85 C9 74 ? 48 8B 09 48 85 C9 74 ? "
     "4C 8B 4A 08 4D 85 C9 74 ? 49 39 09 74"},

    {Target::ItemStackMatchesWrapper, L"itemStackMatchesWrapper", L"item stack comparison flags",
     "56 57 48 83 EC 28 48 89 D6 48 89 CF 4C 8D 05 ? ? ? ? E8 ? ? ? ? 84 C0 74 ? 66 81 7F 20 FF 7F"},

    {Target::ContainerScreenDtor, L"containerScreenDtor", L"container screen controller destructor",
     "55 41 56 56 57 53 48 83 EC 30 48 8D 6C 24 30 48 C7 45 F8 FE FF FF FF 48 89 CE 48 8D "
     "05 ? ? ? ? 48 89 01 48 8D 05 ? ? ? ? 48 89 81 C8 0B 00 00 48 8B 81 48 0D 00 00"},

    {Target::ContainerScreenTick, L"containerScreenTick", L"container screen controller tick",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC C8 00 00 00 48 8D AC 24 80 00 00 00 0F 29 75 30 "
     "48 C7 45 28 FE FF FF FF 48 89 CE 48 8D B9 ? ? 00 00 C6 45 F8 00 48 81 C1 ? ? 00 00 E8"},

    {Target::ContainerScreenCtor, L"containerScreenCtor", L"container screen controller constructor",
     "55 41 57 41 56 41 55 41 54 56 57 53 B8 D8 2D 00 00 E8 ? ? ? ? 48 29 C4 48 8D AC 24 "
     "80 00 00 00 0F 29 B5 40 2D 00 00"},

    {Target::Trade2Ctor, L"trade2Ctor", L"trade screen controller constructor",
     "55 41 57 41 56 41 55 41 54 56 57 53 B8 68 11 00 00 E8 ? ? ? ? 48 29 C4 48 8D AC 24 "
     "80 00 00 00 48 C7 85 E0 10 00 00 FE FF FF FF"},

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
     "55 56 57 48 83 EC 40 48 8D 6C 24 40 48 C7 45 F8 FE FF FF FF 48 89 D7 48 89 CE 48 83 "
     "C7 08 48 8D 4D E8 31 D2"},

    {Target::TradeSelTier, L"tradeSelTier", L"tier index of a trade row selection",
     "55 56 48 83 EC 58 48 8D 6C 24 50 48 C7 45 00 FE FF FF FF 48 89 CE 80 79 20 00 0F 85 "
     "? ? ? ? 48 89 F1 E8 ? ? ? ? 84 C0 0F 84 ? ? ? ? 48 C7 45 E8 00 00 00 00 "
     "48 C7 45 F0 0B 00 00 00 48 C7 45 F8 0F 00 00 00 48 B8 74 72 61 64 65 5F 74 69 48 89 "
     "45 E0 C7 45 E7 69 65 72 73 48 8D 55 E0 48 89 F1 E8 ? ? ? ? 90 48 89 C1 31 D2 E8 "
     "? ? ? ? 90 89 46 1C C6 46 20 01 48 8B 45 F8 48 83 F8 0F 76 ? 48 8B 4D E0 48 8D "
     "50 01 48 81 FA 00 10 00 00 72 ? 4C 8B 41 F8 48 83 C1 F8 4C 29 C1 48 83 F9 20 73 ? "
     "48 83 C0 28 48 89 C2 4C 89 C1 E8 ? ? ? ? 80 7E 20 00 75 ? E8 ? ? ? ? C7 46 "
     "1C 00 00 00 00 C6 46 20 01 8B 46 1C 48 83 C4 58 5E"},

    {Target::TradeSelIndex, L"tradeSelIndex", L"trade index of a trade row selection",
     "55 56 48 83 EC 58 48 8D 6C 24 50 48 C7 45 00 FE FF FF FF 48 89 CE 80 79 28 00 0F 85 "
     "? ? ? ? 48 89 F1 E8 ? ? ? ? 84 C0 0F 84 ? ? ? ? 48 C7 45 F0 06 00 00 00 "
     "48 C7 45 F8 0F 00 00 00 48 C7 45 E6 00 00 00 00 66 C7 45 EE 00 00 8B 05 ? ? ? ? "
     "89 45 E0 0F B7 05 ? ? ? ? 66 89 45 E4 48 8D 55 E0 48 89 F1 E8 ? ? ? ? 90 48 "
     "89 C1 31 D2 E8 ? ? ? ? 90 89 46 24 C6 46 28 01 48 8B 45 F8 48 83 F8 0F 76 ? 48 "
     "8B 4D E0 48 8D 50 01 48 81 FA 00 10 00 00 72 ? 4C 8B 41 F8 48 83 C1 F8 4C 29 C1 48 "
     "83 F9 20 73 ? 48 83 C0 28 48 89 C2 4C 89 C1 E8 ? ? ? ? 80 7E 28 00 75 ? E8 ? "
     "? ? ? C7 46 24 00 00 00 00 C6 46 28 01 8B 46 24 48 83 C4 58 5E"},

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

    {Target::ItemStorageComponentSite, L"itemStorageComponentSite", L"does an item have the storage_item component",
     "48 8B 46 50 48 85 C0 0F 84 ? ? ? ? 48 8B 08 48 85 C9 0F 84 ? ? ? ? 48 8B 01 48 8B 40 ? FF 15 ? ? ? ? 84 C0 0F 84 ? ? ? ? "
     "48 8B 46 50 48 85 C0 0F 84 ? ? ? ? 48 8B 08 48 85 C9 0F 84 ? ? ? ? 48 8B 01 48 8B 80 ? ? ? ? 48 8D 15 ? ? ? ? FF 15 ? ? ? ? 48 85 C0"},

    {Target::TradeCurrentTier, L"tradeCurrentTier", L"the trader's current tier",
     "56 57 53 48 83 EC 20 48 89 CE 48 8B 49 30 48 8B 91 F8 01 00 00 8B 42 08 0F 1F 84 00 "
     "00 00 00 00 44 8D 40 01 F0 44 0F B1 42 08 75 ? 48 8B B9 F0 01 00 00 48 8B 99 F8 01 "
     "00 00 48 85 DB 74 ? F0 FF 4B 08 75 ? 48 8B 03 48 8B 00 48 89 D9 FF 15 ? ? ? ? F0 FF "
     "4B 0C 75 ? 48 8B 03 48 8B 40 08 48 89 D9 FF 15 ? ? ? ? 48 8B 07 48 8B 40 68 48 89 F9 "
     "FF 15 ? ? ? ? 48 8B 08 48 8B 91 58 01 00 00 48 89 C1 48 89 D0 FF 15 ? ? ? ? 48 8B 96 "
     "98 01 00 00 48 8B 08 4C 8B 89 F0 01 00 00 48 89 C1 45 31 C0 4C 89 C8 FF 15 ? ? ? ? "
     "48 85 C0 0F 84 ? ? ? ?"},

    {Target::TradePossible, L"tradePossible", L"whether the player can pay for a trade",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 98 00 00 00 48 8D AC 24 80 00 00 00 0F "
     "29 75 00 48 C7 45 F8 FE FF FF FF 48 89 D6 48 89 CF 0F 57 C0 0F 29 45 C0 48 8B 89 C0 "
     "00 00 00 48 85 C9"},

    {Target::ScenePopCall, L"scenePopCall", L"pop the top screen from the scene stack (call site)",
     "48 8B 4D F0 48 8B 01 48 8B 80 D0 00 00 00 BA 01 00 00 00 FF 15 ? ? ? "
     "? 90 0F 57 C0 48 8B 7D E8"},

    {Target::ItemCreativeCategoryStore, L"itemCreativeCategoryStore", L"store of an item's creative category",
     "48 8D 05 ? ? ? ? 48 89 85 90 00 00 00 48 C7 85 98 00 00 00 11 00 00 00 4C 89 F1 "
     "48 89 FA E8 ? ? ? ? 48 85 C0 74 ? 0F B6 40 08 EB ? 31 C0 41 88 84 24 80 01 00 "
     "00 48 8D 05 ? ? ? ? 48 89 85 90 00 00 00 48 C7 85 98 00 00 00 0E 00 00 00"},

    {Target::SceneStackPush, L"sceneStackPush", L"push a screen onto the scene stack",
     "55 56 48 83 EC 78 48 8D 6C 24 70 48 C7 45 00 FE FF FF FF C6 45 D8 00 0F 57 C0 0F 29 45 E0 "
     "48 8B 42 08 48 85 C0 74 ? F0 FF 40 08 48 8B 42 08 EB ? 31 C0 48 89 55 F8 48 8B 12 48 89 55 E0 "
     "48 89 45 E8 48 8D 55 E0 4C 8D 4D B0 E8"},

    {Target::OpenTradingScreenPush, L"openTradingScreenPush", L"push of the trade screen inside openTrading",
     "48 8B 5D D8 48 8B 8F 98 0D 00 00 48 8B 01 48 8B 80 18 07 00 00 FF 15 ? "
     "? ? ? 90 48 8D 55 A8 48 89 C1 49 89 F8 49 89 F1 E8 ? ? ? ? 90 48 "
     "8B 03 48 8B 80 C8 00 00 00 48 8D 55 A8 48 89 D9 45 31 C0 FF 15 ? ? ? "
     "? 90 E9 ? ? ? ? 48 8B 8F 98 0D 00 00"},

    {Target::LegacyParticleRender, L"legacyParticleRender", L"submit the legacy particles (NoRender Particle)",
     "55 41 56 56 57 53 48 81 EC 80 00 00 00 48 8D AC 24 80 00 00 00 48 C7 45 F8 FE FF FF FF 4C 89 C6 "
     "49 89 D6 48 89 CF 48 8B 45 50 F3 0F 10 45 58 F3 0F 59 C0 4C 89 45 B0 49 8B 88 ? ? 00 00"},

    {Target::ContainerCloseGetId, L"ContainerClose::getId",
     L"build the inventory-close packet without a copy (HandRestock)", "B8 2F 00 00 00 C3 CC CC"},

    {Target::ShulkerContentsText, L"shulkerContentsText", L"the contents list in a shulker box tooltip (ShulkerPreview)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC A8 01 00 00 48 8D AC 24 80 00 00 00 0F 29 B5 10 01 00 00 48 C7 85 08 01 00 00 FE FF FF FF 0F 57 C0 0F 11 01 48 C7 41 10 00 00 00 00 48 89 8D 00 01 00 00 48 C7 41 18 0F 00 00 00 48 85 D2 0F 84 ? ? ? ? 48 89 D6 48 8D 3D ? ? ? ? 48 89 7D C0 48 C7 45 C8 05 00 00 00 48 8D 55 C0 48 89 F1 E8 ? ? ? ? 90 48 85 C0 0F 84 ? ? ? ? 48 89 7D C0 48 C7 45 C8 05 00 00 00 48 8D 55 C0 48 89 F1 E8 ? ? ? ? 90 48 89 C7 48 85 C0 0F 84 ? ? ? ? 45 31 E4 48 8D 1D ? ? ? ? 0F 57 F6 4C 8D B5 C0 00 00 00 31 F6 EB ? 0F 1F 40 00 41 FF C4 FF C6 48 8B 47 08 48 8B 4F 10 48 29 C1 48 C1 E9 03 39 CE 0F 8D ? ? ? ? 83 FE 04 77 ? 89 F1 4C 8B 3C C8 49 8B 07 48 8B 40 28 4C 89 F9 FF 15 ? ? ? ? 90 3C 0A 75 ? 48 8D 4D C0 E8 ? ? ? ? 90 48 89 5D C0 C7 45 40 00 00 00 00 C6 45 50 00 48 8D 4D C0 4C 89 FA E8 ? ? ? ? 90 48 8B 45 C8 48 85 C0 74 ? 48 8B 08 48 85 C9 74 ? 48 8B 01 48 8B 80 58 03 00 00 48 8D 55 C0 FF 15 ? ? ? ?"},

    {Target::ItemHoverTextBuild, L"itemHoverTextBuild", L"build the whole item tooltip text (DebugKeys F3+H)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 78 01 00 00 48 8D AC 24 80 00 00 00 0F "
     "29 B5 E0 00 00 00 48 C7 85 D8 00 00 00 FE FF FF FF 44 88 CE 4C 89 45 E8 48 89 D0"},

    {Target::HoverRendererRender, L"HoverRendererRender", L"draw the item tooltip box (AppleSkin)",
     "55 56 57 53 48 81 EC 78 01 00 00 48 8D AC 24 80 00 00 00 44 0F 29 8D E0 00 00 00 44 "
     "0F 29 85 D0 00 00 00 0F 29 BD C0 00 00 00 0F 29 B5 B0 00 00 00 48 C7 85 A8 00 00 00 "
     "FE FF FF FF 48 83 79 20 00 0F 84 ? ? ? ? 48 89 CF"},

    {Target::HoverBoxSizeStore, L"HoverBoxSizeStore", L"the tooltip box size fields (ShulkerPreview)",
     "0F 13 76 60 48 8D 4D C0 E8 ? ? ? ? 48 8B 85 30 04 00 00 48 83 F8 10 72 ?"},

    {Target::UiDrawItem, L"UiDrawItem", L"draw one item inside a UI renderer (ShulkerPreview)",
     "55 41 57 41 56 41 54 56 57 53 48 81 EC 60 01 00 00 48 8D AC 24 80 00 00 00 44 0F 29 85 D0 00 00 00 "
     "0F 29 BD C0 00 00 00 0F 29 B5 B0 00 00 00 48 C7 85 A8 00 00 00 FE FF FF FF 49 8B 40 08 48 85 C0 "
     "0F 84 ? ? ? ? 48 8B 18 48 85 DB 0F 84 ? ? ? ? 0F 28 F3 4C 89 C7 48 89 CE 44 8B A5 50 01 00 00 "
     "F3 44 0F 10 85 48 01 00 00 F3 0F"},

    {Target::ShulkerHoverAppend, L"ShulkerHoverAppend", L"the shulker box tooltip text builder (ShulkerPreview)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 83 EC 78 48 8D 6C 24 70 48 C7 45 00 FE FF FF FF 4C 89 CE 48 89 D7 0F B6 45 70 88 44 24 20 E8 ? ? ? ? 48 8B 57 10 48 8D 7D B8 48 89 F9 E8 ? ? ? ?"},

    {Target::ItemStackHoverName, L"ItemStackHoverName", L"the display name of an item stack (ShulkerPreview)",
     "55 41 57 41 56 41 54 56 57 53 48 83 EC 70 48 8D 6C 24 70 48 C7 45 F8 FE FF FF FF 48 "
     "89 D6 48 8D 55 B0 E8 ? ? ? ? 0F B6 45 F0"},

    {Target::ItemMaxDamageSlotSite, L"ItemMaxDamageSlotSite", L"Item::getMaxDamage vtable slot (ArmorHUD)",
     "48 8B 01 48 8B 80 20 01 00 00 FF 15 ? ? ? ? 98 0F 57 C0 F3 0F 2A C0 F3 0F 11 85 A0 03 00 00"},

    {Target::ItemStackDamageValue, L"ItemStackDamageValue", L"ItemStackBase::getDamageValue (ArmorHUD)",
     "56 57 48 83 EC 38 48 8B 05 ? ? ? ? 48 31 E0 48 89 44 24 30 48 8B 41 08 48 85 C0 74 ? "
     "48 83 38 00 74 ? 48 8B 71 10 48 85 F6 74 ? 48 8D 3D ? ? ? ? 48 89 7C 24 20 "
     "48 C7 44 24 28 06 00 00 00"},

    {Target::ShaderColorFillSite, L"ShaderColorFillSite", L"the UI shader color written by fillRectangle (ShulkerPreview)",
     "48 8B 4A 30 0F 10 00 0F 11 01 C6 41 10 01"},

    {Target::GlintTintSite, L"GlintTintSite", L"where the glint command reads the UI shader color (ShulkerPreview)",
     "49 8B 4E 20 49 8B 56 30 48 8B 89 50 01 00 00 48 8B 49 40 F3 0F 10 01 49 83 C6 60 C6 "
     "85 98 03 00 00 00 48 8D 8D 80 03 00 00"},

    {Target::AttackCore, L"AttackCore", L"GameMode::attack body (AutoTool)",
     "55 41 57 41 56 41 54 56 57 53 48 81 EC F0 01 00 00 48 8D AC 24 80 00 00 00 48 C7 85 "
     "68 01 00 00 FE FF FF FF 4C 89 CB 45 89 C6 49 89 D7"},

    {Target::AttackDamageCalc, L"AttackDamageCalc", L"melee damage of the selected item (AutoTool)",
     "41 56 56 57 53 48 83 EC 68 44 0F 29 44 24 50 0F 29 7C 24 40 0F 29 74 24 30 4C 89 C7 48 89 D3 48 89 CE "
     "48 8B 05 ? ? ? ? 48 31 E0 48 89 44 24 28 F3 41 0F 10 38 41 80 78 04 01"},

    {Target::TargetCategorySite, L"TargetCategorySite", L"actor category bits field (AutoTool)",
     "80 7F 06 01 75 ? F6 83 ? ? ? ? 02 74 ? 48 89 D9 48 89 F2 E8"},

    {Target::AnnouncedSlotSite, L"AnnouncedSlotSite", L"the hotbar slot last announced to the server (AutoTool)",
     "48 8B 86 E0 05 00 00 44 0F B6 B0 B0 00 00 00 8B 58 10 0F B6 86 72 0E 00 00 41 3A 47 22 75 ? 48 "
     "8D 8E 50 0E 00 00 4C 89 FA E8 ? ? ? ? 84 C0 74 ? 39 9E E8 0E 00 00 0F 85 ? ? ? ?"},

    {Target::CompoundTagHash, L"CompoundTagHash", L"CompoundTag::hash (ShulkerPreview)",
     "41 57 41 56 41 55 41 54 56 57 55 53 48 83 EC 28 48 8B 79 08 4C 8B 27 49 39 FC 0F 84 ? ? ? ? "
     "48 BB 25 23 22 84 E4 9C F2 CB 49 BE B3 01 00 00 00 01 00 00 41 BF B9 79 37 9E 31 F6 E9"},

    {Target::ServerPlayerVtableRef, L"serverPlayerVtableRef", L"lea of the ServerPlayer vtable (GameData)",
     "48 8D 0D ? ? ? ? 49 89 0C 24 41 89 84 24 E0 0C 00 00 49 8D 84 24 E8 0C 00 00 48 89 "
     "45 00 49 C7 84 24 F8 0C 00 00 05 00 00 00"},

    {Target::HudScreenCtor, L"hudScreenCtor", L"HUD screen controller constructor (InventoryHUD)",
     "55 41 57 41 56 41 55 41 54 56 57 53 B8 B8 13 00 00 E8 ? ? ? ? 48 29 C4 48 8D AC 24 "
     "80 00 00 00 0F 29 B5 20 13 00 00"},

    {Target::HudGetItemSite, L"hudGetItemSite", L"where the HUD binding resolver reads its container manager (InventoryHUD)",
     "49 8B 8E 18 0E 00 00 4C 89 FA 41 89 D8 E8 ? ? ? ? 49 89 C6 48 8B 00 48 8B 40 28 "
     "4C 89 F1 FF 15 ? ? ? ? BB FF FF FF FF 84 C0 0F 84 ? ? ? ? 49 8B 46 08"},

    {Target::RenderCurrentFrame, L"renderCurrentFrame", L"start of one rendered frame (DebugScreen fps)",
     "55 41 57 41 56 41 55 41 54 56 57 53 B8 78 35 00 00 E8 ? ? ? ? 48 29 C4 48 8D AC 24 "
     "80 00 00 00 44 0F 29 85 E0 34 00 00"},

    {Target::ServerLevelTick, L"serverLevelTick", L"integrated server level tick (DebugScreen tps)",
     "49 8B 4E 40 E8 ? ? ? ? 90 49 8B 4E 48 48 85 C9 74 ? 48 8B 41 10 48 "
     "83 C1 10 48 8B 40 20 FF 15 ? ? ? ? 90 49 8B 4E 50 48 85 C9 74 ? 48 "
     "8B 01 48 8B 40 20 FF 15 ? ? ? ? 90 0F 1F 44 00 00 48 89 D9 E8 ? ? "
     "? ? 48 89 D9 E8 ? ? ? ?"},

    {Target::PacketCheckSize, L"packetCheckSize", L"incoming packet size check (DebugScreen rx)",
     "56 57 55 53 48 81 EC 88 00 00 00 44 89 CD 4C 89 C3 48 89 D6 48 89 CF 4C 89 44 24 28 48 8B 01 48 "
     "8B 40 18 FF 15"},

    {Target::RenderedActorCountStore, L"renderedActorCountStore", L"store of the rendered entity count (DebugScreen E)",
     "44 01 FF 89 3D ? ? ? ? 48 8B 85 E8 01 00 00 48 8B 40 28 48 8B 70 18 48 8D 4E 40 "
     "C6 46 78 01 48 8B 46 60 48 8B 56 58 48 01 D0"},

    {Target::ClimateSampleCall, L"climateSampleCall", L"call of the climate sampler in BiomeSource3d::getBiome (DebugScreen)",
     "48 8B 0E 8B 47 08 89 45 18 48 8B 07 48 89 45 10 48 8D 55 A8 4C 8D 45 10 E8 ? ? ? ? 80 BE C0 00 00 00 01"},

    {Target::OverworldGeneratorCtor, L"overworldGeneratorCtor", L"OverworldGenerator constructor (vtable and BiomeSource field)",
     "48 8D 05 ? ? ? ? 48 89 07 48 8D 05 ? ? ? ? 48 89 47 70 48 8D 87 78 02 00 00 48 89 85 ? ? ? ? C6 87 ? ? ? ? 00 "
     "48 8D 87 ? ? ? ? 48 89 85"},

    {Target::PreliminarySurfaceLoop, L"preliminarySurfaceLoop", L"search loop of the preliminary surface level (DebugScreen PS)",
     "BA 27 00 00 00 F3 0F 10 15 ? ? ? ? F3 44 0F 10 25 ? ? ? ? 0F 57 E4 4C 8D 05 "
     "? ? ? ? F3 0F 10 2D ? ? ? ? F3 0F 10 35 ? ? ? ? F3 0F 10 3D ? ? ? ? "
     "F3 44 0F 10 05 ? ? ? ? F3 44 0F 10 0D ? ? ? ? F3 44 0F 10 15 ? ? ? ? F3 "
     "44 0F 10 1D ? ? ? ? 48 8D 1D ? ? ? ? EB ? 90"},

    {Target::PreliminarySurfaceCall, L"preliminarySurfaceCall", L"blend call inside the preliminary surface level (DebugScreen PS)",
     "4C 8B 8B ?? ?? 00 00 48 8B 83 ?? ?? 00 00 F3 44 0F 11 45 ?? F3 0F 11 45 ?? C7 45 ?? 00 00 00 00 48 89 44 24 20 "
     "48 8D 4D ?? 4C 8D 45 ?? 4C 89 F2 E8"},

    {Target::DensityGridEntry, L"densityGridEntry", L"density grid of one chunk in OverworldGenerator (DebugScreen N)",
     "55 41 57 41 56 56 57 53 48 83 EC 78 48 8D 6C 24 70 48 C7 45 00 FE FF FF FF 4C 89 C7 48 89 D6 48 89 CB 49 8B 10 "
     "4C 8B 89 ?? ?? 00 00 48 8B 81 ?? ?? 00 00 4C 8D 81 ?? ?? 00 00 48 89 44 24 20 48 8D 4D C8 E8"},

    {Target::BlenderFactoryFlags, L"blenderFactoryFlags", L"ChunkBlenderFactory construction (DebugScreen N safety flag)",
     "48 C7 40 40 00 00 00 00 40 88 78 48 48 C7 40 50 00 00 00 00 66 44 89 70 58 40 84 FF"},

    {Target::DBChunkStorageVtable, L"dbChunkStorageVtable", L"DBChunkStorage constructor (DebugScreen Chunks[S])",
     "48 8D 05 ? ? ? ? 48 8B 4D 08 48 89 01 C7 41 70 00 00 00 00 0F 57 C0 0F 11 41 78 48 "
     "8B 0D ? ? ? ?"},

    {Target::DiscardSetInsert, L"discardSetInsert", L"insert into the discarding chunk set (DebugScreen Chunks[S] unload)",
     "48 B8 FF FF FF FF FF FF FF 07 48 39 86 90 01 00 00 0F 84 ? ? ? ? 48 8D 86 88 01 00 "
     "00 48 89 45 D8 48 8B 0D ? ? ? ?"},
    {Target::NetworkSend, L"networkSend", L"outgoing packet send (DebugScreen tx)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 08 01 00 00 48 8D AC 24 80 00 00 00 48 "
     "C7 85 80 00 00 00 FE FF FF FF 45 89 CD 4C 89 C6 48 89 CB"},

    {Target::StartGameHandle, L"StartGameHandle", L"StartGame packet handler (DebugScreen server version)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 38 0F 00 00 48 8D AC 24 80 00 00 00 0F "
     "29 B5 A0 0E 00 00 48 C7 85 98 0E 00 00 FE FF FF FF 4C 89 85 90 0E 00 00 48 89 95 28 "
     "0E 00 00 48 89 CE"},

    {Target::TransferHandle, L"TransferHandle", L"Transfer packet handler (/tk rejoin)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC A8 02 00 00 48 8D AC 24 80 00 00 00 48 "
     "C7 85 20 02 00 00 FE FF FF FF 4C 89 C6 48 89 CB 41 80 78 52 00 0F 84 ? ? ? ?"},

    {Target::RegionalDifficultyCondition, L"RegionalDifficultyCondition", L"Level regional difficulty vtable slot (DebugScreen)",
     "56 57 48 83 EC 48 0F 29 7C 24 30 0F 29 74 24 20 4C 89 C6 48 89 CF 48 8B 02 48 8B 40 28 48 89 D1 "
     "FF 15 ? ? ? ? 0F 28 F0 F3 0F 10 7F 10 48 8B 4E 08 8B 96 ? ? 00 00 48 8B 01 48 8B 80 ? ? 00 00"},

    {Target::RegionalDifficultyTail, L"RegionalDifficultyTail", L"Level regional difficulty return tail (DebugScreen)",
     "F3 0F 58 DA 0F 57 D2 F3 0F 2A D6 F3 0F 59 D3 F3 0F 10 1D ? ? ? ? 0F 2E DA 77 ? 0F 2E "
     "15 ? ? ? ?"},

    {Target::LevelChunkTickCall, L"LevelChunkTickCall", L"call of LevelChunk tick (DebugScreen)",
     "48 8B 0F 48 89 4D 20 4C 89 75 28 4C 89 65 E0 48 89 4D E8 48 89 75 F0 48 "
     "89 5D 18 48 89 F2 4C 8B 45 D8 49 89 D9 E8 ? ? ? ? 90 4D 85 F6 74 ?"},

    {Target::GameButtonActionTableSite, L"gameButtonActionTableSite", L"input action table access (named hotkey rows)",
     "48 63 85 60 01 00 00 48 8D 04 40 48 8D 15 ? ? ? ? 0F 10 44 C2 08 0F "
     "29 45 B0 0F 57 C0"},

    {Target::GameButtonFindKeymap, L"gameButtonFindKeymap", L"keymapping lookup by action name (hotkeys as game buttons)",
     "55 41 56 56 57 53 48 83 EC 40 48 8D 6C 24 40 48 C7 45 F8 FE FF FF FF 48 "
     "8B 71 08 4C 8B 71 10 4C 39 F6 74 ? 48 8B 3A 48 8B 5A 08 EB ? 0F 1F 00 "
     "48 83 C6 40 4C 39 F6"},

    {Target::GameButtonBindAction, L"gameButtonBindAction", L"bind one input action to a button (hotkeys as game buttons)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 78 01 00 00 48 8D AC 24 80 "
     "00 00 00 0F 29 BD E0 00 00 00 0F 29 B5 D0 00 00 00 48 C7 85 C8 00 00 00 "
     "FE FF FF FF 4C 89 8D 98 00 00 00 4C 89 45 30 48 89 95 C0 00 00 00 48 63 "
     "85 60 01 00 00 48 8D 04 40 48 8D 15 ? ? ? ? 0F 10 44 C2 08 0F 29 45 "
     "B0 0F 57 C0 0F 29 45 40 48 8B 91 18 01 00 00 48 85 D2 74 ? 8B 42 08 66 "
     "0F 1F 84 00 00 00 00 00 85 C0 74 ? 44 8D 40 01 F0 44 0F B1 42 08 75 ? "
     "0F 10 81 10 01 00 00 48 8B 89 10 01 00 00 0F 29 45 40 EB ? 31 C9 EB ? "
     "48 8B 4D 40 48 8D 55 B0 48 89 4D 18 E8 ? ? ? ? 48 89 85 90 00 00 00 "
     "48 8B 85 90 00 00 00"},

    {Target::InputMappingFactoryDtorSite, L"inputMappingFactoryDtorSite", L"the two mapping tables of the input mapping factory (hotkey chords)",
     "48 8D 4E ? 48 8B 56 ? E8 ? ? ? ? 48 8B 4E ? BA 00 03 00 00 E8 ? ? ? ? 48 8B 4E ? 48 85 C9 74 ? 48 8B 56 ? 48 29 CA "
     "48 81 FA 00 10 00 00 72 ? 48 8B 41 F8 48 83 C1 F8 48 29 C1 48 83 F9 20 73 ? 48 83 C2 27 48 89 C1 48 8D 5E ? E8 ? ? ? ? "
     "0F 57 C0 0F 11 03 48 C7 43 10 00 00 00 00 48 8D 4E ? 48 8B 56 ? E8 ? ? ? ? 48 8B 4E ? BA 00 03 00 00"},

    {Target::InputMappingChordsField, L"inputMappingChordsField", L"chord vector field of InputMapping (hotkey chords)",
     "48 8B 45 E0 48 8D 88 ? ? ? ? 0F 11 B0 ? ? ? ? 48 C7 80 ? ? ? ? 00 00 00 00 4D 8D 48 08 49 8B 50 08 49 2B 10 48 C1 FA 06"},

    {Target::InputMappingFactoryPtrSite, L"inputMappingFactoryPtrSite", L"owner -> input mapping factory field (hotkey chords)",
     "48 8B 08 48 39 C1 75 ? 49 8B 5D ? 48 8B 43 ? 48 85 C0 0F 84 ? ? ? ? 4C 8B 73 ? 48 8B 4B ? 48 C1 E9 03 48 39 C1"},

    {Target::GameButtonRegisterDownSite, L"gameButtonRegisterDownSite", L"call of InputHandler button-down registration (hotkeys as game buttons)",
     "48 8D 95 40 19 00 00 4C 8D 85 20 09 00 00 48 89 F9 45 31 C9 E8 ? ? ? "
     "? 48 8B 7E 20 48 8D 05 ? ? ? ?"},

    {Target::GameButtonInputUpdate, L"gameButtonInputUpdate", L"client input handler update (hotkeys as game buttons)",
     "55 41 57 41 56 56 57 53 48 83 EC 38 48 8D 6C 24 30 48 C7 45 00 FE FF FF "
     "FF 4C 89 CB 4D 89 C6 48 89 D6 48 89 CF 48 8B 0D ? ? ? ? 48 8B 01 48 "
     "8B 40 10 B2 13 FF 15 ? ? ? ? 88 45 FF 48 8B 4F 20 49 8B 16 48 85 D2"},

    {Target::GameButtonRebuild, L"gameButtonRebuild", L"rebuild the input mappings (hotkeys as game buttons)",
     "55 41 56 56 57 53 48 81 EC 80 00 00 00 48 8D AC 24 80 00 00 00 48 C7 45 "
     "F8 FE FF FF FF 48 89 CE 48 8B 49 08 48 8B 01 48 8B 80 58 09 00 00 FF 15 "
     "? ? ? ? 48 89 C7"},

    {Target::GameButtonRegisterUpSite, L"gameButtonRegisterUpSite", L"call of InputHandler button-up registration (game button states)",
     "C7 85 A8 16 00 00 6D 6F 74 65 48 8D 95 A0 16 00 00 45 31 C9 E8 ? ? ? "
     "? 48 8B 7E 20 48 8D 05 ? ? ? ?"},

    {Target::GameButtonPadBind, L"gameButtonPadBind", L"controller input mapping row binding (hotkeys)",
     "41 57 41 56 41 55 41 54 56 57 55 53 48 83 EC 58 0F 29 74 24 40 4C 89 C6 "
     "48 89 D7 48 89 CB 48 8B 05 ? ? ? ? 48 31 E0 48 89 44 24 38 C7 44 24 "
     "34 00 00 80 BF C7 44 24 30 00 00 80 BF 44 89 C8 48 8D 04 40 48 8D 0D ? "
     "? ? ? 48 8D 14 C1 48 83 C2 08 48 89 F9 E8 ? ? ? ? 48 8B 68 20 4C "
     "8B 78 28 4C 39 FD 0F 84 ? ? ? ? 49 89 C6 4C 8D 64 24 2C 4C 8D 6C 24 "
     "2B 0F 57 F6 EB ? 66 90 C6 44 24 2B 00 48 8D 44 24 34"},

    {Target::PauseMenuOnFocusLostSite, L"pauseMenuOnFocusLostSite", L"read of options.pauseMenuOnFocusLost (DebugKeys F3+P)",
     "48 8B 4D F0 48 8B 01 48 8B 80 68 05 00 00 FF 15 ? ? ? ? 90 48 8B 08 "
     "4C 8B 89 D8 00 00 00 48 8D 55 20 48 89 C1 41 B8 2F 03 00 00 4C 89 C8 FF "
     "15 ? ? ? ? 90 48 8B 4D 20"},

    {Target::BoolOptionSet, L"boolOptionSet", L"BoolOption::set (DebugKeys F3+P)",
     "56 57 53 48 83 EC 30 48 8B 05 ? ? ? ? 48 31 E0 48 89 44 24 28 48 8B 41 08 48 83 B8 "
     "38 02 00 00 00 74 ? 48 8B 44 24 28"},

    {Target::GuiDataClearMessages, L"guiDataClearMessages", L"clear the vanilla chat (DebugKeys F3+D)",
     "56 57 53 48 83 EC 20 48 89 CE 48 8B B9 50 01 00 00 48 8B 99 58 01 00 00 48 39 DF 74 ? 0F 1F 00 48 89 F9 E8 ? ? ? ? 48 81 C7 10 01 00 00"},

    {Target::GuiDataFieldSite, L"guiDataFieldSite", L"ClientInstance GuiData field (DebugKeys F3+D)",
     "48 8B B9 ? ? ? ? 48 85 FF 0F 84 ? ? ? ? 0F 57 C0 0F 11 02 48 C7 42 10 00 00 00 00 48 8B 47 38 48 85 C0"},

    {Target::GamePauseCallSite, L"gamePauseCallSite", L"pause model calls (DebugKeys F3+Esc)",
     "48 8B 89 ? ? 00 00 B2 01 E8 ? ? ? ? 48 8B 8E ? ? 00 00 B2 01 E8 ? ? ? ? 48 8B 8E ? ? 00 00 E8"},

    {Target::EmoteReleased, L"emoteReleased", L"vanilla emote button release (DebugKeys F3+F4)",
     "55 41 56 56 57 53 48 83 EC 50 48 8D 6C 24 50 48 C7 45 F8 FE FF FF FF 48 89 CF E8 ? "
     "? ? ? 84 C0 0F 85 ? ? ? ? 48 8B 07 48 8B 80 F0 00 00 00 48 89 F9 FF 15 ? ? "
     "? ? 48 85 C0 0F 84 ? ? ? ? 48 89 C6 48 8B 07 48 8B 80 D8 04 00 00 48 89 F9 FF "
     "15 ? ? ? ? 84 C0 75 ? 48 8B 07 48 8B 80 A0 01 00 00 48 89 F9 FF 15 ? ? ? ? "
     "84 C0 0F 84 ? ? ? ? 48 8B 07 48 8B 80 F0 00 00 00 48 89 F9 FF 15 ? ? ? ? 48 "
     "85 C0 74 ? 48 89 C3 48 8B 00 48 8B 80 68 02 00 00 48 89 D9 FF 15 ? ? ? ? 48 89 "
     "C1 E8 ? ? ? ? 41 89 C6 48 89 D9 48 81 C1 58 07 00 00 E8 ? ? ? ? 44 08 F0 75 "
     "? 48 8B 8B C8 0A 00 00 48 8B 01 48 8B 80 80 00 00 00 FF 15 ? ? ? ? 48 8B 07 48 "
     "8B 80 B8 09 00 00 48 89 F9 FF 15 ? ? ? ? 48 89 F9 E8 ? ? ? ? 48 85 C0 74 ? "
     "0F B7 48 60 F6 C1 40 75 ? 48 C7 40 04 00 00 00 00 0F 57 C0 0F 11 40 24 8B 10 41 89 "
     "D0 41 81 E0 1F 00 E0 FF 44 89 00 C6 40 0E 00 66 C7 40 0C 00 00 84 C9 78 ? 81 E2 1E "
     "00 E0 FF 89 10 41 89 C8 81 E1 E3 FF 00 00 8B 50 10 41 89 D1 41 81 E1 1F 00 E0 FF 66 "
     "89 48 60 44 89 48 10 C6 40 1E 00 66 C7 40 1C 00 00 45 84 C0 78 ? 81 E2 1E 00 E0 FF "
     "89 50 10 48 8B 9E 58 0D 00 00 48 8D 4D E0 E8 ? ? ? ? 48 B8 DB 34 B6 D7 82 DE 1B "
     "43 48 F7 6D E0 48 89 D0 48 C1 E8 3F 48 C1 FA 12 48 01 C2 48 39 D3 0F 8E ? ? ? ? "
     "48 8B 07 48 8B 80 30 07 00 00 48 8D 55 E0 48 89 F9 FF 15 ? ? ? ? 48 8B 45 E0 48 "
     "85 C0 75 ? C7 04 25 00 00 00 00 DE C0 AD DE 80 38 00 75 ? C7 04 25 00 00 00 00 DE "
     "C0 AD DE 48 8B 5D F0 48 8B 07 48 8B 80 18 07 00 00 48 89 F9 FF 15 ? ? ? ? 90 48 "
     "8D 55 D0 48 89 C1 E8 ? ? ? ? 90 48 8B 03 48 8B 80 C8 00 00 00 48 8D 55 D0 48 89 "
     "D9"},

    {Target::EmoteWheelSelected, L"emoteWheelSelected", L"emote wheel selection (DebugKeys F3+F4)",
     "41 56 56 57 53 48 83 EC 48 89 D7 48 89 CE 48 8B 81 48 0D 00 00 48 8B 48 48 48 85 C9 "
     "0F 84 ? ? ? ?"},

    {Target::EmoteWheelCtor, L"emoteWheelCtor", L"identify own emote wheel (DebugKeys F3+F4)",
     "55 56 57 48 81 EC D0 03 00 00 48 8D AC 24 80 00 00 00 48 C7 85 48 03 00 00 FE FF FF "
     "FF 4C 89 85 38 03 00 00 48 89 CE 0F 57 C0 0F 29 85 F0 02 00 00 48 8B 42 08 48 85 C0 "
     "74 ? F0 FF 40 08 48 8B 42 08 EB ? 31 C0 48 89 95 30 03 00 00 48 8B 0A 48 89 8D F0 "
     "02 00 00 48 89 85 F8 02 00 00 48 8D 95 F0 02 00 00 48 89 F1 E8 ? ? ? ? 90 48 8D "
     "05 ? ? ? ? 48 89 06 48 8D 05 ? ? ? ? 48 89 86 C8 0B 00 00 48 8B 86 48 0D 00 "
     "00 48 8B 48 48 48 85 C9 74 ? 80 39 00 75 ? C7 04 25 00 00 00 00 DE C0 AD DE 48 8B "
     "48 58 48 8B 01 48 8B 80 F0 00 00 00 48 89 B5 40 03 00 00 FF 15 ? ? ? ? 90 48 8B "
     "95 40 03 00 00 48 89 82 60 0D 00 00 0F 57 C0 0F 29 85 00 03 00 00 48 8B 8D 30 03 00 "
     "00 48 8B 41 08 48 85 C0 74 ? F0 FF 40 08 48 8B 41 08 EB ? C7 04 25 00 00 00 00 DE "
     "C0 AD DE 80 39 00 74 ? EB ? 31 C0 4C 8B 85 38 03 00 00 48 8D B2 68 0D 00 00 48 8B "
     "09 48 89 8D 00 03 00 00"},

    {Target::EmoteWheelBindings, L"emoteWheelBindings", L"emote binding vtables (DebugKeys F3+F4)",
     "48 8D 05 ? ? ? ? 48 89 85 20 02 00 00 48 89 8D 28 02 00 00 48 8D 9A 00 0B 00 00 C7 85 F4 03 00 00 8C D4 3E 3C"},

    {Target::ChatCommandRunSite, L"chatCommandRunSite", L"run a chat command like the chat screen (DebugKeys F3+F4 / F3+N)",
     "48 8B 48 48 48 89 4D F0 48 89 5D F8 48 8B 40 58 48 89 45 00 48 85 C9 0F 84 ? ? ? ? 80 39 00 0F 84 ? ? ? ? 48 8D 55 F0 48 89 F9 E8 ? ? ? ?"},

    {Target::ProfanityFilterGetter, L"profanityFilterGetter", L"options.filter_profanity getter (DebugScreen F3+D)",
     "48 83 EC 38 48 8B 05 ? ? ? ? 48 31 E0 48 89 44 24 30 48 8B 01 48 8B 40 08 48 8D 54 24 28 41 B8 34 02 00 00"},

    {Target::ActorAttachPos, L"actorAttachPos", L"Actor::getAttachPos (DebugScreen F3+B)",
     "41 57 41 56 41 55 41 54 56 57 55 53 48 83 EC 78 0F 29 74 24 60 48 89 D6 "
     "41 8D 40 FF 83 F8 01 77 ? 48 8B 81 48 02 00 00 F3 0F 10 40 14 F3 0F 10 "
     "48 08 F3 0F 5C C1 F3 0F 59 05 ? ? ? ? F2 0F 10 50 0C F2 0F 10 18 0F "
     "5C D3 0F 59 15 ? ? ? ? F3 0F 58 C1 0F 58 D3 0F 13 16 F3 0F 11 46 08 "
     "E9 ? ? ? ? 48 8B 79 08 8B 05 ? ? ? ? 8B 15 ? ? ? ? 65 4C 8B "
     "0C 25 58 00 00 00 49 8B 14 D1 3B 82 04 00 00 00 0F 8F ? ? ? ? 8B 05 "
     "? ? ? ? 48 8B 57 30 4C 8B 4F 38 49 29 D1 49 C1 E9 03 41 FF C9 41 21 "
     "C1 4E 8D 14 CA 48 8B 57 50 66 66 66 66 66 66 2E 0F 1F 84 00 00 00 00 00 "
     "4D 8B 0A 49 83 F9 FF 74 ? 4F 8D 0C 49 49 C1 E1 04 4E 8D 14 0A 42 39 44 "
     "0A 08 75 ? 4C 01 CA 48 39 57 58 74 ? 80 7A 2C 04 41 0F 94 C1 81 7A 28 "
     "B4 45 DC 7C 41 0F 95 C2 48 8B 42 10 48 85 C0 0F 94 C2 44 08 CA 44 08 D2 "
     "0F 84 ? ? ? ? 4C 8B 49 10 8B 41 18 4D 8B 59 48 49 8B 51 50 4C 29 DA "
     "48 C1 EA 03 BF FF FF FF FF 48 01 D7 89 FA 81 E2 11 72 BE BA 4D 8D 34 D3 "
     "4D 8B 51 68 31 D2 66 90 49 8B 1E 48 83 FB FF 0F 84 ? ? ? ? 48 C1 E3 "
     "05 4D 8D 34 1A 41 81 7C 1A 08 11 72 BE BA 75 ? 4C 01 D3 49 39 59 70 74 "
     "? 48 8B 53 10 48 85 D2 74 ? 89 C3 81 E3 FF FF 03 00 41 89 DE 41 C1 EE "
     "0B 4C 8B 7A 08 4C 8B 62 10 4D 29 FC 49 C1 FC 03 4D 39 E6 73 ? 4F 8B 34 "
     "F7 4D 85 F6 74 ? 81 E3 FF 07 00 00 89 C5 81 E5 00 00 FC FF 41 8B 1C 9E "
     "31 DD 81 FD FE FF 03 00 77 ? 41 89 DE 41 81 E6 80 FF 03 00 48 8B 52 50 "
     "41 C1 EE 04 4E 8B 34 32 83 E3 7F 49 8D 14 DE 4D 85 F6 49 0F 44 D6 EB ? "
     "31 D2 89 FB 81 E3 EE 68 D3 AC 4D 8D 3C DB 31 DB 4D 8B 37 49 83 FE FF 0F "
     "84 ? ? ? ? 49 C1 E6 05 4F 8D 3C 32 43 81 7C 32 08 EE 68 D3 AC 75 ? "
     "4D 01 D6 4D 39 71 70 0F 84 ? ? ? ? 49 8B 5E 10 48 85 DB 74 ? 41 89 "
     "C6 41 81 E6 FF FF 03 00 45 89 F7 41 C1 EF 0B 4C 8B 63 08 4C 8B 6B 10 4D "
     "29 E5 49 C1 FD 03 4D 39 EF 73 ? 4F 8B 3C FC 4D 85 FF 74 ? 41 81 E6 FF "
     "07 00 00 89 C5 81 E5 00 00 FC FF 47 8B 34 B7 44 31 F5 81 FD FE FF 03 00 "
     "77 ? 45 89 F7 41 81 E7 80 FF 03 00 48 8B 5B 50 41 C1 EF 04 4E 8B 3C 3B "
     "41 83 E6 7F 4B 8D 1C F6 49 8D 1C 9F 4D 85 FF 49 0F 44 DF EB ? 31 DB 81 "
     "E7 16 C3 21 DE 49 8D 3C FB 0F 1F 80 00 00 00 00 4C 8B 1F 49 83 FB FF 0F "
     "84 ? ? ? ? 49 C1 E3 05 4B 8D 3C 1A 43 81 7C 1A 08 16 C3 21 DE 75 ? "
     "4D 01 DA 4D 39 51 70 0F 84 ? ? ? ? 4D 8B 4A 10 4D 85 C9 0F 84 ? ? "
     "? ? 41 89 C2 41 81 E2 FF FF 03 00 45 89 D3 41 C1 EB 0B 49 8B 79 08 4D "
     "8B 71 10 49 29 FE 49 C1 FE 03 4D 39 F3 0F 83 ? ? ? ? 4E 8B 1C DF 4D "
     "85 DB 0F 84 ? ? ? ? 41 81 E2 FF 07 00 00 25 00 00 FC FF 47 8B 14 93 "
     "44 31 D0 3D FE FF 03 00 0F 87 ? ? ? ? 49 8B 41 50 45 89 D1 41 C1 E9 "
     "04 41 81 E1 F8 3F 00 00 4A 8B 04 08 48 85 C0 0F 84 ? ? ? ? 41 81 E2 "
     "FF FF 03 00 41 83 E2 7F 45 89 D1 41 C1 E1 06 4F 8D 0C 91 4C 01 C8 4C 8B "
     "91 50 02 00 00 4C 8B 99 40 02 00 00 4C 8B 89 48 02 00 00"},

    {Target::HealthAttributeLoad, L"healthAttributeLoad", L"health attribute global (PlayerList hearts)",
     "4C 8D 05 ? ? ? ? 48 8D 55 E8 48 89 F1 E8 ? ? ? ? 48 8B 55 E8 48 85 D2 0F 84 ? ? ? ? 48 8B 4D F0 48 85 C9 0F 84 ? ? ? ? F3 0F 10 42 74 F3 0F 11 42 7C 48 8B 41 48 FF 15 ? ? ? ?"},

    {Target::ItemIsFood, L"ItemIsFood", L"Item::isFood (AppleSkin getFood slot)",
     "48 83 EC 28 48 8B 01 48 8B 80 C8 00 00 00 FF 15 ? ? ? ? 48 85 C0 0F 95 C0 48 83 C4 28 C3"},

    {Target::HungerRendererUpdate, L"HungerRendererUpdate", L"HUD hunger bar update (AppleSkin)",
     "55 41 57 41 56 56 57 53 48 81 EC 28 05 00 00 48 8D AC 24 80 00 00 00 0F 29 B5 90 04 00 00 48 C7 85 88 04 00 00 "
     "FE FF FF FF 48 89 CE FF 41 10 48 89 D7 80 79 14 00"},

    {Target::HeartRendererUpdate, L"HeartRendererUpdate", L"HUD hearts update (AppleSkin)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC A8 0A 00 00 48 8D AC 24 80 "
     "00 00 00 0F 29 B5 10 0A 00 00 48 C7 85 08 0A 00 00 FE FF FF FF 48 89 D3 "
     "49 89 CF 48 8D 4D A0 E8 ? ? ? ? 0F 57 F6 F2 48 0F 2A 75 A0 41 80 7F "
     "10 00 0F 85 ? ? ? ? 48 8B 03 48 8B 80 38 03 00 00 48 8D 55 A0 48 89 "
     "D9 FF 15 ? ? ? ? 48 83 7D A0 00 74 ? 48 8B 03 48 8B 80 38 05 00 00 "
     "48 89 D9 FF 15 ? ? ? ? 90 48 85 C0 74 ? 48 8B 03 48 8B 80 38 05 00 "
     "00 48 89 D9 FF 15 ? ? ? ? 90 48 8B 08 48 8B 91 68 05 00 00 48 89 C1 "
     "48 89 D0 FF 15 ? ? ? ? 90 44 0F B6 80 C5 03 00 00 48 8B 55 A0 4C 89 "
     "F9 E8 ? ? ? ? 90 EB ? 48 8B 55 A0 4C 89 F9 45 31 C0 E8 ? ? ? ? "
     "90 41 C6 47 10 01 48 8B 7D A8 48 85 FF 74 ? F0 FF 4F 08 75 ? 48 8B 07 "
     "48 8B 00 48 89 F9 FF 15 ? ? ? ? F0 FF 4F 0C 75 ? 48 8B 07 48 8B 40 "
     "08 48 89 F9 FF 15 ? ? ? ? 48 8B 03 48 8B 80 F0 00 00 00 48 89 D9 FF "
     "15 ? ? ? ? 48 85 C0 0F 84 ? ? ? ? 48 89 C7 80 B8 89 02 00 00 00 "
     "0F 85 ? ? ? ? 48 8B 03 48 8B 80 C0 06 00 00 48 8D 55 A0 48 89 D9 FF "
     "15 ? ? ? ? 48 8B 45 A0 48 85 C0 75 ? C7 04 25 00 00 00 00 DE C0 AD "
     "DE 80 38 00 75 ? C7 04 25 00 00 00 00 DE C0 AD DE 48 8B 45 B0 83 B8 50 "
     "04 00 00 00 41 0F 95 47 11 0F 57 C0 4C 8B 75 A8 0F 11 45 A0 4D 85 F6 74 "
     "? F0 41 FF 4E 08 75 ? 49 8B 06 48 8B 00 4C 89 F1 FF 15 ? ? ? ? F0 "
     "41 FF 4E 0C 75 ? 49 8B 06 48 8B 40 08 4C 89 F1 FF 15 ? ? ? ? 4C 8B "
     "75 A8 48 C7 45 B0 00 00 00 00 4D 85 F6 74 ? F0 41 FF 4E 08 75 ? 49 8B "
     "06 48 8B 00 4C 89 F1 FF 15 ? ? ? ? F0 41 FF 4E 0C 75 ? 49 8B 06 48 "
     "8B 40 08 4C 89 F1 FF 15 ? ? ? ? 41 80 7F 11 01 0F 85 ? ? ? ? F2 "
     "0F 5E 35 ? ? ? ? F2 0F 59 35 ? ? ? ? F2 0F 2C C6 8B 8F C4 01 00 "
     "00 89 85 F0 09 00 00 44 69 C0 27 C6 04 00 48 8D 05 ? ? ? ? 48 89 45 "
     "A0 48 8D 45 A8 48 8D 15 ? ? ? ? 48 89 55 A8 48 BA 00 00 00 00 00 00 "
     "00 01 48 89 95 88 09 00 00 44 89 45 B0 C7 85 7C 09 00 00 00 00 00 00 48 "
     "C7 85 80 09 00 00 00 00 00 00 44 89 45 B4 BA 07 00 00 00 66 66 66 66 2E "
     "0F 1F 84 00 00 00 00 00 45 89 C1 41 C1 E9 1E 45 31 C1 45 69 C1 65 89 07 "
     "6C 49 01 D0 49 83 C0 FA 44 89 44 95 9C 48 81 FA 93 01 00 00 74 ? 45 89 "
     "C1 41 C1 E9 1E 45 31 C1 45 69 C1 65 89 07 6C 41 01 D0 41 83 C0 FB 44 89 "
     "44 95 A0 48 83 C2 02 EB ? 48 63 C9 48 69 D1 56 55 55 55 49 89 D0 49 C1 "
     "E8 3F 48 C1 EA 20 44 01 C2 81 E2 01 00 00 80 83 FA 01 41 0F 94 C4 83 F9 "
     "0A 41 0F 9D C5 48 B9 70 02 00 00 8E 01 00 00 48 89 8D 74 09 00 00 C6 85 "
     "90 09 00 00 01 48 89 85 98 09 00 00 8B B7 C8 01 00 00 4C 8D 05 ? ? ? "
     "? 48 8D 95 A0 09 00 00 48 89 F9 E8 ? ? ? ? 4C 8D 05 ? ? ? ? 48 "
     "8D 95 A8 09 00 00 48 89 F9 E8 ? ? ? ? 48 8B 8D A8 09 00 00 48 8B 85 "
     "A0 09 00 00 F3 0F 10 41 78 F3 0F 10 49 7C 45 31 C9 0F 57 D2 0F 2E D0 B9 "
     "00 00 00 00 73 ? 0F 2E 05 ? ? ? ? 73 ? F3 0F 2C C8 F3 0F 5B D8 0F "
     "5B DB 0F 2E D8 83 D1 00 83 F9 51 7C ? B9 50 00 00 00 F3 0F 10 40 7C 0F "
     "2E D1 73 ? 0F 2E 0D ? ? ? ? 73 ? F3 44 0F 2C C9 F3 0F 5B D1 0F 5B "
     "D2 0F 2E D1 41 83 D1 00 41 83 F9 51 7C ? 41 B9 50 00 00 00 48 89 BD F8 "
     "09 00 00 44 39 C9 44 0F 4C C9 45 31 F6 85 F6 BF 00 00 00 00 0F 4F FE 39 "
     "F1 0F 4C F9 0F 57 C9 0F 2E C8 73 ? 0F 2E 05 ? ? ? ? 73 ? F3 44 0F "
     "2C F0 F3 0F 5B C8 0F 5B C9 0F 2E C8 41 83 D6 00 41 83 FE 51 7C ? 41 BE "
     "50 00 00 00 44 89 EE 44 20 E6 89 8D F4 09 00 00 66 0F 6E C1 66 41 0F 6E "
     "CE 66 0F 62 C8 0F 5B C1 0F 59 05 ? ? ? ? F3 0F 5B C8 0F 5B D1 0F C2 "
     "C2 06 0F 14 C0 66 0F 50 C8 89 C8 D0 E8 0F B6 D0 66 0F 70 C1 55 66 0F 7E "
     "C0 48 89 85 C8 09 00 00 48 89 95 D0 09 00 00 44 8D 14 10 83 E1 01 66 0F "
     "7E C8 48 89 8D C0 09 00 00 48 89 85 D8 09 00 00 8D 14 08 44 01 D2 49 8D "
     "87 C0 32 00 00 48 89 85 B0 09 00 00 49 8B 87 C0 32 00 00"},

    {Target::FoodAttributeSite, L"FoodAttributeSite", L"hunger / saturation attributes and Actor::getEffect (AppleSkin)",
     "48 89 D9 BA 11 00 00 00 E8 ? ? ? ? 48 85 C0 0F 95 46 16 4C 8D 05 ? ? ? ? 48 8D 55 B0 48 89 D9"},

    {Target::ExhaustionAttributeSite, L"ExhaustionAttributeSite", L"exhaustion attribute (AppleSkin)",
     "48 89 F1 E8 ? ? ? ? 84 C0 74 ? 0F 28 75 40 48 81 C4 D8 00 00 00 5E 5D C3 4C 8D 05 ? ? ? ?"},

    {Target::DifficultySite, L"DifficultySite", L"Actor level and Level::getDifficulty slot (AppleSkin)",
     "48 8B 8B 00 02 00 00 48 8B 01 48 8B 80 20 01 00 00 FF 15 ? ? ? ? 85 C0 74 ? 4C "
     "8D 05 ? ? ? ? 48 8D 54 24 20 48 89 D9 E8 ? ? ? ? 48 8B 44 24 20 F3 0F 10 40 "
     "78 0F 2E 40 7C 77 ? 41 80 7E 7C 00 74 ? 0F B6 AC 24 80 00 00 00 48 8B 47 08 45 31 "
     "C0"},

    {Target::GameRulesSite, L"GameRulesSite", L"Level::getGameRules slot (AppleSkin natural regeneration)",
     "48 8B 46 20 48 8B 88 00 02 00 00 48 8B 01 48 8B 80 B0 0A 00 00 FF 15 ? ? ? ? 48 8B "
     "48 18 45 31 ED"},

    {Target::SelectedItemSlotSite, L"SelectedItemSlotSite", L"selected item vtable slot (HeldItem (FastBlockPlacement))",
     "48 8B 4E 08 80 FB 01 74 ? 45 85 E4 48 8B 01 48 8B 80 ? ? ? ? FF 15"},

    {Target::BuildBlockPlaceCheckSite, L"BuildBlockPlaceCheckSite", L"build-over check in buildBlock (FastBlockPlacement)",
     "48 8B 4F 08 48 8B 91 F8 01 00 00 8B 42 08 66 0F 1F 84 00 00 00 00 00 44 8D 40 01 F0 44 0F B1 42 08 75 ? 4C 8B A9 F0 01 00 00 48 8B B1 F8 01 00 00 48 85 F6 74 ? F0 FF 4E 08 75 ? 48 8B 06 48 8B 00 48 89 F1 FF 15 ? ? ? ? F0 FF 4E 0C 75 ? 48 8B 06 48 8B 40 08 48 89 F1 FF 15 ? ? ? ? 49 8B 45 00 48 8B 40 68 4C 89 E9 FF 15 ? ? ? ? 90 49 89 C5 48 8B 00 48 8B 40 10 4C 89 E9 4C 89 E2 FF 15 ? ? ? ? 90 41 8B 4C 24 08 89 8D 08 02 00 00 49 8B 0C 24 48 89 8D 00 02 00 00 48 8B 48 68 48 8B 11 4C 8B 92 A0 02 00 00 48 89 C2 4D 89 E8 4D 89 E1"},

    {Target::BuildBlockKeptClickSite, L"BuildBlockKeptClickSite", L"kept click point in buildBlock (Easy Place)",
     "48 8B 88 40 02 00 00 E8 ? ? ? ? 90 84 C0 74 ? 80 7F 41 00 48 8D 47 "
     "6C 48 0F 45 F0 8B 46 08 89 85 18 02 00 00 48 8B 06 48 89 85 10 02 00 00 "
     "48 8B 43 08 48 8B 4B 18 48 8B 00 8B 51 08"},

    {Target::PlayerGameModeSite, L"PlayerGameModeSite", L"player's GameMode field (Scaffold / FastBlockPlacement / Easy Place)",
     "48 8B 8E ? ? ? ? 48 8B 85 ? ? ? ? 48 89 86 ? ? ? ? 48 85 C9 74 ? 48 8B 01 48 8B 00 BA 01 00 00 00 FF 15 ? ? ? ? "
     "48 8B 8E ? ? ? ? 48 8B 01 48 8B 80 ? ? ? ? FF 15 ? ? ? ? 90 48 89 F9 44 89 F2 41 89 C0 E8"},

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

    {Target::GuiScaleCompute, L"GuiScaleCompute", L"GUI scale calculation (CustomGuiScale)",
     "41 57 41 56 41 55 41 54 56 57 55 53 48 83 EC 38 48 89 CE 48 8B 05 ? ? ? ? 48 31 "
     "E0 48 89 44 24 30 F2 0F 10 02 F2 41 0F 10 08 0F 59 C8 F2 41 0F 10 41 08 0F 59 C1 0F "
     "13 44 24 28 48 8B 89 98 00 00 00 48 8B 01 48 8B 80 98 06 00 00 FF 15 ? ? ? ? 66 "
     "66 66 66 2E 0F 1F 84 00 00 00 00 00 48 89 C1 48 8B 40 08 48 8B 80 38 02 00 00 48 85 "
     "C0 75 ? 44 8B 79 18 48 8D 7C 24 28 48 89 F1 48 89 FA E8 ? ? ? ? 89 C3 48 89 F1 "
     "48 89 FA E8 ? ? ? ? 89 C5 48 89 F1 48 89 FA E8 ? ? ? ? 89 C0 4C 8D 35 ? ? "
     "? ? 0F 57 C0 F3 0F 2A C5 F3 41 0F 10 0C 86"},

    {Target::GuiScaleProviderVtableSite, L"GuiScaleProviderVtableSite", L"GUI scale option provider (CustomGuiScale)",
     "48 89 D6 0F 57 C0 0F 11 41 40 48 C7 41 50 00 00 00 00 48 8D 05 ?? ?? ?? ?? 48 89 8D 18 01 00 00 48 89 01 48 8B 02 48 8B 40 38"},

    {Target::ClientRefreshScreenSize, L"ClientRefreshScreenSize", L"refresh GUI size (CustomGuiScale)",
     "56 57 53 48 83 EC 40 48 89 CE 48 8B 01 48 8B 80 C0 06 00 00 48 8D 54 24 28 FF 15 ? ? "
     "? ? 48 8B 44 24 28"},

    {Target::AccessibilitySettingsBuild, L"AccessibilitySettingsBuild", L"accessibility settings group builder (CustomGuiScale)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC F8 06 00 00 48 8D AC 24 80 00 00 00 0F "
     "29 B5 60 06 00 00 48 C7 85 58 06 00 00 FE FF FF FF 48 8B 79 08 0F 57 C0 0F 11 02"},
    {Target::VideoSettingsBuild, L"VideoSettingsBuild", L"video settings group builder (CustomGuiScale)",
     "55 41 57 41 56 41 54 56 57 53 48 81 EC 70 03 00 00 48 8D AC 24 80 00 00 00 48 C7 85 "
     "E8 02 00 00 FE FF FF FF 48 89 D3 48 8B 79 08 48 8B 07"},

    {Target::FovOptionCtor, L"FovOptionCtor", L"field of view FloatOption vtable and range (ExtendedFov)",
     "BA 2F 00 00 00 45 31 C0 41 B9 10 00 00 00 E8 ? ? ? ? 90 48 8D 05 ? ? ? ? 48 89 03 C7 43 20 6F 12 83 3A 0F 28 05 ? ? ? ? 0F 11 43 10"},

    {Target::FovRenderClamp, L"FovRenderClamp", L"upper clamp of the rendered field of view (ExtendedFov)",
     "F3 0F 5E FA 0F 28 C7 F3 0F 5F 05 ? ? ? ? F3 0F 10 0D ? ? ? ? 0F 28 F1 F3 0F C2 F7 01 0F 54 CE 0F 55 F0 0F 56 F1"},

    {Target::KeyBindingUnassignConflicts, L"KeyBindingUnassignConflicts",
     L"unassign other key bindings that use the key just bound (AllowDuplicateKeys)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 83 EC 68 48 8D 6C 24 60 48 C7 45 00 FE FF FF FF 48 89 D7 48 89 CB "
     "48 8D 55 C8 E8 ? ? ? ? 48 8B 75 C8 4C 8B 65 D0 4C 39 E6 0F 84 ? ? ? ? 4C 8D 7D E0 EB ? C7 02 00 00 00 00 "
     "48 83 40 28 04 48 8B 4B 10 4C 89 FA E8"},

    {Target::KeyBindingUnassignOthers, L"KeyBindingUnassignOthers",
     L"unassign other key bindings after resetting a binding (AllowDuplicateKeys)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 83 EC 68 48 8D 6C 24 60 48 C7 45 00 FE FF FF "
     "FF 48 89 D7 48 89 CB 48 8D 55 D8 E8 ? ? ? ? 48 8B 75 D8"},

    {Target::EnchantCommandExecute, L"EnchantCommandExecute", L"EnchantCommand::execute (ExtendedEnchantLevel)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 98 04 00 00 48 8D AC 24 80 00 00 00 0F "
     "29 B5 00 04 00 00 48 C7 85 F8 03 00 00 FE FF FF FF 4D 89 C5 48 89 CB 44 8B B9 E8 00 "
     "00 00 41 80 FF 2B"},

    {Target::EnchantLevelRangeCheckSite, L"EnchantLevelRangeCheckSite",
     L"/enchant's level range check call (ExtendedEnchantLevel)",
     "48 8B 01 48 8B 40 28 FF 15 ? ? ? ? 8B 8B F0 00 00 00 BA 01 00 00 00 41 89 C0 4D 89 E9 E8 ? ? ? ? 84 C0 0F 84"},

    {Target::ApplyEnchantCanEnchantSite, L"ApplyEnchantCanEnchantSite",
     L"EnchantUtils::applyEnchant's can-enchant call (ExtendedEnchantLevel)",
     "48 8B 3F 48 89 7D F8 48 89 EA 4C 89 F1 49 89 F8 41 89 D9 E8 ? ? ? ? "
     "90 B8 04 00 00 00 40 80 FF 29 77 ?"},

    {Target::EnchantCanEnchantCheckSite, L"EnchantCanEnchantCheckSite",
     L"EnchantUtils::canEnchant's can-enchant call (ExtendedEnchantLevel)",
     "4C 89 C7 48 89 CE 4C 8D 75 A8 48 89 D1 4C 89 F2 E8 ? ? ? ? 4C 8B 07 4C 89 F1 48 89 F2 41 89 D9 E8 ? ? ? ?"},

    {Target::StructureSizeClampServer, L"StructureSizeClampServer",
     L"integrated server's 64-block clamp of the structure block size (ExtendedStructureSize)",
     "41 83 FA 40 BE 40 00 00 00 44 0F 4D D6 41 83 F9 02 45 0F 4C CB 44 8B 7D "
     "70 41 83 FF 02 45 0F 4D DF 41 83 FB 40 44 0F 4D DE 44 89 5D 70 44 89 4D "
     "74 44 89 55 78 41 FF CA 0F 57 C0"},

    {Target::MobEffectScreenListLoop, L"MobEffectScreenListLoop",
     L"vanilla mob effect screen's effect list walk (InventoryEffects)",
     "41 FF C5 0F B6 47 42 88 43 F8 8B 05 ? ? ? ? 4C 8D 05 ? ? ? ? 0F 1F 44 00 00 48 81 C7 A8 00 00 00 4C 39 E7 0F 84 ? ? ? ? 8B 0F 48 83 F9 26 77 ? 39 C1 74 ? 49 8B 0C C8 48 85 C9 74 ? 83 B9 A0 00 00 00 00 78 ? 8B 4F 04 85 C9 74 ? 83 F9 FF 0F 95 C2 85 C9 0F 9E C1 84 CA 75 ?"},

    {Target::MobEffectInstanceDisplayName, L"MobEffectInstanceDisplayName",
     L"MobEffectInstance::getDisplayName (InventoryEffects)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 38 01 00 00 48 8D AC 24 80 00 00 00 48 "
     "C7 85 B0 00 00 00 FE FF FF FF 49 89 D6 48 89 CB 8B 05 ? ? ? ? 8B 0D ? ? ? ? "
     "65 48 8B 14 25 58 00 00 00 48 8B 0C CA 3B 81 04 00 00 00 0F 8F ? ? ? ? 0F 57 C0 "
     "0F 29 45 B0 8B 03 48 83 F8 26 76 ? 4C 8D 05 ? ? ? ? EB ? 48 8D 0D ? ? ? ? "
     "48 8B 04 C1 48 8D 88 80 00 00 00 48 85 C0"},

    {Target::MobEffectDurationText, L"MobEffectDurationText", L"mob effect duration as m:ss (InventoryEffects)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC D8 00 00 00 48 8D AC 24 80 00 00 00 48 "
     "C7 45 50 FE FF FF FF 89 D7 48 89 CE 48 63 C2 48 69 C8 67 66 66 66 48 89 CA 48 C1 EA "
     "3F 48 C1 E9 20 C1 F9 03 01 D1 4C 69 E8 B5 81 4E 1B 4C 89 E8 48 C1 E8 3F"},

    {Target::MobEffectIconNameSite, L"MobEffectIconNameSite", L"mob effect icon name fields (InventoryEffects)",
     "8B 04 01 48 83 F8 26 0F 87 ? ? ? ? 48 8D 0D ? ? ? ? 48 8B 04 C1 48 85 C0 0F 84 ? ? ? "
     "?"},

    {Target::MobEffectColorStore, L"MobEffectColorStore", L"MobEffect colour fields (InventoryEffects)",
     "F3 0F 2A C1 C1 E8 10 F3 0F 2A CA 0F B6 C0 F3 0F 2A D0 44 88 6E 0C 0F 14 D1 F3 0F 10 "
     "35 ? ? ? ? 0F 14 C6 0F 16 D0 0F 5E 15 ? ? ? ? 0F 11 56 10 48 8D 4E 28 0F 57 "
     "FF 0F 11 7E 28"},

    {Target::MobEffectsRendererRender, L"MobEffectsRendererRender", L"HUD mob effect icons renderer (EffectTimer)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 78 06 00 00 48 8D AC 24 80 00 00 00 44 "
     "0F 29 BD E0 05 00 00 44 0F 29 B5 D0 05 00 00 44 0F 29 AD C0 05 00 00"},

    {Target::MobEffectsRendererOwnerSite, L"MobEffectsRendererOwnerSite", L"GuiData slot and owner position in the effect renderer (EffectTimer)",
     "49 8B 45 00 48 8B 80 B0 07 00 00 4C 89 E9 FF 15 ? ? ? ? 90 49 89 C7 F6 47 18 01 "
     "74 ? 48 89 F9 E8 ? ? ? ? 90 F3 0F 2C 47 10 41 89 47 44 49 8B 45 00 48 8B 80 B0 "
     "07 00 00 4C 89 E9"},

    {Target::MobEffectsRendererLoop, L"MobEffectsRendererLoop", L"effect layout records walk in the effect renderer (EffectTimer)",
     "44 89 C7 4C 8B 7A 18 48 8B 42 20 4C 29 F8 48 C1 F8 02 49 0F AF C1 48 39 F8 0F 86 ? ? ? ? 4C 8B 31 48 8B 41 08 4C 29 F0 48 C1 F8 03 49 0F AF C2 48 39 F8 0F 86 ? ? ? ? 41 83 F8 26 77 ? 48 8D 05 ? ? ? ? 48 8B 34 F8 48 85 F6 74 ? 48 69 C7 A8 00 00 00 49 01 C6 8B 05 ? ? ? ? 41 39 06 74 ? 83 BE A0 00 00 00 00 78 ?"},

    {Target::MobEffectsRendererBackground, L"MobEffectsRendererBackground", L"effect background rect field in the effect renderer (EffectTimer)",
     "49 8D 57 14 0F 28 E3 0F C6 DB 55 66 41 0F 7E D0 66 0F 70 D2 55 66 41 0F 7E D1 F3 0F "
     "11 5C 24 38 F3 0F 11 64 24 30 F3 0F 11 4C 24 28 F3 0F 11 44 24 20 C7 44 24 48 00 00 "
     "00 00 C7 44 24 40 00 00 00 00 48 8B 8D 20 05 00 00 E8 ? ? ? ? 90"},

    {Target::UiControlPositionStore, L"UiControlPositionStore", L"UIControl absolute position fields (EffectTimer)",
     "0F 13 76 10 80 66 18 FE 0F 28 75 F0 48 83 C4 50 5F 5E 5D C3 66 66 66 2E 0F 1F 84 00 "
     "00 00 00 00 48 89 54 24 10"},

    {Target::GuiDataGuiScale, L"GuiDataGuiScale", L"GuiData::getGuiScale (EffectTimer)",
     "55 56 57 53 48 83 EC 58 48 8D 6C 24 50 0F 29 75 F0 48 C7 45 E8 FE FF FF FF 48 89 CE "
     "80 B9 B0 0C 00 00 01 0F 85 ? ? ? ? 48 8B 8E 98 00 00 00"},

    {Target::MobEffectsLayout, L"MobEffectsLayout", L"HUD mob effect icon layout (EffectTimer columns)",
     "41 57 41 56 41 55 41 54 56 57 55 53 48 81 EC F8 00 00 00 44 0F 29 BC 24 E0 00 00 00 "
     "44 0F 29 B4 24 D0 00 00 00 44 0F 29 AC 24 C0 00 00 00 44 0F 29 A4 24 B0 00 00 00 44 "
     "0F 29 9C 24 A0 00 00 00 44 0F 29 94 24 90 00 00 00 44 0F 29 8C 24 80 00 00 00 44 0F "
     "29 44 24 70 0F 29 7C 24 60 0F 29 74 24 50 48 89 CE 48 8B 05 ? ? ? ? 48 31 E0 48 89 "
     "44 24 48"},

    {Target::MobEffectsLayoutRects, L"MobEffectsLayoutRects", L"HUD mob effect rect fields written by the layout (EffectTimer columns)",
     "0F 11 5F 14 8B 4E 38 8B 56 3C 44 8D 04 11 01 C2 66 0F 6E D0 66 0F 6E DA 66 0F 62 D3 "
     "0F 5B D2 66 0F 6E D9 66 41 0F 6E E0 66 0F 62 DC 0F 5B DB 0F 58 D9 0F 16 DA 0F 11 5F "
     "04 4C 8D 1D ? ? ? ? F3 0F 10 4E 08 F3 0F 5D C8"},

    {Target::TickingTextureRender, L"TickingTextureRender", L"TickingTextureStage::render (NoBlockAnimation)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 78 01 00 00 48 8D AC 24 80 00 00 00 0F 29 B5 E0 00 00 00 "
     "48 C7 85 D8 00 00 00 FE FF FF FF 48 89 D3 48 89 CF 8B 05 ? ? ? ? 8B 0D ? ? ? ? 65 48 8B 14 25 58 00 00 00"},

    {Target::ItemActorRender, L"ItemActorRender", L"ItemRenderer::render for dropped items (NoRender Item)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 88 01 00 00 48 8D AC 24 80 "
     "00 00 00 44 0F 29 95 F0 00 00 00 44 0F 29 8D E0 00 00 00 44 0F 29 85 D0 "
     "00 00 00 0F 29 BD C0 00 00 00 0F 29 B5 B0 00 00 00 48 C7 85 A8 00 00 00 "
     "FE FF FF FF 49 8B 30 48 85 F6 0F 84 ? ? ? ? 4D 89 C5 48 89 D7 48 89 "
     "CB 48 89 F1 BA 40 00 00 00 E8 ? ? ? ? 84 C0 0F 84 ? ? ? ? 48 8D "
     "8E D0 03 00 00 48 8B 86 D0 03 00 00 48 8B 40 28 FF 15 ? ? ? ?"},

    {Target::GetBlockTessellator, L"GetBlockTessellator", L"JG-1 ghost mesh",
     "55 56 57 53 48 83 EC 38 48 8D 6C 24 30 48 C7 45 00 FE FF FF FF 48 89 CE 48 8B 89 18 "
     "04 00 00 48 8B 01 48 8B 80 40 01 00 00"},
    {Target::BlockRenderLayerOf, L"BlockRenderLayerOf", L"JG-1 ghost mesh",
     "56 48 83 EC 20 48 89 CE E8 ?? ?? ?? ?? 48 8B 4E 68 48 85 C0 0F 84 ?? ?? ?? ?? 48 8B 91 40 02 00 00"},

    {Target::TerrainMaterialRegistrySite, L"TerrainMaterialRegistrySite", L"JG-1 ghost mesh",
     "48 BB 79 4B 29 A1 36 6F 8F C5 48 89 9D E0 01 00 00 48 8D 15 ? ? ? ? 48 8D 8D A0 "
     "00 00 00 4C 8D 85 E0 01 00 00 E8 ? ? ? ? 90 48 8B 85 A8 00 00 00 48 85 C0"},

    {Target::AirBlockLookupSite, L"AirBlockLookupSite", L"JG-1 ghost mesh",
     "48 8D 0D ? ? ? ? 4C 8D 05 ? ? ? ? 48 8D 55 50 45 31 C9 E8 ? ? ? ? 90 48 8B 45 50 48 85 C0 75 ? 48 8D 3D ? ? ? ? 4C 8D 2D ? ? ? ? 48 8D 75 50 66 66 66 66 66 66 2E 0F 1F 84 00 00 00 00 00 C7 44 24 20 00 00 00 00 48 89 F9 48 89 F2 4D 89 E8 45 31 C9 E8 ? ? ? ? 90 48 8B 45 50 48 85 C0 74 ? F3 0F 7E 80 40 02 00 00"},

    {Target::BlockTypeIsValidSpawn, L"BlockTypeIsValidSpawn", L"can a mob spawn on this block (LightOverlay)",
     "56 57 53 48 83 EC 60 4C 89 CB 4C 89 C6 48 89 D7 48 81 C1 38 03 00 00 B2 0B E8 ? ? ? ? 48 85 C0 74"},

    {Target::InvisibleLayerBarrier, L"InvisibleLayerBarrier", L"draw the barrier terrain layer (ShowInvisible)",
     "41 F6 C6 01 74 ? 48 8D 05 ? ? ? ? 48 89 44 24 20 C7 44 24 30 00 00 00 00 C7 44 24 28 00 00 00 40"},

    {Target::InvisibleLayerLight, L"InvisibleLayerLight", L"draw the light block terrain layer (ShowInvisible)",
     "41 F6 C6 02 48 8B B5 18 0B 00 00 74 ? 48 8D 05 ? ? ? ? 48 89 44 24 20 C7 44 24 "
     "30 00 00 00 00 C7 44 24 28 00 00 00 40 48 8D 8D 40 09 00 00 48 89 F2 4D 89 E8 49 89 "
     "F9 E8 ? ? ? ? 90 48 8D 8D 98 07 00 00 E8 ? ? ? ? 0F 28 85 40 09 00 00 0F 11 "
     "85 98 07 00 00"},
    {Target::InvisibleLayerStructureVoid, L"InvisibleLayerStructureVoid",
     L"draw the structure void terrain layer (ShowInvisible)",
     "41 F6 C6 04 74 ? 48 8D 05 ? ? ? ? 48 89 44 24 20 C7 44 24 30 00 00 00 00 C7 44 24 28 00 00 00 40"},

    {Target::SpectatorViewerFlag, L"SpectatorViewerFlag", L"is the camera owner a spectator (ShowInvisible)",
     "B1 01 83 FB 06 74 ? 89 D9 83 F1 05 83 F0 06 09 C8 0F 94 C1 EB ? 4C 8B B5 B0 01 00 "
     "00 49 8D 86 D8 01 00 00 48 89 85 A8 00 00 00 49 8D 86 38 09 00 00 48 89 45 68 83 FB "
     "FF 0F 95 C0 20 C8 88 85 82 00 00 00 48 8D 05 ? ? ? ? 48 89 45 F0 48 89 7D F8"},

    {Target::InvisibleTaskBarrier, L"InvisibleTaskBarrier", L"draw the barrier terrain layer in the render task (ShowInvisible)",
     "0F B6 46 48 A8 01 74 ? 0F 57 C0 0F 29 45 ? 48 8B 46 28 48 85 C0 74 ? F0 FF 40 08"},
    {Target::InvisibleTaskLight, L"InvisibleTaskLight", L"draw the light block terrain layer in the render task (ShowInvisible)",
     "0F B6 46 48 A8 02 74 ? 0F 57 C0 0F 29 45 ? 48 8B 46 28 48 85 C0 74 ? F0 FF 40 08"},
    {Target::InvisibleTaskStructureVoid, L"InvisibleTaskStructureVoid",
     L"draw the structure void terrain layer in the render task (ShowInvisible)",
     "0F B6 46 48 A8 04 74 ? 0F 57 C0 0F 29 45 ? 48 8B 46 28 48 85 C0 74 ? F0 FF 40 08"},

    {Target::BlockSourceSetBlock, L"BlockSource::setBlock", L"set a block in the client's chunk (IgnoreBlock)",
     "41 57 41 56 56 57 55 53 48 81 EC 98 00 00 00 44 89 CD 4D 89 C6 48 89 D7 48 89 CE 48 "
     "8B 05 ? ? ? ?"},

    {Target::SubChunkRequestSite, L"SubChunkRequestSite", L"ask the server for the sub chunks around the player (F3+A)",
     "48 8B 98 C8 01 00 00 4C 8B B8 D0 01 00 00 48 8B 8E F8 01 00 00 8B 41 08 8D 50 01 F0 0F B1 51 08 "
     "75 ? 4C 8B A6 F0 01 00 00 4C 8B B6 F8 01 00 00 4D 85 F6 74 ? F0 41 FF 4E 08 75 ? 49 8B 06 48 "
     "8B 00 4C 89 F1 FF 15 ? ? ? ? F0 41 FF 4E 0C 75 ? 49 8B 06 48 8B 40 08 4C 89 F1 FF 15 ? ? "
     "? ? 49 29 DF 41 0F B7 84 24 CA 00 00 00 66 41 2B 84 24 C8 00 00 00 8D 48 0F 66 85 C0 0F 49 C8 "
     "0F BF C1 C1 E8 04 0F B7 C0 49 C1 FF 03 48 B9 C5 4E EC C4 4E EC C4 4E 4C 0F AF F9 49 39 C7 4C 0F "
     "43 F8 4D 85 FF 0F 84 ? ? ? ? 49 6B FF 68 45 31 E4 EB ? 49 83 C4 68 4C 39 E7 74 ? 42 83 7C "
     "23 18 02 75 ? 42 0F BE 44 23 61 8B 4D A0 8B 55 A4 89 4D F8 89 45 FC 89 55 00 4C 8B B6 00 11 00 "
     "00 4D 85 F6 74 ? 48 89 F1 4C 8D 7D B4 4C 89 FA E8 ? ? ? ? 49 8B 0E 4C 89 FA 4C 8D 45 F8 E8 "
     "? ? ? ? 90 41 88 C7 49 8B 0E 48 8D 55 B4 4C 8D 45 F8 4D 89 F1 E8 ? ? ? ? 90 41 84 C7 75 "
     "? 49 8B 0E 48 8D 55 B4 4C 8D 45 F8 4D 89 F1 E8 ? ? ? ? 90"},

    {Target::SkyDescriptionDimensionCall, L"SkyDescriptionDimensionCall",
     L"getDimensionId call while building the sky description (OverworldSky)",
     "48 8B 8E 88 09 00 00 48 8B 01 48 8B 40 18 48 8D 95 C4 01 00 00 FF 15 ? ? ? ? 90 "
     "41 80 7D 68 01 75 ? 49 8B 8D 30 07 00 00 48 85 C9 48 8D 05 ? ? ? ?"},

    {Target::SkyStateEndCheckCalls, L"SkyStateEndCheckCalls",
     L"two getDimensionId calls that decide the End sky in the per-frame sky state (OverworldSky)",
     "49 8B 8E 88 09 00 00 48 8B 01 48 8B 40 18 48 8D 95 80 2F 00 00 FF 15 ? ? ? ? 90 "
     "83 BD 80 2F 00 00 02 0F 94 85 C8 2D 00 00 49 8B 8E 88 09 00 00 48 8B 01 48 8B 40 18 "
     "48 8D 95 F8 2D 00 00 FF 15 ? ? ? ? 90 44 8B B5 FC 2D 00 00 44 22 B5 BF 2F 00 00 "
     "83 BD F8 2D 00 00 02 0F 57 C0 74 ? 48 8B 85 C0 2F 00 00"},

    {Target::DimensionTimeOfDayBase, L"DimensionTimeOfDayBase", L"Dimension::getTimeOfDay used by the overworld (OverworldSky)",
     "48 63 C2 48 69 C8 F1 19 76 05 48 89 CA 48 C1 EA 3F 48 C1 F9 29 01 D1 69 C9 C0 5D 00 00 29 C8 F3 0F 2A C0 F3 0F 58 C2 "
     "F3 0F 5E 05"},

    {Target::SkyColorBiomeCall, L"SkyColorBiomeCall", L"biome sky color lookup for the sky color fade (OverworldSky)",
     "4C 8D 45 ? 48 89 D9 48 89 FA E8 ? ? ? ? 48 8D 4D ? 48 89 DA 0F 28 D0 E8 ? ? ? ? 48 8D 86 ? ? 00 00 "
     "80 BE ? ? 00 00 01 0F 85"},

    {Target::SkyFogBrightnessCall, L"SkyFogBrightnessCall", L"fog color darkened by sky brightness (OverworldSky)",
     "48 8B 8E 88 09 00 00 48 8B 01 48 8B 80 C0 00 00 00 48 8D 55 40 4C 8D 85 10 01 00 00 "
     "FF 15 ? ? ? ? 90 0F 10 45 40 0F 29 85 10 01 00 00 48 8B 96 88 09 00 00 48 8D 4D "
     "40"},

    {Target::OverworldFogBrightness, L"OverworldFogBrightness", L"overworld fog color brightness (OverworldSky)",
     "48 89 D0 41 0F 10 00 0F 11 02 F3 0F 10 05 ? ? ? ? F3 0F 59 C3 F3 0F 58 05 ? ? "
     "? ? F2 0F 10 0A 0F 14 C0 0F 59 C1 0F 13 02 F3 0F 59 1D ? ? ? ? F3 0F 58 1D ? "
     "? ? ? F3 0F 59 5A 08 F3 0F 11 5A 08 C3 CC"},

    {Target::DimensionSunAngleBase, L"DimensionSunAngleBase", L"Dimension::getSunAngle used by the overworld (OverworldSky)",
     "56 57 48 83 EC 48 0F 29 7C 24 30 0F 29 74 24 20 0F 28 FA 48 89 D6 48 89 CF C6 42 10 00 48 8B 89 ? ? 00 00 "
     "48 8B 01 48 8B 80 ? ? 00 00 FF 15 ? ? ? ? 48 8B 48 18 0F 57 F6 48 85 C9"},

    {Target::WeatherTickLevels, L"WeatherTickLevels", L"Weather::tick moving the rain and lightning levels (OverworldSky)",
     "48 8B 41 ? 80 B8 ? ? 00 00 01 0F 85 ? ? ? ? 48 89 CE FF 41 ? F3 0F 10 41 ? F3 0F 10 51 ? F3 0F 11 41 ? "
     "F3 0F 11 51 ? F3 0F 10 49 ? 0F 2E CA"},

    {Target::SkyDarkenWeatherLoad, L"SkyDarkenWeatherLoad", L"Dimension weather load in the sky darkness (OverworldSky)",
     "48 8B 86 E8 01 00 00 F3 0F 10 50 4C F3 0F 59 15 ? ? ? ? F3 0F 59 15 ? ? ? ? F3 0F 58 "
     "15 ? ? ? ? F3 0F 59 D0"},

    {Target::ReloadUiDefinitions, L"reloadUiDefinitions", L"reload the UI definitions like F3+T (after a late injection)",
     "55 56 57 48 83 EC 40 48 8D 6C 24 40 48 C7 45 F8 FE FF FF FF 48 89 CE 48 8B 01 48 85 "
     "C0 0F 84 ? ? ? ? 80 38 00"},
};

static_assert(std::size(kTargets) == static_cast<size_t>(Target::Count));

}
