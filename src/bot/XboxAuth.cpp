#include "XboxAuth.h"
#include "Base64.h"
#include "core/Strings.h"
#include <winhttp.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <limits>

namespace tsukuyomi::bot {
namespace {
using Json = nlohmann::json;
constexpr char kClientId[] = "0000000048183522";
constexpr char kScope[] = "service::user.auth.xboxlive.com::MBI_SSL";
constexpr char kXboxAgent[] = "XAL Android 2025.04.20250326.000";
constexpr char kServiceUri[] = "https://authorization.franchise.minecraft-services.net";
constexpr char kPlayFabTitle[] = "20CA2";
constexpr std::int64_t kFiletimeEpoch = 116444736000000000;
std::int64_t nowUnix()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
std::int64_t nowFiletime()
{
    FILETIME time{}; GetSystemTimeAsFileTime(&time);
    return static_cast<std::int64_t>((static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime);
}
std::string oneLine(std::string text)
{
    std::replace(text.begin(), text.end(), '\r', ' '); std::replace(text.begin(), text.end(), '\n', ' ');
    return text;
}
std::string errorAt(const HttpResponse& response, std::string_view stage)
{
    std::string error = "HTTP " + std::to_string(response.status);
    if (const auto code = response.header("X-Err")) error += " X-Err " + *code;
    error += " at " + std::string(stage);
    if (!response.error.empty()) error += ": " + response.error;
    const auto body = Json::parse(response.body, nullptr, false);
    if (body.is_object()) {
        for (const auto* field : {"error", "error_description", "WebPage"})
            if (body.contains(field) && body[field].is_string()) error += " " + body[field].get<std::string>();
    }
    return oneLine(std::move(error));
}
Result<Json> parseResponse(const HttpResponse& response, std::string_view stage)
{
    if (response.status < 200 || response.status >= 300) return {std::nullopt, errorAt(response, stage)};
    auto body = Json::parse(response.body, nullptr, false);
    if (!body.is_object()) return {std::nullopt, errorAt(response, stage) + ": invalid JSON"};
    return {std::move(body), {}};
}
std::string malformed(const HttpResponse& response, std::string_view stage)
{
    return errorAt(response, stage) + ": invalid response fields";
}
std::string formValue(std::string_view text)
{
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (const unsigned char c : text) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            result += static_cast<char>(c);
        else { result += '%'; result += hex[c >> 4]; result += hex[c & 15]; }
    }
    return result;
}
HttpResponse sendForm(IHttp& http, std::string_view endpoint, std::string body)
{
    return http.send({"POST", "https://login.live.com/" + std::string(endpoint),
        {{"Content-Type", "application/x-www-form-urlencoded"}}, std::move(body)});
}
Result<MsaToken> parseMsa(const HttpResponse& response, std::string_view stage, const std::string& previousRefresh)
{
    auto parsed = parseResponse(response, stage);
    if (!parsed.value) return {std::nullopt, parsed.error};
    try {
        const auto& body = *parsed.value;
        MsaToken token{body.at("access_token").get<std::string>(), body.value("refresh_token", previousRefresh), 0};
        const auto seconds = body.at("expires_in").get<std::int64_t>();
        const auto now = nowUnix();
        if (token.accessToken.empty() || token.refreshToken.empty() || seconds <= 0
            || seconds > (std::numeric_limits<std::int64_t>::max)() - now) return {std::nullopt, malformed(response, stage)};
        token.expiresAtUnix = now + seconds;
        return {std::move(token), {}};
    } catch (const Json::exception&) { return {std::nullopt, malformed(response, stage)}; }
}
Json proofJwk(const crypto::EcKey& key)
{
    return {{"crv", "P-256"}, {"alg", "ES256"}, {"use", "sig"}, {"kty", "EC"},
        {"x", base64::encodeUrl(key.publicX())}, {"y", base64::encodeUrl(key.publicY())}};
}
bool updateClock(XblSession& session, const HttpResponse& response)
{
    const auto date = response.header("Date");
    if (!date) return false;
    const auto wide = toUtf16(*date);
    SYSTEMTIME time{}; FILETIME filetime{};
    if (!WinHttpTimeToSystemTime(wide.c_str(), &time) || !SystemTimeToFileTime(&time, &filetime)) return false;
    const auto server = static_cast<std::int64_t>((static_cast<std::uint64_t>(filetime.dwHighDateTime) << 32) | filetime.dwLowDateTime);
    session.clockSkew100ns = server - nowFiletime();
    return true;
}
HttpResponse sendXbox(IHttp& http, XblSession& session, std::string url, std::string_view path, const Json& body)
{
    HttpRequest request{"POST", std::move(url), {{"User-Agent", kXboxAgent}, {"Content-Type", "application/json"},
        {"x-xbl-contract-version", "1"}}, body.dump()};
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto signature = xboxSignature(session, "POST", path, "", request.body, nowFiletime());
        if (signature.empty()) return {0, {}, {}, "Cannot sign Xbox request"};
        if (attempt == 0) request.headers.emplace_back("Signature", signature);
        else request.headers.back().second = signature;
        auto response = http.send(request);
        const bool corrected = updateClock(session, response);
        if (attempt == 0 && response.status == 401 && !response.header("X-Err") && corrected) continue;
        return response;
    }
    return {0, {}, {}, "Xbox retry exhausted"};
}
std::optional<std::int64_t> isoUnix(const std::string& value)
{
    unsigned year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    int used = 0;
    if (sscanf_s(value.c_str(), "%4u-%2u-%2uT%2u:%2u:%2u%n", &year, &month, &day, &hour, &minute, &second, &used) != 6)
        return std::nullopt;
    std::size_t end = static_cast<std::size_t>(used);
    if (end < value.size() && value[end] == '.') {
        const auto start = ++end;
        while (end < value.size() && value[end] >= '0' && value[end] <= '9') ++end;
        if (end == start) return std::nullopt;
    }
    if (value.substr(end) != "Z" || year < 1601 || year > 9999 || month > 12 || day > 31 || hour > 23 || minute > 59 || second > 59)
        return std::nullopt;
    SYSTEMTIME time{}; time.wYear = static_cast<WORD>(year); time.wMonth = static_cast<WORD>(month);
    time.wDay = static_cast<WORD>(day); time.wHour = static_cast<WORD>(hour);
    time.wMinute = static_cast<WORD>(minute); time.wSecond = static_cast<WORD>(second);
    FILETIME ft{};
    if (!SystemTimeToFileTime(&time, &ft)) return std::nullopt;
    return (static_cast<std::int64_t>((static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) - kFiletimeEpoch) / 10000000;
}
std::string xblAuthorization(const XstsToken& token) { return "XBL3.0 x=" + token.userHash + ';' + token.token; }
HttpResponse sendService(IHttp& http, std::string_view path, const Json& body, const std::string& authorization = {})
{
    HttpRequest request{"POST", std::string(kServiceUri) + std::string(path), {{"Content-Type", "application/json"},
        {"Accept", "application/json"}, {"User-Agent", "libhttpclient/1.0.0.0"}}, body.dump()};
    if (!authorization.empty()) request.headers.emplace_back("Authorization", authorization);
    return http.send(request);
}
}
Result<DeviceCode> startDeviceCode(IHttp& http)
{
    const auto response = sendForm(http, "oauth20_connect.srf", std::string("client_id=") + kClientId + "&scope=" + formValue(kScope) + "&response_type=device_code");
    auto parsed = parseResponse(response, "device code");
    if (!parsed.value) return {std::nullopt, parsed.error};
    try {
        const auto& body = *parsed.value;
        DeviceCode code{body.at("user_code").get<std::string>(), body.at("device_code").get<std::string>(),
            body.at("verification_uri").get<std::string>(), body.at("interval").get<int>(), body.at("expires_in").get<int>()};
        if (code.userCode.empty() || code.deviceCode.empty() || code.verificationUri.empty() || code.intervalSec <= 0 || code.expiresInSec <= 0)
            return {std::nullopt, malformed(response, "device code")};
        return {std::move(code), {}};
    } catch (const Json::exception&) { return {std::nullopt, malformed(response, "device code")}; }
}
std::pair<PollState, Result<MsaToken>> pollDeviceCode(IHttp& http, const DeviceCode& code)
{
    const auto response = sendForm(http, "oauth20_token.srf", std::string("client_id=") + kClientId
        + "&grant_type=" + formValue("urn:ietf:params:oauth:grant-type:device_code") + "&device_code=" + formValue(code.deviceCode));
    const auto body = Json::parse(response.body, nullptr, false);
    if (response.status == 400 && body.is_object() && body.contains("error") && body["error"].is_string()) {
        if (body["error"] == "authorization_pending") return {PollState::Pending, {std::nullopt, {}}};
        if (body["error"] == "slow_down") return {PollState::SlowDown, {std::nullopt, {}}};
    }
    auto token = parseMsa(response, "device poll", "");
    const auto state = token.value ? PollState::Done : PollState::Failed;
    return {state, std::move(token)};
}
Result<MsaToken> refreshMsa(IHttp& http, const std::string& refreshToken)
{
    return parseMsa(sendForm(http, "oauth20_token.srf", std::string("client_id=") + kClientId + "&scope=" + formValue(kScope)
        + "&grant_type=refresh_token&refresh_token=" + formValue(refreshToken)), "MSA refresh", refreshToken);
}
std::string xboxSignature(const XblSession& session, std::string_view method, std::string_view path,
    std::string_view authorization, std::string_view body, std::int64_t filetime)
{
    if (session.proofKey.publicX().size() != 32) return {};
    const auto skew = session.clockSkew100ns;
    if ((skew > 0 && filetime > (std::numeric_limits<std::int64_t>::max)() - skew)
        || (skew < 0 && filetime < (std::numeric_limits<std::int64_t>::min)() - skew)) return {};
    const auto timestamp = static_cast<std::uint64_t>(filetime + skew);
    std::vector<std::uint8_t> header{0, 0, 0, 1};
    for (int shift = 56; shift >= 0; shift -= 8) header.push_back(static_cast<std::uint8_t>(timestamp >> shift));
    std::vector<std::uint8_t> input{0, 0, 0, 1, 0};
    input.insert(input.end(), header.begin() + 4, header.end()); input.push_back(0);
    for (const auto part : {method, path, authorization, body}) {
        input.insert(input.end(), part.begin(), part.end()); input.push_back(0);
    }
    const auto signature = session.proofKey.signDigest(crypto::sha256(input));
    if (signature.size() != 64) return {};
    header.insert(header.end(), signature.begin(), signature.end());
    return base64::encode(header);
}
namespace {
struct DeviceToken { std::string token; std::int64_t notAfterUnix = 0; };
Result<DeviceToken> requestDeviceToken(IHttp& http, XblSession& session)
{
    const auto response = sendXbox(http, session, "https://device.auth.xboxlive.com/device/authenticate", "/device/authenticate",
        {{"RelyingParty", "http://auth.xboxlive.com"}, {"TokenType", "JWT"}, {"Properties", {
            {"AuthMethod", "ProofOfPossession"}, {"Id", session.deviceId}, {"DeviceType", "Android"}, {"Version", "13"}, {"ProofKey", proofJwk(session.proofKey)}}}});
    auto parsed = parseResponse(response, "device/authenticate");
    if (!parsed.value) return {std::nullopt, parsed.error};
    try {
        DeviceToken token{parsed.value->at("Token").get<std::string>(), 0};
        if (const auto it = parsed.value->find("NotAfter"); it != parsed.value->end() && it->is_string())
            token.notAfterUnix = isoUnix(it->get<std::string>()).value_or(0);
        if (!token.token.empty()) return {std::move(token), {}};
    } catch (const Json::exception&) { }
    return {std::nullopt, malformed(response, "device/authenticate")};
}
}
Result<std::string> deviceToken(IHttp& http, XblSession& session)
{
    auto token = requestDeviceToken(http, session);
    if (!token.value) return {std::nullopt, token.error};
    return {std::move(token.value->token), {}};
}
Result<XstsToken> sisuAuthorize(IHttp& http, XblSession& session, const MsaToken& msa, const std::string& device, const std::string& relyingParty)
{
    const auto response = sendXbox(http, session, "https://sisu.xboxlive.com/authorize", "/authorize", {
        {"AccessToken", "t=" + msa.accessToken}, {"AppId", kClientId}, {"DeviceToken", device}, {"Sandbox", "RETAIL"},
        {"UseModernGamertag", true}, {"SiteName", "user.auth.xboxlive.com"}, {"RelyingParty", relyingParty}, {"ProofKey", proofJwk(session.proofKey)}});
    auto parsed = parseResponse(response, "sisu/authorize");
    if (!parsed.value) return {std::nullopt, parsed.error};
    try {
        const auto& body = *parsed.value;
        const auto page = body.value("WebPage", std::string{});
        if (!body.contains("AuthorizationToken"))
            return {std::nullopt, oneLine(errorAt(response, "sisu/authorize") + ": Xbox profile setup required " + page)};
        const auto& auth = body.at("AuthorizationToken");
        const auto& claims = auth.at("DisplayClaims").at("xui").at(0);
        const auto expiry = isoUnix(auth.at("NotAfter").get<std::string>());
        XstsToken token{auth.at("Token").get<std::string>(), claims.at("uhs").get<std::string>(),
            claims.value("xid", std::string{}), claims.value("gtg", std::string{}), expiry.value_or(0)};
        if (token.token.empty() || token.userHash.empty() || !expiry) return {std::nullopt, malformed(response, "sisu/authorize")};
        return {std::move(token), {}};
    } catch (const Json::exception&) { return {std::nullopt, malformed(response, "sisu/authorize")}; }
}
Result<PlayFabLogin> playFabLogin(IHttp& http, const XstsToken& token)
{
    const Json info{{"GetCharacterInventories", false}, {"GetCharacterList", false}, {"GetPlayerProfile", true},
        {"GetPlayerStatistics", false}, {"GetTitleData", false}, {"GetUserAccountInfo", true}, {"GetUserData", false},
        {"GetUserInventory", false}, {"GetUserReadOnlyData", false}, {"GetUserVirtualCurrency", false},
        {"PlayerStatisticNames", nullptr}, {"ProfileConstraints", nullptr}, {"TitleDataKeys", nullptr}, {"UserDataKeys", nullptr}, {"UserReadOnlyDataKeys", nullptr}};
    const auto response = http.send({"POST", "https://20ca2.playfabapi.com/Client/LoginWithXbox", {{"Content-Type", "application/json"}},
        Json{{"CreateAccount", true}, {"EncryptedRequest", nullptr}, {"InfoRequestParameters", info},
            {"PlayerSecret", nullptr}, {"TitleId", kPlayFabTitle}, {"XboxToken", xblAuthorization(token)}}.dump()});
    auto parsed = parseResponse(response, "PlayFab LoginWithXbox");
    if (!parsed.value) return {std::nullopt, parsed.error};
    try {
        const auto& data = parsed.value->at("data");
        PlayFabLogin login{data.at("SessionTicket").get<std::string>(), data.at("PlayFabId").get<std::string>()};
        if (!login.sessionTicket.empty() && !login.playFabId.empty()) return {std::move(login), {}};
    } catch (const Json::exception&) { }
    return {std::nullopt, malformed(response, "PlayFab LoginWithXbox")};
}
Result<std::string> mcSessionStart(IHttp& http, const std::string& ticket, const std::string& deviceId)
{
    const auto response = sendService(http, "/api/v1.0/session/start", {
        {"device", {{"applicationType", "MinecraftPE"}, {"capabilities", Json::array()}, {"gameVersion", kGameVersion},
            {"id", deviceId}, {"memory", "17179869184"}, {"platform", "Windows10"}, {"playFabTitleId", kPlayFabTitle},
            {"storePlatform", "uwp.store"}, {"type", "Windows10"}}},
        {"user", {{"language", "en"}, {"languageCode", "en-US"}, {"regionCode", "US"}, {"token", ticket}, {"tokenType", "PlayFab"}}}});
    auto parsed = parseResponse(response, "session/start");
    if (!parsed.value) return {std::nullopt, parsed.error};
    try {
        auto token = parsed.value->at("result").at("authorizationHeader").get<std::string>();
        if (token.starts_with("MCToken ") && token.size() > 8) return {std::move(token), {}};
    } catch (const Json::exception&) { }
    return {std::nullopt, malformed(response, "session/start")};
}
Result<std::string> multiplayerToken(IHttp& http, const std::string& token, const crypto::EcKey& key)
{
    return multiplayerToken(http, token, key.spkiDer());
}
Result<std::string> multiplayerToken(IHttp& http, const std::string& token, std::span<const std::uint8_t> spki)
{
    if (spki.empty()) return {std::nullopt, "Invalid P-384 key at multiplayer/session/start"};
    const auto response = sendService(http, "/api/v1.0/multiplayer/session/start", {{"publicKey", base64::encode(spki)}}, token);
    auto parsed = parseResponse(response, "multiplayer/session/start");
    if (!parsed.value) return {std::nullopt, parsed.error};
    try {
        auto signedToken = parsed.value->at("result").at("signedToken").get<std::string>();
        if (!signedToken.empty()) return {std::move(signedToken), {}};
    } catch (const Json::exception&) { }
    return {std::nullopt, malformed(response, "multiplayer/session/start")};
}
Result<StoredAccount> completeFirstLogin(IHttp& http, const MsaToken& msa)
{
    auto key = crypto::EcKey::generate(crypto::Curve::P256);
    const auto deviceId = crypto::newUuid(), mcId = crypto::newUuid();
    if (!key || deviceId.empty() || mcId.empty()) return {std::nullopt, "Cannot create Xbox device identity"};
    XblSession session{std::move(*key), "{" + deviceId + "}", 0};
    auto device = deviceToken(http, session);
    if (!device.value) return {std::nullopt, device.error};
    auto profile = sisuAuthorize(http, session, msa, *device.value, "http://xboxlive.com");
    if (!profile.value) return {std::nullopt, profile.error};
    if (profile.value->gamertag.empty() || profile.value->xuid.empty())
        return {std::nullopt, "Xbox profile missing gamertag or XUID at sisu/authorize"};
    if (msa.refreshToken.empty()) return {std::nullopt, "Missing refresh token at first login"};
    StoredAccount account{profile.value->gamertag, profile.value->xuid, session.deviceId, mcId,
        std::move(session.proofKey), msa.refreshToken, nowUnix()};
    return {std::move(account), {}};
}
namespace {
constexpr char kXboxLiveRp[] = "http://xboxlive.com";
constexpr char kPlayFabRp[] = "https://b980a380.minecraft.playfabapi.com/";
constexpr std::int64_t kCacheMarginSec = 300;
constexpr std::int64_t kPlayFabCacheSec = 3600, kMcTokenCacheSec = 1800;
bool fresh(std::int64_t untilUnix) { return untilUnix > nowUnix() + kCacheMarginSec; }
bool rateLimited(const std::string& error) { return error.starts_with("HTTP 429"); }
struct Chain {
    IHttp& http;
    StoredAccount& account;
    AuthCache* cache;
    std::optional<XblSession> session;
    Result<MsaToken> msa()
    {
        if (cache && cache->msa && fresh(cache->msa->expiresAtUnix)) return {*cache->msa, {}};
        auto token = refreshMsa(http, account.refreshToken);
        if (!token.value) return token;
        account.refreshToken = token.value->refreshToken; account.savedAt = nowUnix();
        if (cache) { cache->msa = *token.value; cache->msa->refreshToken.clear(); }
        return token;
    }
    bool openSession()
    {
        auto proof = crypto::EcKey::importPrivate(crypto::Curve::P256, account.proofKey.exportPrivate());
        if (!proof) return false;
        session.emplace(XblSession{std::move(*proof), account.deviceId, cache ? cache->clockSkew100ns : 0});
        return true;
    }
    void keepClock() { if (cache && session) cache->clockSkew100ns = session->clockSkew100ns; }
    Result<std::string> device()
    {
        if (cache && !cache->deviceToken.empty() && fresh(cache->deviceNotAfterUnix)) return {cache->deviceToken, {}};
        auto token = requestDeviceToken(http, *session);
        keepClock();
        if (!token.value) return {std::nullopt, token.error};
        if (cache) { cache->deviceToken = token.value->token; cache->deviceNotAfterUnix = token.value->notAfterUnix; }
        return {std::move(token.value->token), {}};
    }
    Result<XstsToken> xsts(const MsaToken& msaToken, const std::string& deviceToken, std::string_view relyingParty)
    {
        if (cache) if (const auto it = cache->xsts.find(relyingParty); it != cache->xsts.end() && fresh(it->second.notAfterUnix)) return {it->second, {}};
        auto token = sisuAuthorize(http, *session, msaToken, deviceToken, std::string(relyingParty));
        keepClock();
        if (token.value && cache) cache->xsts.insert_or_assign(std::string(relyingParty), *token.value);
        return token;
    }
    Result<PlayFabLogin> playFab(const XstsToken& token)
    {
        if (cache && cache->playFab && fresh(cache->playFabUntilUnix)) return {*cache->playFab, {}};
        auto login = playFabLogin(http, token);
        if (login.value && cache) { cache->playFab = *login.value; cache->playFabUntilUnix = nowUnix() + kPlayFabCacheSec; }
        return login;
    }
    Result<std::string> mcToken(const std::string& ticket)
    {
        if (cache && !cache->mcToken.empty() && fresh(cache->mcUntilUnix)) return {cache->mcToken, {}};
        auto token = mcSessionStart(http, ticket, account.mcDeviceId);
        if (token.value && cache) { cache->mcToken = *token.value; cache->mcUntilUnix = nowUnix() + kMcTokenCacheSec; }
        return token;
    }
};
Result<std::string> xboxLiveAuthorizationOnce(IHttp& http, StoredAccount& account, AuthCache* cache)
{
    Chain chain{http, account, cache, std::nullopt};
    auto msa = chain.msa();
    if (!msa.value) return {std::nullopt, msa.error};
    if (!chain.openSession()) return {std::nullopt, "Cannot import the proof key at xboxLiveAuthorization"};
    auto device = chain.device();
    if (!device.value) return {std::nullopt, device.error};
    auto xbl = chain.xsts(*msa.value, *device.value, kXboxLiveRp);
    if (!xbl.value) return {std::nullopt, xbl.error};
    return {"XBL3.0 x=" + xbl.value->userHash + ";" + xbl.value->token, {}};
}
Result<LoginMaterial> prepareLoginOnce(IHttp& http, StoredAccount& account, std::span<const std::uint8_t> foreignSpki, bool xboxLive, AuthCache* cache)
{
    Chain chain{http, account, cache, std::nullopt};
    auto msa = chain.msa();
    if (!msa.value) return {std::nullopt, msa.error};
    auto loginKey = crypto::EcKey::generate(crypto::Curve::P384);
    if (!chain.openSession() || !loginKey) return {std::nullopt, "Cannot create authentication keys at prepareLogin"};
    auto device = chain.device();
    if (!device.value) return {std::nullopt, device.error};
    auto pf = chain.xsts(*msa.value, *device.value, kPlayFabRp);
    if (!pf.value) return {std::nullopt, pf.error};
    auto playfab = chain.playFab(*pf.value);
    if (!playfab.value) return {std::nullopt, playfab.error};
    auto mc = chain.mcToken(playfab.value->sessionTicket);
    if (!mc.value) return {std::nullopt, mc.error};
    LoginMaterial material{std::move(*loginKey), {}, account.xuid, account.name, playfab.value->playFabId, {}};
    if (foreignSpki.empty()) {
        auto token = multiplayerToken(http, *mc.value, material.loginKey);
        if (!token.value) return {std::nullopt, token.error};
        material.token = std::move(*token.value);
    } else {
        auto foreign = multiplayerToken(http, *mc.value, foreignSpki);
        if (!foreign.value) return {std::nullopt, foreign.error};
        material.foreignToken = std::move(*foreign.value);
    }
    if (xboxLive) {
        auto xbl = chain.xsts(*msa.value, *device.value, kXboxLiveRp);
        if (!xbl.value) return {std::nullopt, xbl.error};
        material.xboxLiveAuthorization = "XBL3.0 x=" + xbl.value->userHash + ";" + xbl.value->token;
    }
    return {std::move(material), {}};
}
}
Result<std::string> xboxLiveAuthorization(IHttp& http, StoredAccount& account, AuthCache* cache)
{
    const bool reused = cache && !cache->empty();
    auto result = xboxLiveAuthorizationOnce(http, account, cache);
    if (!result.value && reused && !rateLimited(result.error)) {
        *cache = {};
        result = xboxLiveAuthorizationOnce(http, account, cache);
    }
    return result;
}
Result<LoginMaterial> prepareLogin(IHttp& http, StoredAccount& account, std::span<const std::uint8_t> foreignSpki, bool xboxLive, AuthCache* cache)
{
    const bool reused = cache && !cache->empty();
    auto result = prepareLoginOnce(http, account, foreignSpki, xboxLive, cache);
    if (!result.value && reused && !rateLimited(result.error)) {
        *cache = {};
        result = prepareLoginOnce(http, account, foreignSpki, xboxLive, cache);
    }
    return result;
}
}
