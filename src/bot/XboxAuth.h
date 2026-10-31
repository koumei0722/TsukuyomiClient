#pragma once
#include "AccountStore.h"
#include "Http.h"
#include <map>

namespace tsukuyomi::bot {
inline constexpr char kGameVersion[] = "1.26.60";
template<class T> struct Result {
    std::optional<T> value;
    std::string error;
};
struct DeviceCode {
    std::string userCode, deviceCode, verificationUri;
    int intervalSec = 0, expiresInSec = 0;
};
struct MsaToken {
    std::string accessToken, refreshToken;
    std::int64_t expiresAtUnix = 0;
};
enum class PollState { Pending, SlowDown, Done, Failed };
Result<DeviceCode> startDeviceCode(IHttp&);
std::pair<PollState, Result<MsaToken>> pollDeviceCode(IHttp&, const DeviceCode&);
Result<MsaToken> refreshMsa(IHttp&, const std::string& refreshToken);
struct XblSession {
    crypto::EcKey proofKey;
    std::string deviceId;
    std::int64_t clockSkew100ns = 0;
};
std::string xboxSignature(const XblSession&, std::string_view method, std::string_view pathAndQuery,
    std::string_view authorization, std::string_view body, std::int64_t filetime);
struct XstsToken {
    std::string token, userHash, xuid, gamertag;
    std::int64_t notAfterUnix = 0;
};
Result<std::string> deviceToken(IHttp&, XblSession&);
Result<XstsToken> sisuAuthorize(IHttp&, XblSession&, const MsaToken&, const std::string& deviceToken, const std::string& relyingParty);
struct PlayFabLogin { std::string sessionTicket, playFabId; };
Result<PlayFabLogin> playFabLogin(IHttp&, const XstsToken&);
Result<std::string> mcSessionStart(IHttp&, const std::string& sessionTicket, const std::string& mcDeviceId);
Result<std::string> multiplayerToken(IHttp&, const std::string& mcToken, const crypto::EcKey&);
Result<std::string> multiplayerToken(IHttp&, const std::string& mcToken, std::span<const std::uint8_t> spki);
struct AuthCache {
    std::optional<MsaToken> msa;
    std::string deviceToken;
    std::int64_t deviceNotAfterUnix = 0;
    std::map<std::string, XstsToken, std::less<>> xsts;
    std::optional<PlayFabLogin> playFab;
    std::int64_t playFabUntilUnix = 0;
    std::string mcToken;
    std::int64_t mcUntilUnix = 0;
    std::int64_t clockSkew100ns = 0;
    bool empty() const { return !msa && deviceToken.empty() && xsts.empty() && !playFab && mcToken.empty(); }
};
struct LoginMaterial {
    crypto::EcKey loginKey;
    std::string token, xuid, gamertag, playFabId;
    std::string foreignToken;
    std::string xboxLiveAuthorization;
};
Result<LoginMaterial> prepareLogin(IHttp&, StoredAccount&, std::span<const std::uint8_t> foreignSpki = {}, bool xboxLive = false,
    AuthCache* cache = nullptr);
Result<std::string> xboxLiveAuthorization(IHttp&, StoredAccount&, AuthCache* cache = nullptr);
Result<StoredAccount> completeFirstLogin(IHttp&, const MsaToken&);
}
