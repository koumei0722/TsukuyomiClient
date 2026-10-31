#pragma once
#include "Jwt.h"

namespace tsukuyomi::bot {
std::string clientDataJwt(const crypto::EcKey&, const nlohmann::json& claims);
std::string loginJson(const std::string& token);
std::vector<std::uint8_t> connectionRequest(const std::string& loginJson, const std::string& clientDataJwt);
std::optional<std::pair<std::string, std::string>> splitConnectionRequest(const std::string& request);
nlohmann::json defaultClientData(const std::string& gamertag, const std::string& playFabId, const std::string& serverAddress,
    const std::string& deviceId = {}, const std::string& selfSignedId = {});
std::string stableUuid(std::string_view seed);
}
