#include "Mpsd.h"

namespace tsukuyomi::bot::mpsd {
namespace {
using Json = nlohmann::json;
constexpr char kMinecraftScid[] = "4fc10100-5f7a-4470-899b-280835760c07";
constexpr char kSessionDirectory[] = "https://sessiondirectory.xboxlive.com";
constexpr int kNoncePolls = 45;
constexpr auto kNonceInterval = std::chrono::milliseconds(2000);

std::vector<std::pair<std::string, std::string>> headers(const std::string& authorization, bool body)
{
    std::vector<std::pair<std::string, std::string>> result{{"Authorization", authorization}, {"x-xbl-contract-version", "107"},
        {"Accept", "application/json"}, {"Accept-Language", "en-US"}};
    if (body) result.emplace_back("Content-Type", "application/json");
    return result;
}
std::string failure(const HttpResponse& response, const char* stage)
{
    std::string text = "HTTP " + std::to_string(response.status) + " at " + stage;
    if (!response.error.empty()) text += ": " + response.error;
    return text;
}
std::string sessionUrl(const SessionRef& ref)
{
    return std::string(kSessionDirectory) + "/serviceconfigs/" + ref.scid + "/sessionTemplates/" + ref.templateName + "/sessions/" + ref.name;
}
std::optional<std::uint64_t> readId(const Json& value)
{
    if (value.is_number_unsigned()) return value.get<std::uint64_t>();
    if (value.is_number_integer()) return static_cast<std::uint64_t>(value.get<std::int64_t>());
    if (value.is_string()) {
        try { return std::stoull(value.get<std::string>()); } catch (...) { return std::nullopt; }
    }
    return std::nullopt;
}
}

std::optional<SessionRef> findByNetherNetId(const Json& handles, std::uint64_t netherNetId)
{
    const auto results = handles.find("results");
    if (results == handles.end() || !results->is_array()) return std::nullopt;
    for (const auto& handle : *results) {
        if (!handle.is_object()) continue;
        const auto custom = handle.find("customProperties");
        const auto ref = handle.find("sessionRef");
        if (custom == handle.end() || !custom->is_object() || ref == handle.end() || !ref->is_object()) continue;
        const auto connections = custom->find("SupportedConnections");
        if (connections == custom->end() || !connections->is_array()) continue;
        for (const auto& connection : *connections) {
            if (!connection.is_object() || !connection.contains("NetherNetId")) continue;
            const auto id = readId(connection["NetherNetId"]);
            if (!id || *id != netherNetId) continue;
            SessionRef found{ref->value("scid", std::string()), ref->value("templateName", std::string()), ref->value("name", std::string())};
            if (found.scid.empty() || found.templateName.empty() || found.name.empty()) return std::nullopt;
            return found;
        }
    }
    return std::nullopt;
}

std::optional<std::string> nonceFor(const Json& session, const std::string& xuid)
{
    const auto properties = session.find("properties");
    if (properties == session.end() || !properties->is_object()) return std::nullopt;
    const auto custom = properties->find("custom");
    if (custom == properties->end() || !custom->is_object()) return std::nullopt;
    const auto nonces = custom->find("nonces");
    if (nonces == custom->end() || !nonces->is_object()) return std::nullopt;
    const auto nonce = nonces->find(xuid);
    if (nonce == nonces->end() || !nonce->is_string() || nonce->get<std::string>().empty()) return std::nullopt;
    return nonce->get<std::string>();
}

Result<std::optional<SessionRef>> findSession(IHttp& http, const std::string& authorization, const std::string& xuid, std::uint64_t netherNetId)
{
    const Json query{{"type", "activity"}, {"scid", kMinecraftScid},
        {"owners", {{"people", {{"moniker", "people"}, {"monikerXuid", xuid}}}}}};
    const auto found = http.send({"POST", std::string(kSessionDirectory) + "/handles/query?include=relatedInfo,customProperties",
        headers(authorization, true), query.dump()});
    if (found.status != 200) return {std::nullopt, failure(found, "handles/query")};
    return {findByNetherNetId(Json::parse(found.body, nullptr, false), netherNetId), {}};
}
Result<std::string> joinAndGetNonce(IHttp& http, const std::string& authorization, const std::string& xuid, std::uint64_t netherNetId,
    const std::function<bool(std::chrono::milliseconds)>& wait, const ActiveMember* active)
{
    const auto session = findSession(http, authorization, xuid, netherNetId);
    if (!session.value) return {std::nullopt, session.error};
    const auto& ref = *session.value;
    if (!ref) return {std::nullopt, "the world is not among the Xbox sessions of people this bot follows (befriend the host first)"};
    Json join{{"members", {{"me", {{"constants", {{"system", {{"xuid", xuid}, {"initialize", true}}}}}}}}}};
    if (active) join["members"]["me"]["properties"] = {{"system", {{"active", true}, {"connection", active->connection},
        {"subscription", {{"id", active->subscription}, {"changeTypes", Json::array({"everything"})}}}}}};
    const auto joined = http.send({"PUT", sessionUrl(*ref), headers(authorization, true), join.dump()});
    if (joined.status != 200 && joined.status != 201) return {std::nullopt, failure(joined, "joining the Xbox session")};
    if (const auto nonce = nonceFor(Json::parse(joined.body, nullptr, false), xuid)) return {*nonce, {}};
    for (int i = 0; i < kNoncePolls; ++i) {
        if (!wait(kNonceInterval)) return {std::nullopt, "cancelled"};
        const auto read = http.send({"GET", sessionUrl(*ref), headers(authorization, false), {}});
        if (read.status != 200) return {std::nullopt, failure(read, "reading the Xbox session")};
        if (const auto nonce = nonceFor(Json::parse(read.body, nullptr, false), xuid)) return {*nonce, {}};
    }
    return {std::nullopt, "the host did not hand out a join nonce in 90 seconds"};
}
Result<bool> leave(IHttp& http, const std::string& authorization, const std::string& xuid, std::uint64_t netherNetId)
{
    const auto session = findSession(http, authorization, xuid, netherNetId);
    if (!session.value) return {std::nullopt, session.error};
    if (!*session.value) return {true, {}};
    const Json leaving{{"members", {{"me", nullptr}}}};
    const auto left = http.send({"PUT", sessionUrl(**session.value), headers(authorization, true), leaving.dump()});
    if (left.status != 200 && left.status != 201 && left.status != 204) return {std::nullopt, failure(left, "leaving the Xbox session")};
    return {true, {}};
}
}
