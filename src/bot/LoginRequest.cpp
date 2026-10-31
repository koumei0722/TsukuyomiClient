#include "LoginRequest.h"
#include "Base64.h"
#include "XboxAuth.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>

namespace tsukuyomi::bot {
namespace {
nlohmann::json header(const crypto::EcKey& key)
{
    return {{"alg", "ES384"}, {"x5u", base64::encode(key.spkiDer())}};
}
}
std::string clientDataJwt(const crypto::EcKey& key, const nlohmann::json& claims)
{
    return jwt::signEs384(key, header(key), claims);
}
std::string loginJson(const std::string& token)
{
    return nlohmann::json{{"AuthenticationType", 0}, {"Token", token}}.dump();
}
std::vector<std::uint8_t> connectionRequest(const std::string& login, const std::string& data)
{
    const auto max = static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)());
    if (login.size() > max || data.size() > max) return {};
    std::vector<std::uint8_t> result;
    result.reserve(8 + login.size() + data.size());
    for (const auto* text : {&login, &data}) {
        const auto size = static_cast<std::uint32_t>(text->size());
        for (int shift = 0; shift < 32; shift += 8) result.push_back(static_cast<std::uint8_t>(size >> shift));
        result.insert(result.end(), text->begin(), text->end());
    }
    return result;
}
std::optional<std::pair<std::string, std::string>> splitConnectionRequest(const std::string& request)
{
    std::size_t at = 0;
    const auto part = [&]() -> std::optional<std::string> {
        if (request.size() - at < 4) return std::nullopt;
        std::uint32_t size = 0;
        for (int i = 0; i < 4; ++i) size |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(request[at + i])) << (8 * i);
        at += 4;
        if (request.size() - at < size) return std::nullopt;
        auto text = request.substr(at, size);
        at += size;
        return text;
    };
    auto login = part();
    auto data = login ? part() : std::nullopt;
    if (!login || !data || at != request.size()) return std::nullopt;
    return std::pair{std::move(*login), std::move(*data)};
}
std::string stableUuid(std::string_view seed)
{
    auto bytes = crypto::sha256(std::span(reinterpret_cast<const std::uint8_t*>(seed.data()), seed.size()));
    if (bytes.size() < 16) return {};
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (std::size_t i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) result += '-';
        result += hex[bytes[i] >> 4]; result += hex[bytes[i] & 15];
    }
    return result;
}
nlohmann::json defaultClientData(const std::string& gamertag, const std::string& playFabId, const std::string& serverAddress,
    const std::string& givenDeviceId, const std::string& givenSelfId)
{
    std::string lowerId = playFabId;
    std::transform(lowerId.begin(), lowerId.end(), lowerId.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
    });
    const auto random = crypto::randomBytes(8);
    const auto selfId = givenSelfId.empty() ? crypto::newUuid() : givenSelfId, skinId = crypto::newUuid();
    const auto deviceId = givenDeviceId.empty() ? crypto::newUuid() : givenDeviceId;
    if (random.size() != 8 || selfId.empty() || skinId.empty() || deviceId.empty()) return {};
    std::uint64_t bits = 0;
    std::memcpy(&bits, random.data(), sizeof(bits));
    std::vector<std::uint8_t> skin(64 * 32 * 4, 0);
    for (std::size_t i = 3; i < skin.size(); i += 4) skin[i] = 255;
    return {
        {"AnimatedImageData", nlohmann::json::array()}, {"ArmSize", "wide"}, {"CapeData", ""}, {"CapeId", ""},
        {"CapeImageHeight", 0}, {"CapeImageWidth", 0}, {"CapeOnClassicSkin", false},
        {"ClientRandomId", std::bit_cast<std::int64_t>(bits)}, {"CurrentInputMode", 2}, {"DefaultInputMode", 2},
        {"DeviceModel", ""}, {"DeviceOS", 1}, {"DeviceId", deviceId}, {"GameVersion", kGameVersion}, {"GuiScale", -1},
        {"FilterProfanity", false}, {"ClientEditorConnectionIntent", 0}, {"ClientIsEditorCapable", false}, {"LanguageCode", "ja_JP"},
        {"PersonaSkin", false}, {"PlatformOfflineId", ""}, {"PlatformOnlineId", ""}, {"PremiumSkin", false},
        {"SelfSignedId", selfId}, {"ServerAddress", serverAddress}, {"SkinAnimationData", ""}, {"SkinData", base64::encode(skin)},
        {"SkinGeometryData", base64::encode("{}")}, {"SkinGeometryDataEngineVersion", base64::encode("0.0.0")},
        {"SkinId", skinId}, {"PlayFabId", lowerId}, {"SkinImageHeight", 32}, {"SkinImageWidth", 64},
        {"SkinResourcePatch", base64::encode(R"({"geometry":{"default":"geometry.humanoid.custom"}})")},
        {"SkinColor", "#0"}, {"PersonaPieces", nlohmann::json::array()}, {"PieceTintColors", nlohmann::json::array()},
        {"ThirdPartyName", gamertag}, {"UIProfile", 0}, {"TrustedSkin", false}, {"CompatibleWithClientSideChunkGen", true},
        {"PlatformType", 0}
    };
}
}
