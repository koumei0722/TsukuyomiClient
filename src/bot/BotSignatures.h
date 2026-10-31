#pragma once
#include <string_view>

namespace tsukuyomi::bot {

enum class Site {
    NetworkReceivePacket,
    PeerSendSite,
    SenderToServerSite,
    SenderSubIdSite,
    NetherNetCreateTransport,
    NetherNetPeerSendSite,
    NetherNetConnectFlagSite,
    NetherNetSetSignalingSite,
    NetherNetOpenSessionSite,
    NetherNetAvailableSite,
    NetherNetReadSite,
    HttpSignalingVtableSite,
    HttpSignalingPeerIdSite,
    RequestNetworkSettingsSite,
    NetworkConnectionCreateSite,
    NetworkConnectionOptionsSite,
    NetworkConnectionDestroySite,
    PeerUpdateSite,
    CompressionPeerSite,
    CompressionFieldsSite,
    EnableEncryption,
    RakPeerCreateSite,
    RakPeerStartupSite,
    RakPeerConnectSite,
    RakPeerShutdownSite,
    RakPeerReceiveSite,
    RakPacketGuidSite,
    RakPacketDataSite,
    RakPeerDeallocSite,
    RakNetPeerSendSite,
    RakNetPeerAddressSite,
    NetherNetIdentityCtorSite,
    ClientHandshakeSecretSite,
    CreatePacket,
    BinaryStreamVtableSite,
    NetworkSystemSendSite,
    TransactionFromType,
    TransactionPayloadCtor,
    TransactionPayloadDtor,
    DescriptorCopyCtor,
    BlockNetworkIdSite,
    PathFinderSite,
    BlockSourceCtorSite,
    DimensionChunkSourceSite,
    SubChunkAskSite,
    TerrainHandlerCtor,
    TerrainHandlerTick,
    TerrainLevelDimension,
    TerrainPrepareRegion,
    TerrainTrackerMove,
    TerrainDestroyRegion,
    TerrainJobEnqueue,
    TerrainJobConsume,
    TerrainJobFinish,
    ActorGetInteraction,
    ActorInteractionSite,
    StackDescriptorCtorSite,
    ContainerAtPosition,
    LevelContainerInRange,
    ContainerScreenOpenSite,
    ContainerScreenCreateSite,
    SimpleContainerCtorSite,
    LevelContainerModelCtorSite,
    LevelContainerWeakRef,
    LevelContainerWeakRefSite,
    ContainerNameAt,
    ContainerCustomNameAt,
    MinecraftGameUpdate,
    FriendWorldJoinHandler,
    FriendWorldJoinerCtor,
    FriendJoinRequestHandle,
    FriendWorldFind,
    ClientDataBuild,
    ClientDataNonce,
    Count,
};

struct SiteInfo {
    Site site;
    const wchar_t* name;
    const wchar_t* purpose;
    std::string_view signature;
};

inline constexpr SiteInfo kSites[] = {
    {Site::NetworkReceivePacket, L"NetworkReceivePacket", L"decrypted packet reception (Bot)",
     "56 57 48 83 EC 28 48 89 D7 48 89 CE 48 8B 49 08 48 8B 01 48 8B 40 40 FF 15 ? ? ? ? 48 89 FA 48 83 7F 10 00 BF 02 00 00 00 0F 45 F8 85 C0 0F 45 F8 85 FF 75 ? 48 89 F1 45 31 C0 E8 ? ? ? ? 89 F8 48 83 C4 28 5F 5E C3 CC CC"},
    {Site::PeerSendSite, L"PeerSendSite", L"peer sendPacket fields (Bot)",
     "41 80 BE 99 01 00 00 00 75 ? 49 8B 8E 18 01 00 00 48 85 C9 74 ? 44 8B 47 0C 44 8B 4F 28 48 8B 01 48 8B 40 08 4C 8B 15 ? ? ? ? 48 8B 55 D0"},
    {Site::SenderToServerSite, L"SenderToServerSite", L"server connection vector fields (Bot)",
     "80 79 28 00 0F 85 ? ? ? ? 48 89 D6 48 8B 79 20 48 8B 87 E8 00 00 00 48 3B 87 F0 00 00 00 74 ? 4C 8B 20 41 0F 10 04 24"},
    {Site::SenderSubIdSite, L"SenderSubIdSite", L"sender SubClientId field (Bot)",
     "0F B6 41 18 88 42 10 80 79 28 00 0F 85 ? ? ? ? 48 89 D6 48 8B 79 20 48 8B 87 E8 00 00 00"},
    {Site::NetherNetCreateTransport, L"NetherNetCreateTransport", L"NetherNet transport factory (Bot extra connections)",
     "55 41 56 56 57 53 48 81 EC 10 0D 00 00 48 8D AC 24 80 00 00 00 48 C7 85 88 0C 00 00 "
     "FE FF FF FF 4C 89 CB 4D 89 C6"},
    {Site::NetherNetPeerSendSite, L"NetherNetPeerSendSite", L"NetherNet peer send (Bot extra connections)",
     "31 D2 41 83 F8 01 0F 94 C2 48 8B 41 28 4C 8B 41 48 4C 8B 51 40 4C 89 54 24 40 0F 10 41 30 0F 29 44 24 30 48 8B 88 E8 00 00 00 48 8B 01 48 8B 40 08 89 54 24 20 48 8D 54 24 30"},
    {Site::NetherNetConnectFlagSite, L"NetherNetConnectFlagSite", L"NetherNet trickle ICE flag (Bot extra connections)",
     "48 8B 8E E8 00 00 00 48 8B 01 80 BE E1 04 00 00 01 75 ? 48 8B 80 90 00 00 00 FF 15 ? ? ? ? 90 EB ? 48 8B 80 88 00 00 00 FF 15 ? ? ? ? 90"},
    {Site::NetherNetSetSignalingSite, L"NetherNetSetSignalingSite", L"NetherNet signaling slot (Bot extra connections)",
     "48 8B 8E E8 00 00 00 48 8B 01 48 8B 40 40 48 8D 55 10 FF 15 ? ? ? ? 90 48 8B 7D 18"},
    {Site::NetherNetOpenSessionSite, L"NetherNetOpenSessionSite", L"NetherNet open session slot (Bot extra connections)",
     "48 8B 01 48 8B 40 20 48 8D 55 D0 4C 8D 45 C0 FF 15 ? ? ? ? 90 48 8B 45 20 48 8B 70 08 48 85 F6"},
    {Site::NetherNetAvailableSite, L"NetherNetAvailableSite", L"NetherNet available slot (Bot extra connections)",
     "48 8B 43 28 4C 8B 43 48 48 8B 4B 40 48 89 4D E0 0F 10 43 30 0F 29 45 D0 48 8B 88 E8 00 00 00 48 8B 01 48 8B 40 10 4C 8D 65 D0 4C 8D 75 FC 4C 89 E2"},
    {Site::NetherNetReadSite, L"NetherNetReadSite", L"NetherNet read slot (Bot extra connections)",
     "48 8B 89 E8 00 00 00 48 8B 01 48 8B 40 18 4C 8B 15 ? ? ? ? 4C 89 74 24 28 89 54 24 20 48 8D 55 B0 41 FF D2 90 89 C1 B8 01 00 00 00 84 C9"},
    {Site::HttpSignalingVtableSite, L"HttpSignalingVtableSite", L"HttpSignalingClient vtable (Bot extra connections)",
     "48 8D 05 ? ? ? ? 48 89 06 48 C7 45 C8 00 00 00 00 48 C7 45 D0 0D 00 00 00 48 C7 45 D8 0F 00 00 00 48 B8 48 74 74 70 53 69 67 6E 48 89 45 C0"},
    {Site::HttpSignalingPeerIdSite, L"HttpSignalingPeerIdSite", L"HttpSignalingClient peer id (Bot extra connections)",
     "48 89 86 58 01 00 00 C6 86 68 01 00 00 01 0F 28 45 50 0F 28 4D 60 0F 11 8E 80 01 00 00 0F 11 86 70 01 00 00 48 C7 45 60 00 00 00 00 48 C7 45 68 0F 00 00 00 C6 45 50 00"},
    {Site::RequestNetworkSettingsSite, L"RequestNetworkSettingsSite", L"network protocol version (Bot extra connections)",
     "48 8D 05 ? ? ? ? 48 89 44 24 28 48 B8 ? ? ? ? 01 00 00 00 48 89 44 24 58 48 8B 4E 78"},
    {Site::NetworkConnectionCreateSite, L"NetworkConnectionCreateSite", L"NetworkConnection constructor (Bot extra connections)",
     "E8 ? ? ? ? 90 4C 8B BE 48 01 00 00 48 8B 0D ? ? ? ? 48 8B 01 48 8B 40 08 BA E0 01 00 00 FF 15 ? ? ? ? 90 49 89 C4 48 85 C0 0F 84 ? ? ? ? C6 45 08 00 48 8B 4D 60 80 79 28 01 75 ? 8B 01 89 45 E0 0F 10 41 08 0F 10 49 18 0F 11 4D F8 0F 11 45 E8 48 C7 41 18 00 00 00 00 48 C7 41 20 0F 00 00 00 C6 41 08 00 C6 45 08 01 48 8D 86 14 02 00 00 0F 28 45 20 0F 29 45 10 0F 57 C0 0F 29 45 20 0F 29 45 30 48 8B 8E 38 01 00 00 48 85 C9 74 ? F0 FF 41 08 48 8B 8E 38 01 00 00 EB ? 31 C9 48 8B 96 30 01 00 00 48 89 55 30 48 89 4D 38 48 8B 8E 40 01 00 00 48 89 4D 40 0F 10 0B 0F 29 4D D0 0F 11 03 48 8D 4D E0 48 89 4C 24 40 48 8D 4D 10 48 89 4C 24 38 48 89 44 24 30 4C 89 7C 24 28 48 8D 45 30 48 89 44 24 20 4C 8D 45 D0 4C 89 E1 48 89 FA 45 89 F1 4C 89 65 68 E8 ? ? ? ? 90 48 8B 45 68"},
    {Site::NetworkConnectionOptionsSite, L"NetworkConnectionOptionsSite", L"NetworkConnection optional argument (Bot extra connections)",
     "48 8B 8D 80 00 00 00 80 79 28 01 75 ? 8B 01 89 87 B0 01 00 00 0F 11 87 C8 01 00 00 0F 11 87 B8 01 00 00 0F 10 41 08 0F 10 49 18 0F 11 8F C8 01 00 00 0F 11 87 B8 01 00 00 48 C7 41 18 00 00 00 00 48 C7 41 20 0F 00 00 00 C6 41 08 00 C6 87 D8 01 00 00 01"},
    {Site::NetworkConnectionDestroySite, L"NetworkConnectionDestroySite", L"NetworkConnection destructor (Bot extra connections)",
     "48 8B 75 58 48 85 F6 74 ? 48 89 F1 E8 ? ? ? ? BA E0 01 00 00 48 89 F1 E8 ? ? ? ? 48 8B 75 28 48 85 F6"},
    {Site::PeerUpdateSite, L"PeerUpdateSite", L"NetworkPeer update/flush slots (Bot extra connections)",
     "48 8B 01 48 8B 40 20 48 8D 55 C0 FF 15 ? ? ? ? 90 48 8B 4D F8 48 85 C9 74 ? 48 8D 45 C0 48 39 C1 0F 95 C2 48 8B 01 48 8B 40 20 FF 15 ? ? ? ? 48 8B 4E 08 48 85 C9 74 ? 48 8B 01 48 8B 40 18 FF 15 ? ? ? ? 90 48 83 C4 68"},
    {Site::CompressionPeerSite, L"CompressionPeerSite", L"compression peer of a connection (Bot extra connections)",
     "48 8B 88 ? ? ? ? 48 85 C9 0F 84 ? ? ? ? 4C 8B B0 ? ? ? ? F0 FF 41 0C"},
    {Site::CompressionFieldsSite, L"CompressionFieldsSite", L"compression settings fields (Bot extra connections)",
     "66 83 7F 32 FF 41 0F 95 46 58 41 89 46 68 66 41 C7 46 6C 00 00 41 C7 46 70 00 00 00 00 66 41 C7 46 74 00 00 41 C7 46 78 00 00 00 00 48 85 DB 74 ?"},
    {Site::EnableEncryption, L"EnableEncryption", L"Bedrock encryption (Bot extra connections)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 88 00 00 00 48 8D AC 24 80 00 00 00 48 C7 45 00 FE FF FF FF 48 89 D7 48 89 CE 48 8B 49 08 48 8B 01 48 8B 40 ? FF 15 ? ? ? ? 84 C0"},
    {Site::RakPeerCreateSite, L"RakPeerCreateSite", L"RakPeer factory (Bot extra connections)",
     "48 83 3D ? ? ? ? 00 75 ? 48 8D 05 ? ? ? ? 48 89 05 ? ? ? ? 48 8D 4D 70 E8 ? ? ? ? 90 48 8B 7D 78 48 8B B5 A0 01 00 00 48 8B 8E 28 04 00 00 48 89 BE 28 04 00 00 48 85 C9 74 ? 48 8B 85 68 01 00 00 48 8B 00 FF 15 ? ? ? ? 90 48 8B BE 28 04 00 00 48 8B 45 70 48 8B 8D 68 01 00 00 48 89 01 48 8D 15 ? ? ? ? 48 8D 5D A8 41 B8 88 00 00 00 48 89 D9 E8 ? ? ? ? 48 8B 07 48 8B 80 B0 01 00 00 48 89 F9 BA 10 27 00 00 49 89 D8 FF 15 ? ? ? ? 90 48 8B 85 A0 01 00 00 48 8B 88 28 04 00 00 48 8B 01 48 8B 80 50 01 00 00 B2 01 FF 15 ? ? ? ? 90"},
    {Site::RakPeerStartupSite, L"RakPeerStartupSite", L"RakPeer::Startup (Bot extra connections)",
     "8B 53 0C 49 8B 06 48 8B 40 10 C7 44 24 20 61 79 FE FF 4C 8D 45 E0 4C 89 F1 41 B9 01 "
     "00 00 00 FF 15 ? ? ? ?"},
    {Site::RakPeerConnectSite, L"RakPeerConnectSite", L"RakPeer::Connect (Bot extra connections)",
     "48 8B 01 48 8B 40 ? 4C 8B 15 ? ? ? ? 89 54 24 30 C7 44 24 48 00 00 00 00 C7 44 24 40 ? ? ? ? C7 44 24 38 ? ? ? ? 48 C7 44 24 28 00 00 00 00 C7 44 24 20 00 00 00 00 48 89 F2 45 31 C9 41 FF D2"},
    {Site::RakPeerShutdownSite, L"RakPeerShutdownSite", L"RakPeer::Shutdown (Bot extra connections)",
     "48 8B 8E ? ? ? ? 48 8B 01 48 8B 40 ? 31 D2 FF 15 ? ? ? ? 48 8B 8E ? ? ? ? 48 8B 01 48 8B 40 ? BA ? ? ? ? 45 31 C0 41 B9 ? ? ? ? FF 15"},
    {Site::RakPeerReceiveSite, L"RakPeerReceiveSite", L"RakPeer::Receive (Bot extra connections)",
     "48 8B 8E 28 04 00 00 48 8B 01 48 8B 80 C8 00 00 00 FF 15 ? ? ? ? 48 85 C0 0F 84 ? ? ? ? 49 89 C5 8B 88 98 00 00 00 8D 81 00 00 00 E0 3D 00 00 00 E0 77 ?"},
    {Site::RakPacketGuidSite, L"RakPacketGuidSite", L"RakNet Packet guid (Bot extra connections)",
     "49 8B 95 88 00 00 00 48 89 95 28 01 00 00 45 0F B7 85 90 00 00 00 66 44 89 85 30 01 00 00 C7 85 B8 01 00 00 00 00 00 00 81 FF 8D 00 00 00"},
    {Site::RakPacketDataSite, L"RakPacketDataSite", L"RakNet Packet data (Bot extra connections)",
     "41 8B 9D 9C 00 00 00 83 C3 07 C1 EB 03 FF CB 4D 8B BD A0 00 00 00 66 0F 7F B5 40 03 00 00 41 BE 0F 00 00 00"},
    {Site::RakPeerDeallocSite, L"RakPeerDeallocSite", L"RakPeer::DeallocatePacket (Bot extra connections)",
     "48 8B 8E 28 04 00 00 48 8B 01 48 8B 80 D0 00 00 00 4C 89 EA FF 15 ? ? ? ? 90 80 7D D8 01"},
    {Site::RakNetPeerSendSite, L"RakNetPeerSendSite", L"RakNet peer send (Bot extra connections)",
     "BA 03 00 00 00 83 FD 03 77 0C 89 E8 48 8D 15 ? ? ? ? 8B 14 82 44 8B 43 10 41 FF C0 48 83 BF ? ? ? ? 10 72 07 48 8B B7 ? ? ? ? 48 8B 01 48 8B 80 ? ? ? ? 4C 8D 4C 24 48 4C 89 4C 24 30 89 54 24 20 C7 44 24 40 00 00 00 00 C6 44 24 38 00 C6 44 24 28 00 48 89 F2 41 B9 ? ? ? ? FF 15"},
    {Site::RakNetPeerAddressSite, L"RakNetPeerAddressSite", L"RakNet peer remote address (Bot extra connections)",
     "48 8B 49 ? 0F 10 46 ? 0F 29 44 24 20 48 8B 01 48 8B 80 ? ? ? ? 48 8D BC 24 ? ? ? ? 4C 8D 44 24 20 48 89 FA FF 15"},
    {Site::NetherNetIdentityCtorSite, L"NetherNetIdentityCtorSite", L"the game's WebRTC identity (Bot switching to a bot on its own connection)",
     "4C 89 85 C0 00 00 00 48 89 D7 48 89 CE 48 8D 05 ? ? ? ? 48 89 01 48 8D 59 08 48 89 D9 BA 05 00 00 00 E8 ? ? ? ? 90 48 8D 46 58 48 89 85 98 00 00 00 0F 57 C0"},
    {Site::ClientHandshakeSecretSite, L"ClientHandshakeSecretSite", L"the game's key exchange (Bot switching on RakNet servers)",
     "4C 8B B6 A0 00 00 00 48 8D 95 00 01 00 00 4C 89 F9 E8 ? ? ? ? 90 48 8D 8D D0 01 00 00 48 8D 95 00 01 00 00 41 B8 05 00 00 00 E8 ? ? ? ? 90 48 8D 55 30 4C 8D 85 D0 01 00 00 4C 89 F1 E8 ? ? ? ? 90 48 8D 05 ? ? ? ? 48 89 85 D0 01 00 00 48 8B 8D F8 01 00 00"},
    {Site::CreatePacket, L"CreatePacket", L"the game's packet factory (Bot packets)",
     "56 48 83 EC 20 48 89 CE 81 FA ? ? 00 00 77 ? 89 D0 48 8D 0D ? ? ? ? 48 63 04 81 48 01 C8 FF E0"},
    {Site::BinaryStreamVtableSite, L"BinaryStreamVtableSite", L"the game's BinaryStream (Bot packets)",
     "48 8D 05 ? ? ? ? 49 89 84 24 90 01 00 00 48 89 4D 18 49 89 8C 24 D8 01 00 00 49 8D 84 24 E0 01 00 00 48 89 45 28 41 0F 11 B4 24 E0 01 00 00"},
    {Site::NetworkSystemSendSite, L"NetworkSystemSendSite", L"the game's packet serialization context (Bot packets)",
     "4C 8B B3 38 02 00 00 48 89 8B B8 01 00 00 48 89 83 C0 01 00 00 48 C7 83 C8 01 00 00 00 00 00 00 48 8B 06 48 8B 40 08 48 89 F1 FF 15 ? ? ? ? 90 49 8B 0E 4C 8B 49 08 48 8D 55 58 4C 89 F1 41 89 C0 4C 89 C8 FF 15 ? ? ? ? 90 4C 8B BB 30 02 00 00 4C 8B 65 58 44 0F B6 76 10"},
    {Site::TransactionFromType, L"TransactionFromType", L"the game's inventory transaction factory (Bot actions)",
     "48 89 CE 83 FA 04 0F 87 ? ? ? ? 4C 89 C7 89 D0 48 8D 0D ? ? ? ? 48 63 04 81 48 01 C8 FF E0"},
    {Site::TransactionPayloadCtor, L"TransactionPayloadCtor", L"the game's inventory transaction payload (Bot actions)",
     "44 88 C3 48 89 95 ? ? ? ? 48 89 CF 48 8D 05 ? ? ? ? 48 89 01 C7 41 08 00 00 00 00 48 8D 41 10"},
    {Site::TransactionPayloadDtor, L"TransactionPayloadDtor", L"the game's inventory transaction payload destructor (Bot actions)",
     "56 48 83 EC 20 48 89 CE 48 8B 89 ? ? 00 00 48 85 C9 74 ? 48 8B 01 48 8B 00 BA 01 00 00 00 FF 15 ? ? ? ? 0F B6 86 ? ? 00 00 48 83 F8 04 77 ? 48 8D 4E ? 48 8D 15"},

    {Site::DescriptorCopyCtor, L"DescriptorCopyCtor", L"the game's item descriptor copy (Bot actions)",
     "48 89 D6 49 89 CE 48 8D 05 ? ? ? ? 48 89 01 48 8D 51 08 48 8B 4E 08 48 85 C9 74 ? 48 8B 01 "
     "48 8B 00 FF 15 ? ? ? ? EB ? 48 C7 02 00 00 00 00 48 8D 05 ? ? ? ? 49 89 06 0F B7 46 10 "
     "66 41 89 46 10 48 8D 05 ? ? ? ? 49 89 06 0F B6 46 18"},

    {Site::BlockNetworkIdSite, L"BlockNetworkIdSite", L"block network id field (Bot actions)",
     "48 8B 46 18 48 85 C0 74 ? 8B 80 14 01 00 00 89 43 38 48 89 5D F8 48 8D 43 40"},

    {Site::PathFinderSite, L"PathFinderSite", L"the game's pathfinder (Bot walk path)",
     "48 8D 4D B8 48 89 C2 4D 89 F0 E8 ? ? ? ? 90 4C 8D 05 ? ? ? ? 48 8D 95 50 01 00 00 48 89 "
     "F9 E8 ? ? ? ? 90 48 8B 85 50 01 00 00 66 41 0F 6E C5 66 0F 6E CB 66 0F 62 C8 0F 5B F1 F3 0F "
     "2A BD 98 03 00 00 F3 0F 58 3D ? ? ? ? 0F 58 35 ? ? ? ? F3 44 0F 10 40 7C 0F 13 B5 D8 02 "
     "00 00 F3 0F 11 BD E0 02 00 00 C6 85 E4 02 00 00 01 48 8D 8D 50 01 00 00 4C 8D 8D D8 02 00 00 48 "
     "89 FA 45 31 C0 E8 ? ? ? ? 90 0F 28 C6 0F C6 C6 55 F3 44 0F 11 44 24 30 F3 0F 11 7C 24 28 F3 "
     "0F 11 44 24 20 48 8D 4D B8 4C 8D 85 50 01 00 00 48 89 F2 0F 28 DE E8 ? ? ? ? 90 48 8D 8D 50 "
     "01 00 00 E8 ? ? ? ? 48 8D 4D B8 E8 ? ? ? ?"},
    {Site::BlockSourceCtorSite, L"BlockSourceCtorSite", L"the game's BlockSource constructor (Bot walk path)",
     "0F 57 F6 0F 11 71 08 48 8D 05 ? ? ? ? 48 89 01 E8 ? ? ? ? 41 89 46 18 45 88 6E 1C 45 88 7E 1D 45 88 66 1E 49 89 76 20 49 89 5E 28 49 89 7E 30 0F B7 87 CA 00 00 00 66 41 89 46 38"},
    {Site::DimensionChunkSourceSite, L"DimensionChunkSourceSite", L"the dimension's chunk source field (Bot walk path)",
     "56 48 83 EC 20 48 89 CE 48 8B 8A D8 01 00 00 48 85 C9 75 ? C7 04 25 00 00 00 00 DE C0 AD DE 48 8B 01 48 8B 40 10 FF 15 ? ? ? ? 84 C0 75 ? 48 8B 4E 10 C6 01 00 48 83 C4 20 5E C3"},

    {Site::SubChunkAskSite, L"SubChunkAskSite", L"the game's sub-chunk request queue (Bot walk path for far bots)",
     "42 83 7C 23 18 02 75 ? 42 0F BE 44 23 61 8B 4D A0 8B 55 A4 89 4D F8 89 45 FC 89 55 00 4C 8B B6 "
     "00 11 00 00 4D 85 F6 74 ? 48 89 F1 4C 8D 7D B4 4C 89 FA E8 ? ? ? ? 49 8B 0E 4C 89 FA 4C 8D "
     "45 F8 E8 ? ? ? ? 90 41 88 C7 49 8B 0E 48 8D 55 B4 4C 8D 45 F8 4D 89 F1 E8 ? ? ? ? 90 41 "
     "84 C7 75 ? 49 8B 0E 48 8D 55 B4 4C 8D 45 F8 4D 89 F1 E8 ? ? ? ? 90"},

    {Site::TerrainHandlerCtor, L"TerrainHandlerCtor", L"the game's chunk receiver constructor (Bot walk path for far bots)",
     "55 41 57 41 56 41 54 56 57 53 48 81 EC 10 01 00 00 48 8D AC 24 80 00 00 00 0F 29 B5 "
     "80 00 00 00 48 C7 45 78 FE FF FF FF 48 89 D6 48 89 CB 48 8D 05 ? ? ? ? 48 89 01 "
     "48 8B 0D ? ? ? ? 48 8B 01 48 8B 40 08 BA 18 00 00 00 FF 15 ? ? ? ? 48 85 C0 "
     "0F 84 ? ? ? ? 49 BE 01 00 00 00 01 00 00 00 4C 89 70 08 48 8D 0D ? ? ? ? 48 "
     "89 08 48 89 C1 48 83 C1 10 C6 40 10 01 48 89 4B 08 48 89 43 10 48 8D 05 ? ? ? ? "
     "48 89 03 48 8D 43 18 48 89 45 F8 0F 57 C0 0F 11 43 18 48 8B 46 58 48 89 43 18 48 8B "
     "46 60 48 89 5D 68 48 89 43 20 0F 11 46 58 48 8B 0D ? ? ? ? 48 8B 01 48 8B 40 08 "
     "BA 78 02 00 00 FF 15 ? ? ? ? 90 48 89 C7 48 85 C0 0F 84 ? ? ? ? 4C 89 77 08 "
     "48 8D 1D ? ? ? ? 48 89 1F 48 89 7D 70 48 83 C7 10 41 B8 68 02 00 00 48 89 F9 31 "
     "D2 E8 ? ? ? ? 90 48 89 F9 E8 ? ? ? ? 90 48 8B 45 68 48 8D 48 28 48 89 4D 00 "
     "48 89 78 28 48 8B 4D 70 48 89 48 30 48 8B 0D ? ? ? ? 48 8B 01 48 8B 40 08 BA 78 "
     "02 00 00 FF 15 ? ? ? ? 90 48 89 C7 48 85 C0 0F 84 ? ? ? ? 4C 89 77 08 48 89 "
     "1F 48 89 7D 70 48 83 C7 10 41 B8 68 02 00 00 48 89 F9 31 D2 E8 ? ? ? ? 90 48 89 "
     "F9 E8 ? ? ? ? 90 48 8B 55 68 48 89 7A 38 48 8B 45 70 48 89 42 40 0F 57 C0 0F 11 "
     "42 48 48 8B 46 30 48 89 42 58 0F 11 42 60 48 8B 46 40 48 85 C0 74 ? F0 FF 40 08 48 "
     "8B 46 40 EB ? 31 C0 48 8B 4E 38 48 89 4A 60 48 89 42 68 48 8B 46 48 48 89 42 70 48 "
     "85 C9 0F 84 ? ? ? ? 80 39 00 0F 84 ? ? ? ? 48 8B 46 20 48 89 42 78 0F 11 82 "
     "80 00 00 00 48 83 7E 08 00 74 ? 48 8B 06 48 89 82 80 00 00 00 48 8B 46 08 48 89 82 "
     "88 00 00 00 F0 FF 40 0C 48 8D 42 38 48 89 45 08 48 8D 42 48 48 89 45 10 48 8D 42 50 "
     "48 89 45 18 48 8D 42 60 48 89 45 20 48 8B 46 10 48 89 82 90 00 00 00 48 8B 46 18 48 "
     "89 82 98 00 00 00 48 8B 46 28 48 89 82 A0 00 00 00 48 8B 46 50 48 89 82 A8 00 00 00 "
     "48 8D 82 B0 00 00 00 48 89 45 28 0F 11 82 B0 00 00 00 0F 11 82 B9 00 00 00 66 C7 82 "
     "C9 00 00 00 01 00 48 8B 0D ? ? ? ? 48 8B 01 48 8B 40 08 BA 18 00 00 00 FF 15 ? "
     "? ? ? 90 48 89 C7 48 85 C0 0F 84 ? ? ? ? 48 8B 4D 68 48 8D 81 D0 00 00 00 48 "
     "89 45 B0 4C 89 77 08 4C 8D 3D ? ? ? ? 4C 89 3F 48 89 F8 48 83 C0 10 C6 47 10 01 "
     "48 89 81 D0 00 00 00 48 89 B9 D8 00 00 00 C6 81 E0 00 00 00 00 C7 81 E8 00 00 00 00 "
     "00 00 00 0F 57 C0 0F 11 81 F0 00 00 00 48 8B 0D ? ? ? ? 48 8B 01 48 8B 40 08 BA "
     "28 00 00 00 FF 15 ? ? ? ? 90 48 89 C7 48 85 C0 0F 84 ? ? ? ? 48 8B 45 68 48 "
     "8D 88 F0 00 00 00 48 89 4D 70 48 89 3F 48 89 7F 08 48 89 B8 F0 00 00 00 48 8D 88 00 "
     "01 00 00 48 89 4D 60 0F 57 C0 0F 11 80 00 01 00 00 48 C7 80 10 01 00 00 00 00 00 00 "
     "48 C7 80 18 01 00 00 07 00 00 00 48 C7 80 20 01 00 00 08 00 00 00 C7 80 E8 00 00 00 "
     "00 00 80 3F 48 8B 0D ? ? ? ? 48 8B 01 48 8B 40 08 BA 80 00 00 00 FF 15 ? ? ? "
     "? 90 48 89 C3 48 85 C0 0F 84 ? ? ? ? 4C 8B 65 68 49 8B 8C 24 00 01 00 00 49 8B "
     "94 24 10 01 00 00 48 29 CA 74 ? 48 81 FA 00 10 00 00 72 ? 48 8B 41 F8 48 83 C1 F8 "
     "48 29 C1 48 83 F9 20 0F 83 ? ? ? ? 48 83 C2 27 48 89 C1 E8 ? ? ? ? 49 8D 84 "
     "24 E8 00 00 00 48 89 45 B8 49 89 9C 24 00 01 00 00 48 89 D8 48 83 E8 80 49 89 84 24 "
     "08 01 00 00 49 89 84 24 10 01 00 00 48 89 3B 48 89 7B 08 48 89 7B 10 48 89 7B 18 48 "
     "89 7B 20 48 89 7B 28 48 89 7B 30 48 89 7B 38 48 89 7B 40 48 89 7B 48 48 89 7B 50 48 "
     "89 7B 58 48 89 7B 60 48 89 7B 68 48 89 7B 70 48 89 7B 78 41 C7 84 24 28 01 00 00 00 "
     "00 00 00 0F 57 C0 41 0F 11 84 24 30 01 00 00 48 8B 0D ? ? ? ? 48 8B 01 48 8B 40 "
     "08 BA 10 01 00 00 FF 15 ? ? ? ? 90 48 89 C7 48 85 C0 0F 84 ? ? ? ? 48 8B 45 "
     "68 48 8D 88 30 01 00 00 48 89 4D 70 48 89 3F 48 89 7F 08 48 89 B8 30 01 00 00 48 8D "
     "88 40 01 00 00 48 89 4D 60 0F 57 C0 0F 11 80 40 01 00 00 48 C7 80 50 01 00 00 00 00 "
     "00 00 48 C7 80 58 01 00 00 07 00 00 00 48 C7 80 60 01 00 00 08 00 00 00 C7 80 28 01 "
     "00 00 00 00 80 3F 48 8B 0D ? ? ? ? 48 8B 01 48 8B 40 08 BA 80 00 00 00 FF 15 ? "
     "? ? ? 90 48 89 C3 48 85 C0 0F 84 ? ? ? ? 4C 8B 65 68 49 8B 8C 24 40 01 00 00 "
     "49 8B 94 24 50 01 00 00 48 29 CA 74 ? 48 81 FA 00 10 00 00 72 ? 48 8B 41 F8 48 83 "
     "C1 F8 48 29 C1 48 83 F9 20 0F 83 ? ? ? ? 48 83 C2 27 48 89 C1 E8 ? ? ? ? 49 "
     "89 9C 24 40 01 00 00 48 89 D8 48 83 E8 80 49 89 84 24 48 01 00 00 49 89 84 24 50 01 "
     "00 00 48 89 3B 48 89 7B 08 48 89 7B 10 48 89 7B 18 48 89 7B 20 48 89 7B 28 48 89 7B "
     "30 48 89 7B 38 48 89 7B 40 48 89 7B 48 48 89 7B 50 48 89 7B 58 48 89 7B 60 48 89 7B "
     "68 48 89 7B 70 48 89 7B 78 0F 57 C0 41 0F 11 84 24 68 01 00 00 49 C7 84 24 78 01 00 "
     "00 00 00 00 00 48 8B 46 68 49 89 84 24 70 01 00 00 48 8B 46 70 49 89 84 24 78 01 00 "
     "00 0F 11 46 68 48 8B 46 78 49 89 84 24 80 01 00 00 48 C7 46 78 00 00 00 00 41 0F 11 "
     "84 24 88 01 00 00 48 8B 86 68 01 00 00 48 85 C0 74 ? F0 FF 40 08 48 8B 86 68 01 00 "
     "00 EB ? 31 C0 48 8B 8E 60 01 00 00 49 89 8C 24 88 01 00 00 49 89 84 24 90 01 00 00 "
     "48 8B 86 70 01 00 00 49 89 84 24 98 01 00 00 48 85 C9 0F 84 ? ? ? ? 80 39 00 0F "
     "84 ? ? ? ? 48 8B 86 90 00 00 00 49 89 84 24 B0 01 00 00 0F 10 86 80 00 00 00 41 "
     "0F 11 84 24 A0 01 00 00 0F 57 C0 41 0F 11 84 24 B8 01 00 00 48 8B 86 A0 00 00 00 48 "
     "85 C0 74 ? F0 FF 40 08 48 8B 86 A0 00 00 00 EB ? 31 C0 48 8B 8E 98 00 00 00 49 89 "
     "8C 24 B8 01 00 00 49 89 84 24 C0 01 00 00 48 8B 86 A8 00 00 00 49 89 84 24 C8 01 00 "
     "00 48 85 C9 0F 84 ? ? ? ? 80 39 00 0F 84 ? ? ? ? 48 8B 86 B0 00 00 00 49 89 "
     "84 24 D0 01 00 00 48 8B 86 B8 00 00 00 48 C7 86 B8 00 00 00 00 00 00 00 49 89 84 24 "
     "D8 01 00 00 0F 57 F6 41 0F 11 B4 24 E0 01 00 00 48 8B 86 C0 00 00 00 48 85 C0 74 ? "
     "49 8D 94 24 E0 01 00 00 48 8D BE C0 00 00 00 48 8B 00 48 89 F9 FF 15 ? ? ? ? 48 "
     "8B 07 EB ? 31 C0 48 C7 86 C0 00 00 00 00 00 00 00 49 89 84 24 E0 01 00 00 41 0F 11 "
     "B4 24 20 02 00 00 48 8B 86 00 01 00 00 48 85 C0 74 ? 49 8D 94 24 20 02 00 00 48 8D "
     "BE 00 01 00 00 48 8B 00 48 89 F9 FF 15 ? ? ? ? 48 8B 07 EB ? 31 C0 49 8D 8C 24 "
     "28 01 00 00 48 89 4D C0 49 8D 8C 24 68 01 00 00 48 89 4D 60 49 8D 8C 24 70 01 00 00 "
     "48 89 4D C8 49 8D 8C 24 88 01 00 00 48 89 4D D0 49 8D 8C 24 A0 01 00 00 48 89 4D D8 "
     "48 C7 86 00 01 00 00 00 00 00 00 49 89 84 24 20 02 00 00 49 8D 84 24 60 02 00 00 48 "
     "89 45 E0 0F 57 C0 41 0F 11 84 24 60 02 00 00 48 8B 86 40 01 00 00 49 89 84 24 60 02 "
     "00 00 48 8B 86 48 01 00 00 49 89 84 24 68 02 00 00 0F 11 86 40 01 00 00 49 8D 84 24 "
     "70 02 00 00 48 89 45 E8 41 0F 11 84 24 70 02 00 00 48 8B 86 50 01 00 00 49 89 84 24 "
     "70 02 00 00 48 8B 86 58 01 00 00 49 89 84 24 78 02 00 00 0F 11 86 50 01 00 00 49 8D "
     "84 24 80 02 00 00 48 89 45 F0 41 0F 11 84 24 80 02 00 00 48 8B 86 78 01 00 00 49 89 "
     "84 24 80 02 00 00 48 8B 86 80 01 00 00 49 89 84 24 88 02 00 00 0F 11 86 78 01 00 00 "
     "48 8B 86 88 01 00 00 49 89 84 24 90 02 00 00 48 C7 86 88 01 00 00 00 00 00 00 4C 8B "
     "25 ? ? ? ? 4D 85 E4 75 ? 48 8B 0D ? ? ? ? 48 8B 01 48 8B 40 08 BA 70 00 00 "
     "00 FF 15 ? ? ? ? 90 48 89 C7 48 85 C0 0F 84 ? ? ? ? 48 8D 05 ? ? ? ? 48 "
     "89 45 30 48 C7 45 38 0B 00 00 00 48 8D 55 30 48 89 F9 66 41 B8 3C 00 48 89 7D 70 E8 "
     "? ? ? ? 90 48 8B 0D ? ? ? ? 4C 8B 65 70 4C 89 25 ? ? ? ? 48 85 C9 74 ? "
     "48 8B 01 48 8B 00 BA 01 00 00 00 FF 15 ? ? ? ? 4C 8B 25 ? ? ? ? 48 8B 05 ? "
     "? ? ? 48 85 C0 0F 84 ? ? ? ? 80 38 00 75 ? C7 04 25 00 00 00 00 DE C0 AD DE "
     "48 8B 3D ? ? ? ? 48 8B 0D ? ? ? ? 48 8B 01 48 8B 40 08 BA 50 01 00 00 FF 15 "
     "? ? ? ? 90 48 89 C3 48 85 C0 0F 84 ? ? ? ? 48 89 5D 70 0F 57 C0 0F 29 45 40 "
     "0F 29 45 30 48 8B 0D ? ? ? ? 48 8B 01 48 8B 40 08 BA 20 00 00 00 FF 15 ? ? ? "
     "? 90 48 89 C3 48 85 C0 0F 84 ? ? ? ? 48 89 5D 30 48 C7 45 40 1D 00 00 00 48 C7 "
     "45 48 1F 00 00 00 0F 10 05 ? ? ? ? 0F 11 43 0D 0F 10 05 ? ? ? ? 0F 11 03 C6 "
     "43 1D 00 4C 8D 4D 30 48 8B 4D 70 48 89 FA 4D 89 E0 E8 ? ? ? ? 90 48 8B 45 60 48 "
     "8B 08 48 8B 55 70 48 89 10 48 85 C9 74 ? 48 8B 01 48 8B 00 BA 01 00 00 00 FF 15 ? "
     "? ? ? 48 8D 1D ? ? ? ? 48 89 D9 E8 ? ? ? ? F0 48 FF 05 ? ? ? ? 48 89 "
     "D9 E8 ? ? ? ? 48 8B 05 ? ? ? ? 48 85 C0 4C 8B 65 68 74 ? 80 38 01 75 ? 48 "
     "8B 0D ? ? ? ? 48 85 C9 0F 84 ? ? ? ? F0 FF 41 08 48 8B 05 ? ? ? ? 48 8B "
     "3D ? ? ? ? E9 ? ? ? ? 48 C7 45 30 00 00 00 00 C6 45 38 00 48 8D 0D ? ? ? "
     "? E8 ? ? ? ? 80 7D 38 01 75 ? 48 8B 4D 30 E8 ? ? ? ? 48 8D 05 ? ? ? ? "
     "49 89 04 24 48 8D 05 ? ? ? ? 49 89 84 24 98 02 00 00"},
    {Site::TerrainHandlerTick, L"TerrainHandlerTick", L"the game's chunk receiver tick (Bot walk path for far bots)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC D8 00 00 00 48 8D AC 24 80 00 00 00 48 C7 45 50 FE FF FF FF 48 89 CE E8 ? ? ? ? 48 89 C7 E8"},
    {Site::TerrainLevelDimension, L"TerrainLevelDimension", L"the game's getOrCreateDimension (Bot walk path for far bots)",
     "48 8B 81 20 03 00 00 48 8B 48 18 E9 ? ? ? ? 48 8B 81 20 03 00 00 48 8B 48 18 E9 ? ? "
     "? ? 41 57"},

    {Site::TerrainPrepareRegion, L"TerrainPrepareRegion", L"the player's chunk view tracker setup (Bot walk path for far bots)",
     "56 57 48 83 EC 28 48 89 D7 48 89 CE E8 ? ? ? ? 48 8D 4E 08 E8 ? ? ? ? 48 8B "
     "00 48 89 44 24 20 48 8D 8E 10 11 00 00 4C 8D 44 24 20 48 89 FA E8 ? ? ? ? E8 ? "
     "? ? ? 48 89 C7"},
    {Site::TerrainTrackerMove, L"TerrainTrackerMove", L"the chunk view tracker move (Bot walk path for far bots)",
     "56 57 55 53 48 81 EC A8 00 00 00 0F 29 B4 24 90 00 00 00 48 8B 05 ? ? ? ? 48 31 E0 48 89 84 24 88 00 00 00 48 8B 41 18 48 85 C0 0F 84 ? ? ? ? 44 89 C3 48 89 D7 48 89 CE F3 0F 10 B4 24 F0 00 00 00 8B 0A C1 F9 04"},

    {Site::TerrainDestroyRegion, L"TerrainDestroyRegion", L"the player's chunk view tracker teardown (Bot walk path for far bots)",
     "56 57 53 48 83 EC 20 48 89 CE 48 8B B9 40 0F 00 00 48 8B 99 48 0F 00 00 48 39 DF 75 "
     "? EB ? 90 48 83 C7 08 48 39 DF 74 ? 48 8B 0F 48 85 C9 74 ? 48 8B 01 48 8B 40 08 "
     "FF 15 ? ? ? ? EB ? 48 8B 86 40 0F 00 00 48 3B 86 48 0F 00 00 74 ? 48 89 86 48 "
     "0F 00 00 48 8B 86 28 11 00 00 48 85 C0 74 ? 48 8B 40 28 48 8B 88 B0 03 00 00 8B 96 "
     "1C 11 00 00 48 8B 01 48 8B 40 18 FF 15 ? ? ? ? 48 8B 8E 28 11 00 00"},
    {Site::TerrainJobEnqueue, L"TerrainJobEnqueue", L"the sub-chunk job queue push (Bot walk path for far bots)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 83 EC 68 48 8D 6C 24 60 48 C7 45 00 FE FF FF "
     "FF 48 89 D3 49 89 CE 48 8B 42 08 48 85 C0"},

    {Site::TerrainJobConsume, L"TerrainJobConsume", L"the sub-chunk job queue consumer (Bot walk path for far bots)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 18 03 00 00 48 8D AC 24 80 00 00 00 44 "
     "0F 29 85 80 02 00 00 0F 29 BD 70 02 00 00 0F 29 B5 60 02 00 00 48 C7 85 58 02 00 00 "
     "FE FF FF FF 49 89 CE 48 8B 89 B8 01 00 00 48 85 C9 0F 84 ? ? ? ?"},
    {Site::TerrainJobFinish, L"TerrainJobFinish", L"the sub-chunk job finisher (Bot walk path for far bots)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 08 01 00 00 48 8D AC 24 80 00 00 00 0F 29 75 70 48 C7 45 68 FE FF FF FF 48 8B 79 08 48 8B 1F 48 8D 35 ? ? ? ? 48 89 F1 E8 ? ? ? ? F0 48 FF 05 ? ? ? ? 48 89 F1"},

    {Site::ActorGetInteraction, L"ActorGetInteraction", L"the actor's right-click interaction (player inventory view)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC D8 00 00 00 48 8D AC 24 80 00 00 00 0F "
     "29 75 40 48 C7 45 38 FE FF FF FF 4C 89 C7 48 89 D6 48 89 CB 44 0F B6 AD C0 00 00 00 "
     "48 8B 89 00 02 00 00"},
    {Site::ActorInteractionSite, L"ActorInteractionSite", L"the open-container interaction (player inventory view)",
     "48 8D 05 ? ? ? ? 48 B9 FF FF FF FF FF FF 00 FF 48 21 C1 48 B8 00 00 00 00 00 00 1D 00 48 09 C8 48 89 06 48 89 5E 10 48 89 7E 18 48 8D 05 ? ? ? ? 48 89 46 08 80 4E 48 01"},

    {Site::StackDescriptorCtorSite, L"StackDescriptorCtorSite", L"the game's item descriptor from an item stack (player inventory view)",
     "48 89 D6 48 89 CB 0F B6 7A 22 48 8D 55 E0 48 89 F1 E8 ? ? ? ? 48 8D 05 ? ? ? ? 48 89 "
     "03 48 8B 4D E8 48 85 C9 74 ? 48 8D 53 08 48 8B 01 48 8B 00 48 89 4D F8 FF 15 ? ? ? ? "
     "90 48 8D 05 ? ? ? ? 48 89 03 66 89 7B 10 48 8B 4D F8 48 8B 01 48 8B 80 88 00 00 00 "
     "BA 01 00 00 00 FF 15 ? ? ? ? EB ? 48 C7 43 08 00 00 00 00 48 8D 05 ? ? ? ? 48 89 03 "
     "66 89 7B 10 48 8D 05 ? ? ? ? 48 89 03 C6 43 18 00 48 8D 43 20 C7 43 20 00 00 00 00"},
    {Site::ContainerAtPosition, L"ContainerAtPosition", L"container at a block position (inventory view without blocks)",
     "41 56 56 57 53 48 83 EC 28 80 FA 17 75 ? 48 8B 81 ? ? 00 00 E9 ? ? ? ? 4C 89 C6 89 D3 48 8B 91 ? ? 00 00"},
    {Site::LevelContainerInRange, L"LevelContainerInRange", L"level container model range check (inventory view without blocks)",
     "56 57 53 48 81 EC 80 00 00 00 44 0F 29 54 24 70 44 0F 29 4C 24 60 44 0F 29 44 24 50 0F 29 7C 24 40 0F 29 74 24 30 0F 28 F1 "
     "48 89 CE 48 8B B9 ? ? 00 00 48 89 FB 66 2E 0F 1F 84 00 00 00 00 00 48 8B 1B 48 39 FB 74 3E 48 8B 4B 30 48 8B 01 48 8B 40 70 "
     "FF 15 ? ? ? ? 84 C0 75 E3 31 C0"},

    {Site::ContainerScreenOpenSite, L"ContainerScreenOpenSite", L"scene stack and factory of the container-open handler (inventory view without blocks)",
     "48 8B 8E 98 0D 00 00 48 8B 01 48 8B 80 60 07 00 00 48 8D 95 98 00 00 00 FF 15 ? ? ? ? 48 8B "
     "8E 98 0D 00 00 48 8B 01 48 8B 80 18 07 00 00 FF 15 ? ? ? ? 90 48 83 BE C8 05 00 00 00 0F 85 "
     "? ? ? ? 48 89 C3"},

    {Site::ContainerScreenCreateSite, L"ContainerScreenCreateSite", L"create and push of the chest screen (inventory view without blocks)",
     "4C 8B 7F 28 49 83 FF FF 0F 84 ? ? ? ? 48 8B 85 98 00 00 00 48 85 C0 75 ? C7 04 25 00 00 00 "
     "00 DE C0 AD DE 80 38 00 75 ? C7 04 25 00 00 00 00 DE C0 AD DE 48 8B BD A8 00 00 00 48 8D 15 ? "
     "? ? ? 48 8D 4D 68 E8 ? ? ? ? 90 4C 89 7C 24 28 4C 89 74 24 20 48 8D 55 B8 4C 8D 45 68 48 "
     "89 D9 49 89 F1 E8 ? ? ? ? 90 48 8B 07 48 8B 80 C8 00 00 00"},

    {Site::SimpleContainerCtorSite, L"SimpleContainerCtorSite", L"SimpleContainer construction (inventory view without blocks)",
     "48 89 D9 B2 13 45 31 C9 48 89 5D E0 E8 ? ? ? ? 90 48 8D 1D ? ? ? ? 48 8B 45 E0 48 89 18 "
     "C7 80 98 01 00 00 02 00 00 00 49 81 C6 A0 01 00 00 4C 89 75 D8 0F 11 B0 A0 01 00 00 48 C7 80 B0 "
     "01 00 00 00 00 00 00 BA 02 00 00 00 4C 89 F1 E8 ? ? ? ?"},

    {Site::LevelContainerModelCtorSite, L"LevelContainerModelCtorSite", L"LevelContainerManagerModel constructor (inventory view without blocks)",
     "48 8B 11 49 89 90 98 01 00 00 8B 49 08 41 89 88 A0 01 00 00 49 C7 80 A8 01 00 00 FF FF FF FF 41 "
     "88 80 B0 01 00 00 48 8D 0D ? ? ? ? 49 89 48 10"},
    {Site::LevelContainerWeakRef, L"LevelContainerWeakRef", L"level container model weak reference (inventory view without blocks)",
     "56 57 48 83 EC 28 48 89 D6 48 89 CF 48 8B 91 ? ? 00 00 48 83 FA FF 0F 84 ? ? ? ? 48 8B 87 ? ? 00 00 48 8B 88 ? ? 00 00 48 8B 01 48 8B"},
    {Site::LevelContainerWeakRefSite, L"LevelContainerWeakRefSite", L"fields of the level container weak reference (inventory view without blocks)",
     "48 C7 06 FF FF FF FF C7 46 08 00 00 00 00 48 8B 8F ? ? 00 00 48 89 4E ? 8B 8F ? ? 00 00 89 4E ? 8B 80 ? ? 00 00 89 46 ? 0F 57 C0 0F 11 46 ? EB"},

    {Site::ContainerNameAt, L"ContainerNameAt", L"container name at a block position (inventory view title)",
     "55 41 57 41 56 41 54 56 57 53 48 83 EC 70 48 8D 6C 24 70 48 C7 45 F8 FE FF FF FF 4C "
     "89 C7 48 89 D6 48 89 CB 48 8B 49 30 48 8B 91 F8 01 00 00 8B 42 08"},

    {Site::ContainerCustomNameAt, L"ContainerCustomNameAt", L"container custom name flag at a block position (inventory view title)",
     "55 56 57 53 48 83 EC 48 48 8D 6C 24 40 48 C7 45 00 FE FF FF FF 48 89 D6 0F 57 C0 0F "
     "29 45 E0 48 8B 51 20 48 85 D2 0F 84 ? ? ? ?"},
    {Site::MinecraftGameUpdate, L"MinecraftGameUpdate", L"MinecraftGame::_update (bots leave with the main player outside worlds)",
     "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 28 04 00 00 48 8D AC 24 80 00 00 00 66 "
     "0F 29 BD 90 03 00 00 66 0F 29 B5 80 03 00 00 48 C7 85 78 03 00 00 FE FF FF FF"},
    {Site::FriendWorldJoinHandler, L"FriendWorldJoinHandler", L"leaveGameThenJoinFriendsWorld (switching in a friend's world)",
     "56 57 48 83 EC 48 48 89 D6 48 89 CF 48 8B 05 ? ? ? ? 48 31 E0 48 89 44 24 40 48 8B 89 ? ? ? ? 48 8B 01 48 8B 80 ? ? ? ? FF 15 ? ? ? ? "
     "48 8B 8F ? ? ? ? 84 C0 4C 8D 44 24 30 48 8D 54 24 20 49 0F 45 D0 0F B6 C0 C1 E0 04 4C 8B 46 10 48 83 7E 18 10 72 03 48 8B 36 "
     "48 89 32 4C 89 42 08 4C 8B 01 4A 8B 44 00 ?"},
    {Site::FriendWorldJoinerCtor, L"FriendWorldJoinerCtor", L"the game's friend world joiner (switching in a friend's world)",
     "48 8B 42 10 48 83 C0 28 49 89 82 88 00 00 00 49 8D 8A 90 00 00 00 48 8D 15 ? ? ? ? 4C 8D 0D ? ? ? ? 48 89 4D E8"},
    {Site::FriendJoinRequestHandle, L"FriendJoinRequestHandle", L"the game's friend world list (switching in a friend's world)",
     "48 8B 03 48 8B 80 C0 00 00 00 48 89 D9 FF 15 ? ? ? ? 89 C1 B8 03 00 00 00 84 C9 0F 85 ? ? ? ? 48 8B 03 48 8B 80 C0 04 00 00 48 89 D9 FF 15 ? ? ? ? 48 8B B0 80 00 00 00 48 8D 05 ? ? ? ? 48 89 45 F0 48 89 5D F8"},
    {Site::FriendWorldFind, L"FriendWorldFind", L"the game's friend world list (switching in a friend's world)",
     "55 56 57 53 48 83 EC 78 48 8D 6C 24 70 48 C7 45 00 FE FF FF FF 4C 89 45 F8 48 89 CE 48 8B 89 ? ? ? ? E8 ? ? ? ? 48 85 C0 74"},
    {Site::ClientDataBuild, L"ClientDataBuild", L"the game's Login clientData (switching to a bot in a friend's world)",
     "55 41 56 56 57 53 48 81 EC C0 01 00 00 48 8D AC 24 80 00 00 00 48 C7 85 38 01 00 00 "
     "FE FF FF FF 4C 89 CB 4C 89 C7 48 89 95 20 01 00 00"},
    {Site::ClientDataNonce, L"ClientDataNonce", L"the game's Login clientData nonce (switching to a bot in a friend's world)",
     "48 8B 95 38 02 00 00 80 7A 20 01 75 ? 48 8D 8D 98 00 00 00 E8 ? ? ? ? 90 48 8D 15 ? "
     "? ? ?"},
};

static_assert(std::size(kSites) == static_cast<std::size_t>(Site::Count));

}
