#include "GuestLogin.h"
#include "LoginRequest.h"
#include "Base64.h"

namespace tsukuyomi::bot::guest {
namespace {
template<class SignData>
std::optional<std::string> build(nlohmann::json login, nlohmann::json data, const SignData& sign)
{
    if (!login.is_object() || !data.is_object()) return std::nullopt;
    const auto token = login.find("Token");
    if (token == login.end() || !token->is_string()) return std::nullopt;
    const auto claims = jwt::readPayload(token->get_ref<const std::string&>());
    if (!claims) return std::nullopt;
    const auto primary = claims->find("mid");
    if (primary == claims->end() || !primary->is_string() || primary->get_ref<const std::string&>().empty()) return std::nullopt;
    login["AuthenticationType"] = 1;
    data["PrimaryPlayFabId"] = *primary;
    const auto signedData = sign(data);
    if (signedData.empty()) return std::nullopt;
    const auto request = connectionRequest(login.dump(), signedData);
    if (request.empty()) return std::nullopt;
    return std::string(request.begin(), request.end());
}
}
std::optional<Login> fromGame(const std::string& gameRequest, const Sign& sign)
{
    if (!sign) return std::nullopt;
    const auto parts = splitConnectionRequest(gameRequest);
    if (!parts) return std::nullopt;
    auto login = nlohmann::json::parse(parts->first, nullptr, false);
    const auto data = jwt::readPayload(parts->second);
    const auto header = jwt::readHeader(parts->second);
    if (!data || !header || header->value("alg", nlohmann::json{}) != "ES384") return std::nullopt;
    const auto publicKey = header->find("x5u");
    if (publicKey == header->end() || !publicKey->is_string() || publicKey->get_ref<const std::string&>().empty()) return std::nullopt;
    auto built = build(std::move(login), *data, [&](const nlohmann::json& claims) {
        const auto content = base64::encodeUrl(header->dump()) + '.' + base64::encodeUrl(claims.dump());
        const auto signature = sign(publicKey->get_ref<const std::string&>(), content);
        return signature && signature->size() == 96 ? content + '.' + base64::encodeUrl(*signature) : std::string{};
    });
    if (!built) return std::nullopt;
    return Login(std::move(*built));
}
std::optional<Login> fromHost(const Host& host, const std::string& name)
{
    if (!host.known()) return std::nullopt;
    auto built = build(nlohmann::json{{"Token", host.token_}}, defaultClientData(name, {}, host.address_),
        [&](const nlohmann::json& claims) { return clientDataJwt(*host.key_, claims); });
    if (!built) return std::nullopt;
    return Login(std::move(*built));
}
}
