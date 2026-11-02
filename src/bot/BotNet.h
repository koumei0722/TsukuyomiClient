#pragma once
#include "BotSignatures.h"
#include "Possess.h"
#include "Relog.h"
#include "SkinSteal.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tsukuyomi::bot::net {
using ReceiveFn = int(__fastcall*)(void* peer, void* out, void* context);
inline constexpr std::uint8_t kSubCount = 4;
struct Received { std::uint16_t id; std::uint8_t recipient; std::string body; };
struct Incoming {
    std::vector<Received> packets;
    std::array<std::uint64_t, kSubCount> perSub{};
    std::uint64_t dropped = 0;
};
bool initialize();
std::byte* site(Site);
void* mainPeer();
void* receiveTarget();
void hooksReady(bool receive, bool send);
bool available();
void observeSender(void* sender);
void activate(std::uint8_t sub);
void deactivate(std::uint8_t sub);
void deactivateAll();
int receive(ReceiveFn original, void* peer, void* out, void* context);
Incoming takeIncoming();
bool connected();
bool sendRaw(const std::string&);
using SendInternalFn = void(__fastcall*)(void* system, void* id, void* packet, const std::string* data);
using SendFilter = bool (*)(std::uint16_t id, protocol::Bytes body);
void setSendFilter(SendFilter filter);
void* sendInternalTarget();
void sendInternal(SendInternalFn original, void* system, void* id, void* packet, const std::string* data);
void schedule(std::string bytes, std::uint64_t delayMs);
std::optional<possess::StartGameHead> mainStartGame();
std::optional<possess::AuthInputHead> lastGameInput();
std::int32_t gameRadius();
void setKeeperSelf(std::uint8_t sub, std::uint64_t runtimeId);
std::optional<possess::Uuid> playerUuid(std::uint8_t sub, std::uint64_t runtimeId);
std::optional<std::string> playerName(std::uint8_t sub, std::uint64_t runtimeId);
std::optional<possess::Uuid> listedUuid(std::int64_t uniqueId);
std::optional<skin::ListedPlayer> listedSkin(const std::string& name);
std::vector<std::string> listedSkinNames();
std::vector<std::string> keeperChunksOf(std::uint8_t sub, std::int32_t chunkX, std::int32_t chunkZ, std::int32_t dimension, std::int32_t radius);
possess::Ids currentIds();
std::optional<relog::ServerAddress> serverAddress();
void armRelog(bool toBot, std::optional<std::pair<possess::Uuid, possess::Uuid>> uuids = std::nullopt, std::uint8_t view = 1);
void clearRouting();
bool relogRouted();
std::string gameLoginKey();
std::string userRequest();
std::string sentRequest();
std::uint64_t loginCount();
void armMainSwap(std::string token, std::string key, const possess::Uuid& user, const possess::Uuid& bot, std::string nonce = {});
void disarmMainSwap();
std::optional<std::string> armedLoginNonce();
bool mainSwapped();
std::string takeMainSwapError();
void setPump(void (*pump)());
void setPumpWanted(bool wanted);
bool timedPending();
std::vector<std::uint64_t> gameCachedBlobs();
void setTerrainRoute(int sub, bool copyMisses);
std::vector<std::string> takeTerrainPackets();
void* mainConnection();
void* networkSystem();
std::uint32_t connectionPeerOffset();
std::uint32_t peerSendSlot();
std::uint32_t peerReceiveSlot();
void setReceiveOriginal(ReceiveFn);
ReceiveFn receiveOriginal();
bool assignToGameString(void* out, const std::string& bytes);
}
