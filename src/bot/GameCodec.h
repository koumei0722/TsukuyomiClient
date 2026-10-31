#pragma once
#include "BotCodec.h"
#include <functional>

namespace tsukuyomi::bot::gamecodec {

bool initialize();
BotCodec* instance();

bool inventoryViewAvailable();
std::string containerCloseToGame(std::uint8_t window, std::uint8_t type, bool serverInitiated);
std::string inventorySlotToGame(std::uint8_t window, std::uint32_t slot, const Item& item);
Item itemFromStack(const void* stack);
std::string stackFailureToGame(std::span<const std::int32_t> ids);
std::string cursorRequest(std::uint8_t sub, std::int32_t requestId, std::uint8_t count, std::int32_t cursorNetId,
                          std::optional<std::pair<std::uint8_t, std::uint8_t>> destination);
using SlotMap = std::function<bool(std::uint8_t& container, std::uint8_t& slot)>;
struct ResponseSlot { std::uint8_t container, slot, count; std::int32_t netId; };
std::optional<std::string> translateStackRequest(protocol::Bytes body, std::uint8_t sub, const SlotMap& map,
                                               std::vector<std::int32_t>& ids);
std::optional<std::string> translateStackResponseToGame(protocol::Bytes body, const SlotMap& map,
                                                       std::vector<ResponseSlot>& seen);

std::string disconnectToGame(const std::string& message);
std::string updateSoftEnumToGame(const std::string& name, const std::vector<std::string>& values, std::uint8_t action);
std::string levelChunkToProxy(protocol::Bytes body, std::optional<std::uint64_t> blob);

std::string requestNetworkSettings(std::int32_t protocol);
std::string login(std::int32_t protocol, std::string_view request);
std::string clientToServerHandshake();
std::string resourcePackResponse(std::uint8_t response);
struct NetworkSettings { std::uint16_t threshold = 0, algorithm = 0; };
std::optional<NetworkSettings> readNetworkSettings(protocol::Bytes body);

}
