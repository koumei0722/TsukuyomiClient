#pragma once
#include "Http.h"
#include "XboxAuth.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace tsukuyomi::bot::mpsd {
struct SessionRef { std::string scid, templateName, name; };

std::optional<SessionRef> findByNetherNetId(const nlohmann::json& handles, std::uint64_t netherNetId);
std::optional<std::string> nonceFor(const nlohmann::json& session, const std::string& xuid);

struct ActiveMember { std::string connection, subscription; };
Result<std::string> joinAndGetNonce(IHttp&, const std::string& authorization, const std::string& xuid, std::uint64_t netherNetId,
    const std::function<bool(std::chrono::milliseconds)>& wait, const ActiveMember* active = nullptr);
Result<bool> leave(IHttp&, const std::string& authorization, const std::string& xuid, std::uint64_t netherNetId);
}
