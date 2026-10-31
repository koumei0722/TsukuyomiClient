#include "Relog.h"
#include "Jwt.h"
#include <charconv>

namespace tsukuyomi::bot::relog {
namespace {
std::optional<std::uint32_t> int32Le(protocol::Bytes data, std::size_t at)
{
    if (data.size() < at + 4) return std::nullopt;
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= std::uint32_t{data[at + i]} << (i * 8);
    return value;
}
}
std::string transfer(const ServerAddress& address, bool reloadWorld)
{
    std::string bytes;
    protocol::writeVarUInt32(bytes, protocol::header(0x55, 0, 0));
    if (!protocol::writeString(bytes, address.host)) return {};
    bytes.push_back(static_cast<char>(address.port & 0xff));
    bytes.push_back(static_cast<char>(address.port >> 8));
    bytes.push_back(reloadWorld ? 1 : 0);
    bytes.push_back(0);
    return bytes;
}
std::optional<ServerAddress> parseServerAddress(std::string_view text)
{
    if (const auto scheme = text.find("://"); scheme != std::string_view::npos) text.remove_prefix(scheme + 3);
    const auto colon = text.rfind(':');
    if (colon == std::string_view::npos || colon == 0) return std::nullopt;
    auto host = text.substr(0, colon);
    if (const auto first = host.find(':'); first != std::string_view::npos && host.substr(0, first).find('.') != std::string_view::npos)
        host = host.substr(0, first);
    const auto portText = text.substr(colon + 1);
    unsigned port = 0;
    const auto [end, error] = std::from_chars(portText.data(), portText.data() + portText.size(), port);
    if (error != std::errc{} || end != portText.data() + portText.size() || port == 0 || port > 0xffff) return std::nullopt;
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
    if (host.empty()) return std::nullopt;
    return ServerAddress{std::string(host), static_cast<std::uint16_t>(port)};
}
std::optional<LoginParts> splitLogin(protocol::Bytes body)
{
    if (body.size() < 4) return std::nullopt;
    LoginParts parts;
    parts.protocolVersion = static_cast<std::int32_t>(std::uint32_t{body[0]} << 24 | std::uint32_t{body[1]} << 16 | std::uint32_t{body[2]} << 8 | body[3]);
    protocol::Reader r(body.subspan(4));
    const auto length = r.varUInt32();
    if (!length) return std::nullopt;
    auto request = body.subspan(4 + r.position());
    if (request.size() < *length) return std::nullopt;
    request = request.first(*length);
    const auto loginLength = int32Le(request, 0);
    if (!loginLength || request.size() < 8 + std::size_t{*loginLength}) return std::nullopt;
    const auto dataAt = 4 + std::size_t{*loginLength};
    const auto dataLength = int32Le(request, dataAt);
    if (!dataLength || request.size() < dataAt + 4 + *dataLength) return std::nullopt;
    const auto* bytes = reinterpret_cast<const char*>(request.data());
    parts.request.assign(bytes, request.size());
    parts.loginJson.assign(bytes + 4, *loginLength);
    parts.clientData.assign(bytes + dataAt + 4, *dataLength);
    return parts;
}
std::string clientDataKey(const LoginParts& parts)
{
    const auto header = jwt::readHeader(parts.clientData);
    if (!header || !header->is_object()) return {};
    const auto found = header->find("x5u");
    return found != header->end() && found->is_string() ? found->get<std::string>() : std::string();
}
std::optional<std::string> replaceLoginToken(protocol::Bytes body, const std::string& token)
{
    const auto parts = splitLogin(body);
    if (!parts) return std::nullopt;
    auto login = nlohmann::json::parse(parts->loginJson, nullptr, false);
    if (!login.is_object() || !login.contains("Token") || !login["Token"].is_string()) return std::nullopt;
    login["Token"] = token;
    const auto json = login.dump();
    std::string request;
    for (const auto* text : {&json, &parts->clientData}) {
        const auto size = static_cast<std::uint32_t>(text->size());
        for (int shift = 0; shift < 32; shift += 8) request.push_back(static_cast<char>(size >> shift));
        request += *text;
    }
    std::string out;
    const auto version = static_cast<std::uint32_t>(parts->protocolVersion);
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<char>(version >> shift));
    protocol::writeVarUInt32(out, static_cast<std::uint32_t>(request.size()));
    out += request;
    return out;
}
std::optional<ServerAddress> readLoginServerAddress(protocol::Bytes body)
{
    const auto parts = splitLogin(body);
    if (!parts) return std::nullopt;
    const auto payload = jwt::readPayload(parts->clientData);
    if (!payload || !payload->is_object()) return std::nullopt;
    const auto found = payload->find("ServerAddress");
    if (found == payload->end() || !found->is_string()) return std::nullopt;
    return parseServerAddress(found->get<std::string>());
}
}
