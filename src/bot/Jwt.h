#pragma once
#include "Crypto.h"
#include <nlohmann/json.hpp>
#include <string_view>

namespace tsukuyomi::bot::jwt {
std::string signEs384(const crypto::EcKey&, const nlohmann::json& header, const nlohmann::json& payload);
std::optional<nlohmann::json> readHeader(std::string_view token);
std::optional<nlohmann::json> readPayload(std::string_view token);
}
