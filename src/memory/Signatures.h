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
    MakeCommandOrigin,
    MakeComplexTransaction,
    MakeInventoryAction,
    DestroyInventoryAction,
    AddInventoryAction,
    SendComplexTransaction,
    InventoryHoveredSlot,
    InventoryHotbarKey,
    MoveInputHandler,
    MouseReading,
    PlayerRotation,
    ViewPerspective,
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
     "56 48 83 EC 30 0F 29 74 24 20 48 8B 01 48 8B 50 08 8B 40 10"},

    {Target::SetSelectedSlot, L"SetSelectedSlot", L"selected slot control (AutoTool)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 88 02 00 00 48 8D AC 24 80 00 00 00 "
     "48 C7 85 00 02 00 00 FE FF FF FF 89 D6 83 FA 08 0F 87"},

    {Target::AbilitiesAccess, L"AbilitiesAccess", L"ability lookup (CreativeNoClip)",
     "56 57 55 53 48 83 EC 28 48 83 F9 01 0F 84 ? ? ? ? 48 83 F9 02"},

    {Target::UseItem, L"useItem", L"item use (FastRightClick)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 28 03 00 00 48 8D AC 24 80 00 00 00 "
     "48 C7 85 A0 02 00 00 FE FF FF FF 48 89 D6 48 89 CF 48 8D 9D E8 00 00 00 48 89 D9"},

    {Target::SetGameMode, L"SetGameMode", L"game mode change (GameModeSwitch)",
     "41 57 41 56 56 57 53 48 83 EC 40 44 89 C3 89 D7 48 89 CE"},

    {Target::SwapSlots, L"swapSlots", L"inventory slot swap (HandRestock)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC C8 00 00 00 48 8D AC 24 80 00 00 00 "
     "48 C7 45 40 FE FF FF FF 4C 8B A1 98 01 00 00 49 63 C0 48 69 F8 98 00 00 00"},

    {Target::ItemStackCopyCtor, L"ItemStack::ItemStack(const&)",
     L"copy-construct an item stack (ItemStackOps)",
     "55 56 57 48 83 EC 60 48 8D 6C 24 60 48 C7 45 F8 FE FF FF FF 48 89 D6 "
     "48 8D 05 ? ? ? ? 48 89 01 48 8D 41 08 48 89 45 E8 0F 57 C0 0F 11 41 08"},

    {Target::ItemStackAssign, L"ItemStack::operator=", L"assign an item stack (ItemStackOps)",
     "56 57 48 83 EC 28 48 89 D7 48 89 CE 0F B6 42 22 88 41 22 0F B7 42 20"},

    {Target::ItemStackDtor, L"ItemStack::~ItemStack", L"destroy an item stack (ItemStackOps)",
     "56 57 48 83 EC 28 48 89 CE 48 8D 05 ? ? ? ? 48 89 01 48 8B 49 78 48 85 C9 74"},

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
     "48 8D 55 E8 E8 ? ? ? ? 4C 8B 75 E8 4D 85 F6"},

    {Target::NotifyInventoryOpen, L"notifyInventoryOpen",
     L"tell the server the inventory opened (ItemStackRequest)",
     "55 41 56 56 57 53 48 81 EC 90 00 00 00 48 8D AC 24 80 00 00 00 "
     "48 C7 45 08 FE FF FF FF 89 D7 48 89 CE 48 8B 01 48 8B 80 F8 00 00 00 "
     "FF 15 ? ? ? ? 48 85 C0 0F 84 ? ? ? ? 48 89 C3 48 8B 06"},

    {Target::MoveInputHandler, L"moveInputHandler",
     L"skip the input reset while we tell the server the inventory opened",
     "48 83 EC 28 48 8B 01 48 8B 80 F8 00 00 00 FF 15 ? ? ? ? 48 85 C0 "
     "0F 84 ? ? ? ? 48 8B 50 10 8B 48 18 8B 42 50 4C 8B 42 48 44 29 C0"},

    {Target::MouseReading, L"MouseReading",
     L"IGameInput::GetCurrentReading(mouse) call site (console input capture)",
     "48 C7 45 18 00 00 00 00 4D 8B 07 48 8B 01 48 8B 40 20 BA 20 00 00 00 4C 8D 4D 18"},

    {Target::PlayerRotation, L"PlayerRotation",
     L"player body pitch/yaw writes (FreeCamera freezes the body)",
     "48 8B B6 28 02 00 00 F3 0F 10 44 24 2C F3 0F 11 46 04 "
     "F3 0F 10 4C 24 28 F3 0F 11 0E F3 0F 10 3D"},

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
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 98 01 00 00 48 8D AC 24 80 00 00 00 "
     "48 C7 85 10 01 00 00 FE FF FF FF 48 89 4D 48 C6 41 58 01 48 8B 32"},

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
};

static_assert(std::size(kTargets) == static_cast<size_t>(Target::Count),
              "kTargets と Target の要素数が一致していません");

inline constexpr std::string_view kSchematicaSignature =
    "48 8B C4 55 53 56 57 41 54 41 55 41 56 41 57 48 8D A8 ? ? ? ? 48 81 EC E8 05 00 00 "
    "0F 29 70 A8 0F 29 78 98 44 0F 29 40 88 44 0F 29 88 ? ? ? ? 44 0F 29 90 ? ? ? ? "
    "44 0F 29 98 ? ? ? ? 44 0F 29 A0 ? ? ? ? 44 0F 29 A8 ? ? ? ? 48 8B 05 ? ? ? ? "
    "48 33 C4 48 89 85 50 04 00 00";

}
