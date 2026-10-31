#include "Jwt.h"
#include "Base64.h"

namespace tsukuyomi::bot::jwt {
namespace {
std::optional<nlohmann::json> readPart(std::string_view token, bool payload)
{
    const auto first = token.find('.');
    const auto second = first == std::string_view::npos ? first : token.find('.', first + 1);
    if (first == std::string_view::npos || second == std::string_view::npos
        || token.find('.', second + 1) != std::string_view::npos) return std::nullopt;
    const auto bytes = base64::decodeUrl(payload ? token.substr(first + 1, second - first - 1) : token.substr(0, first));
    if (!bytes) return std::nullopt;
    auto parsed = nlohmann::json::parse(bytes->begin(), bytes->end(), nullptr, false);
    if (!parsed.is_object()) return std::nullopt;
    return parsed;
}
}
std::string signEs384(const crypto::EcKey& key, const nlohmann::json& header, const nlohmann::json& payload)
{
    try {
        if (key.spkiDer().empty()) return {};
        const auto content = base64::encodeUrl(header.dump()) + '.' + base64::encodeUrl(payload.dump());
        const auto digest = crypto::sha384({reinterpret_cast<const std::uint8_t*>(content.data()), content.size()});
        const auto signature = key.signDigest(digest);
        if (signature.size() != 96) return {};
        return content + '.' + base64::encodeUrl(signature);
    } catch (const nlohmann::json::exception&) { return {}; }
}
std::optional<nlohmann::json> readHeader(std::string_view token) { return readPart(token, false); }
std::optional<nlohmann::json> readPayload(std::string_view token) { return readPart(token, true); }
}
