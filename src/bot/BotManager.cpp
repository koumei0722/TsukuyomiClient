#include "BotManager.h"
#include "AccountStore.h"
#include "BotCompletion.h"
#include "Base64.h"
#include "BotNet.h"
#include "ContainerScreen.h"
#include "LocalPlayerIds.h"
#include "LoginRequest.h"
#include "Mpsd.h"
#include "Rta.h"
#include "GameConnection.h"
#include "GameCodec.h"
#include "GamePacket.h"
#include "GamePath.h"
#include "GameTerrain.h"
#include "GameIdentity.h"
#include "GameKey.h"
#include "FriendWorld.h"
#include "NetherNetLink.h"
#include "PlayerInteract.h"
#include "RakNetLink.h"
#include "Relog.h"
#include "Jwt.h"
#include "SkinPack.h"
#include "SkinSteal.h"
#include "XboxAuth.h"
#include "Clipboard.h"
#include "core/Logger.h"
#include "core/Paths.h"
#include "game/ClientChat.h"
#include "game/GameData.h"
#include "game/LevelPlayers.h"
#include <algorithm>
#include <chrono>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <map>
#include <span>
#include <Windows.h>
#include <objbase.h>

namespace tsukuyomi::bot {
namespace {
constexpr std::uint64_t kRelogTimeoutMs = 120000;
constexpr std::uint64_t kRelogRejoinBotDelayMs = 2000;
constexpr std::uint64_t kLeavingWaitMs = 5000;
constexpr std::uint64_t kRiderTimeoutMs = 60000;
constexpr std::uint64_t kPendingPossessTimeoutMs = 120000;
constexpr std::uint64_t kPendingPossessUuidTimeoutMs = 30000;
constexpr std::uint64_t kExtraLingerMs = 1500;
struct RejoinRoute { std::string packet; std::string where; std::string friendId; bool friendWorld = false; std::uint64_t netherNetId = 0; };
std::optional<RejoinRoute> rejoinRoute(std::string& error, void* game, std::uint32_t mainThread)
{
    if (GameData::instance().integratedServer()) {
        error = "switching is unavailable in your local world (after the world reloads, the game's server refuses the bot as a duplicate player)";
        return std::nullopt;
    }
    if (const auto address = net::serverAddress()) {
        auto packet = relog::transfer(*address, false);
        if (packet.empty()) { error = "cannot build the transfer to the server"; return std::nullopt; }
        return RejoinRoute{std::move(packet), address->host + ":" + std::to_string(address->port), {}};
    }
    if (const auto id = nethernet::serverId(net::mainPeer()); id && id->type == 1) {
        if (!friendworld::available()) { error = "switching in a friend's world is unavailable in this game version"; return std::nullopt; }
        const std::string where = "the friend's world " + std::to_string(id->id);
        if (!game || GetCurrentThreadId() != mainThread) return RejoinRoute{{}, where, {}, true, id->id};
        auto friendId = friendworld::idFor(game, id->id);
        if (!friendId || friendId->empty()) {
            error = "this world is not in your game's list of friends' worlds, so the game cannot rejoin it";
            return std::nullopt;
        }
        return RejoinRoute{{}, where + " (list id " + *friendId + ")", std::move(*friendId), true, id->id};
    }
    error = "the server address is unknown (inject Tsukuyomi before joining the world)";
    return std::nullopt;
}
std::uint64_t steadyMs()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
bool sendBotDisconnect(std::uint8_t sub)
{
    const auto packet = codec().disconnect(sub);
    return !packet.empty() && net::sendRaw(packet);
}
bool sameName(const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) == 0; }
std::filesystem::path accountsPath() { return paths::dataDir() / "BotAccounts.json"; }
std::filesystem::path skinsPath() { return paths::dataDir() / "BotSkins.json"; }
std::string lowerKey(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
    return text;
}
std::vector<std::filesystem::path> skinRoots()
{
    std::error_code error;
    std::filesystem::create_directories(paths::dataDir() / "skins", error);
    return {paths::dataDir() / "skins"};
}
std::vector<std::filesystem::path> skinGeometryRoots()
{
    wchar_t exe[MAX_PATH * 2]{};
    const auto length = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    if (!length) return {};
    return {std::filesystem::path(std::wstring(exe, length)).parent_path() / "data" / "skin_packs"};
}
std::atomic<void*> g_completionConnection{nullptr};
std::atomic<int> g_announceLogs{0};
int g_fedLogs = 0;
constexpr std::int32_t kNearChunks = 4, kFedChunkRadius = 4;
constexpr std::uint64_t kFedWaitMs = 60000;
constexpr std::int32_t kSubChunksDown = 4, kSubChunksUp = 3;
constexpr std::uint64_t kAskAgainMs = 3000, kTerrainIdleMs = 10000;
constexpr std::uint64_t kLiquidCheckMs = 100;
int g_liquidLogs = 0;
void announceAccounts(const std::vector<StoredAccount>& accounts)
{
    void* const connection = net::mainConnection();
    if (connection == nullptr || connection != g_completionConnection.load(std::memory_order_acquire)) return;
    std::vector<std::string> names;
    for (const auto& account : accounts) names.push_back(account.name);
    if (auto gamertag = readUserGamertag(); !gamertag.empty()) names.push_back(std::move(gamertag));
    const auto values = accountEnumValues(names);
    auto packet = gamecodec::updateSoftEnumToGame(kAccountSoftEnum, values, 2);
    if (packet.empty()) return;
    net::schedule(std::move(packet), 0);
    if (g_announceLogs.fetch_add(1) < 16) log().info(L"Bot: account name candidates updated ({} name(s))", values.size());
}
}
std::vector<std::string> savedAccountNames() { return accountStore::names(accountsPath()); }
std::string readUserGamertag()
{
    if (const auto parts = splitConnectionRequest(net::userRequest()))
        if (const auto payload = jwt::readPayload(parts->second); payload && payload->contains("ThirdPartyName") && (*payload)["ThirdPartyName"].is_string())
            return (*payload)["ThirdPartyName"].get<std::string>();
    return {};
}
std::vector<std::string> completionNames()
{
    auto names = savedAccountNames();
    if (auto gamertag = readUserGamertag(); !gamertag.empty()) names.push_back(std::move(gamertag));
    return names;
}
void noteAccountCompletion() { g_completionConnection.store(net::mainConnection(), std::memory_order_release); }
BotManager& BotManager::instance() { static BotManager value; return value; }
BotManager::~BotManager() { shutdown(); }
void BotManager::initialize()
{
    std::lock_guard lock(workMutex_);
    if (thread_.joinable()) return;
    stopping_ = false;
    {
        std::lock_guard sessionLock(sessionMutex_);
        std::string message;
        if (!loadSkinSettings(message)) pendingChat_.push_back("[Tsukuyomi] " + message);
    }
    net::setPump(&BotManager::pumpFromReceive);
    net::setSendFilter(&InventoryView::filterSend);
    thread_ = std::thread([this] { worker(); });
    running_.store(true, std::memory_order_release);
}
void BotManager::shutdown()
{
    bool active = false;
    {
        std::lock_guard lock(sessionMutex_);
        active = std::any_of(slots_.begin(), slots_.end(), [](const Slot& slot) { return online(slot); }) || !extras_.empty();
    }
    if (active && running_.load(std::memory_order_acquire)) {
        leaveDone_.store(false, std::memory_order_release);
        leaveRequested_.store(true, std::memory_order_release);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!leaveDone_.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!leaveDone_.load(std::memory_order_acquire))
            log().warn(L"Bot: the game thread did not send Disconnect before unloading");
        while (net::timedPending() && std::chrono::steady_clock::now() < deadline + std::chrono::seconds(1))
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    running_.store(false, std::memory_order_release);
    net::setPumpWanted(false); net::setPump(nullptr); net::setSendFilter(nullptr);
    { std::lock_guard lock(sessionMutex_); inventoryView_.abandon(); }
    {
        std::lock_guard lock(workMutex_);
        stopping_ = true; jobs_.clear();
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
    {
        std::lock_guard lock(sessionMutex_);
        for (auto& slot : slots_) slot = {};
        possessing_ = false; possessed_.clear(); net::deactivateAll();
        leaveExtras(false);
        closeLingering(true);
        endGameAccount();
    }
    gameidentity::release();
    gamekey::release();
    std::lock_guard lock(workMutex_);
    results_.clear(); loginBusy_ = false;
}
void BotManager::post(Result result)
{
    std::lock_guard lock(workMutex_);
    if (!stopping_) results_.push_back(std::move(result));
}
void BotManager::enqueue(Job job)
{
    { std::lock_guard lock(workMutex_); if (!stopping_) jobs_.push_back(std::move(job)); }
    wake_.notify_one();
}
bool BotManager::pauseUntil(std::chrono::steady_clock::time_point deadline)
{
    std::unique_lock lock(workMutex_);
    return !wake_.wait_until(lock, deadline, [this] { return stopping_; });
}
void BotManager::worker()
{
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        Job job;
        {
            std::unique_lock lock(workMutex_);
            wake_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            if (stopping_) break;
            job = std::move(jobs_.front()); jobs_.pop_front();
        }
        try { run(job); }
        catch (const std::exception&) {
            post({failureFor(job.work), "Bot: authentication or account operation failed", job.name, job.generation});
        }
        if (job.work == Work::Login) { std::lock_guard lock(workMutex_); loginBusy_ = false; }
    }
    if (SUCCEEDED(com)) CoUninitialize();
}
void BotManager::run(const Job& job)
{
    if (job.work == Work::SkinSave) { saveStolenSkin(job); return; }
    if (job.work == Work::Skin) {
        auto loaded = skin::load(job.name, skinRoots(), skinGeometryRoots());
        if (!loaded.skin) { post({Delivery::SkinFailed, loaded.error.empty() ? std::string("cannot load the skin") : loaded.error, job.name, job.generation}); return; }
        post({Delivery::SkinReady, loaded.label.empty() ? job.name : loaded.label, job.name, job.generation, {}, {}, std::move(loaded.skin)});
        return;
    }
    const auto path = accountsPath();
    auto stored = accountStore::load(path);
    for (const auto& warning : stored.warnings) post({Delivery::Notice, "Bot: " + warning, {}, 0});
    if (!stored.warnings.empty() && job.work != Work::Accounts) {
        post({failureFor(job.work), "Bot: repair the account file before changing accounts", job.name, job.generation});
        return;
    }
    if (job.work == Work::Accounts) {
        if (stored.accounts.empty()) post({Delivery::Notice, "Bot: no saved accounts", {}, 0});
        for (const auto& account : stored.accounts) post({Delivery::Notice, "Bot account: " + account.name, {}, 0});
        return;
    }
    if (job.work == Work::Logout) {
        const auto found = std::find_if(stored.accounts.begin(), stored.accounts.end(), [&](const auto& a) { return sameName(a.name, job.name); });
        if (found == stored.accounts.end()) { post({Delivery::Notice, "Bot: account not found", {}, 0}); return; }
        const auto name = found->name;
        authCache_.erase(found->xuid);
        accountStore::remove(stored.accounts, name);
        const bool saved = accountStore::save(path, stored.accounts);
        if (saved) announceAccounts(stored.accounts);
        post({Delivery::Notice, saved ? "Bot: removed " + name : "Bot: cannot save accounts", {}, 0});
        return;
    }
    WinHttp winHttp;
    CoolingHttp http(winHttp, httpCooldown_);
    if (job.work != Work::Login) {
        if (!pauseUntil(lastNetworkJob_ + std::chrono::seconds(1))) return;
        lastNetworkJob_ = std::chrono::steady_clock::now();
    }
    if (job.work == Work::Login) {
        auto code = startDeviceCode(http);
        if (!code.value) { post({Delivery::Notice, "Bot login: " + code.error, {}, 0}); return; }
        const auto& uri = code.value->verificationUri;
        const auto link = uri.find('?') == std::string::npos ? uri + "?otc=" + code.value->userCode : uri;
        const bool copied = clipboard::putText(link);
        if (!copied) log().warn(L"Bot: clipboard write failed");
        post({Delivery::Notice, "Open " + uri + " and enter " + code.value->userCode + (copied ? " (link copied to clipboard)" : ""), {}, 0});
        const auto expires = std::chrono::steady_clock::now() + std::chrono::seconds(code.value->expiresInSec);
        auto interval = std::chrono::seconds(code.value->intervalSec);
        while (std::chrono::steady_clock::now() < expires) {
            if (!pauseUntil((std::min)(expires, std::chrono::steady_clock::now() + interval))) return;
            if (std::chrono::steady_clock::now() >= expires) break;
            auto polled = pollDeviceCode(http, *code.value);
            if (polled.first == PollState::SlowDown) { interval += std::chrono::seconds(5); continue; }
            if (polled.first == PollState::Pending) continue;
            if (polled.first != PollState::Done || !polled.second.value) {
                post({Delivery::Notice, "Bot login: " + polled.second.error, {}, 0}); return;
            }
            auto account = completeFirstLogin(http, *polled.second.value);
            if (!account.value) { post({Delivery::Notice, "Bot login: " + account.error, {}, 0}); return; }
            const auto name = account.value->name;
            accountStore::upsert(stored.accounts, std::move(*account.value));
            const bool saved = accountStore::save(path, stored.accounts);
            if (saved) announceAccounts(stored.accounts);
            post({Delivery::Notice, saved ? "Logged in as " + name : "Bot: cannot save accounts", {}, 0});
            return;
        }
        post({Delivery::Notice, "Bot login: device code expired", {}, 0});
        return;
    }
    const auto failed = failureFor(job.work);
    const auto found = std::find_if(stored.accounts.begin(), stored.accounts.end(), [&](const auto& a) { return sameName(a.name, job.name); });
    StoredAccount* account = found == stored.accounts.end() ? nullptr : &*found;
    if (!account) {
        post({failed, "Bot: account not found (use /tk bot accounts)", job.name, job.generation});
        return;
    }
    const auto previousRefresh = account->refreshToken;
    if (job.work == Work::LeaveSession) {
        auto authorization = xboxLiveAuthorization(http, *account, &authCache_[account->xuid]);
        if (account->refreshToken != previousRefresh) accountStore::save(path, stored.accounts);
        const auto left = authorization.value ? mpsd::leave(http, *authorization.value, account->xuid, job.hostNetherNetId) : tsukuyomi::bot::Result<bool>{std::nullopt, authorization.error};
        if (left.value) log().info(L"Bot: {} left the host's Xbox session", std::wstring(account->name.begin(), account->name.end()));
        else log().warn(L"Bot: {} could not leave the host's Xbox session ({})", std::wstring(account->name.begin(), account->name.end()),
            std::wstring(left.error.begin(), left.error.end()));
        return;
    }
    std::vector<std::uint8_t> foreignSpki;
    if (job.work == Work::Foreign) {
        const auto decoded = base64::decode(job.key);
        if (!decoded || decoded->empty()) { post({failed, "Bot: the game's login key is unreadable", job.name, job.generation}); return; }
        foreignSpki = *decoded;
    }
    auto material = prepareLogin(http, *account, foreignSpki, job.hostNetherNetId != 0, &authCache_[account->xuid]);
    if (account->refreshToken != previousRefresh && !accountStore::save(path, stored.accounts)) {
        post({failed, "Bot: cannot save refreshed account", job.name, job.generation}); return;
    }
    if (!material.value) { post({failed, "Bot join: " + material.error, job.name, job.generation}); return; }
    std::string nonce;
    if ((job.work == Work::JoinExtra || job.work == Work::Foreign) && job.hostNetherNetId) {
        const auto asked = std::chrono::steady_clock::now();
        Rta rta;
        std::string rtaError;
        std::optional<mpsd::ActiveMember> active;
        if (auto connection = rta.connectionId(material.value->xboxLiveAuthorization, rtaError); !connection.empty())
            active = mpsd::ActiveMember{std::move(connection), crypto::newUuid()};
        else log().warn(L"Bot: {} cannot connect to Xbox RTA ({}); joining the host's Xbox session as an inactive member (the join nonce comes late)",
            std::wstring(account->name.begin(), account->name.end()), std::wstring(rtaError.begin(), rtaError.end()));
        bool told = false;
        auto got = mpsd::joinAndGetNonce(http, material.value->xboxLiveAuthorization, account->xuid, job.hostNetherNetId,
            [&](std::chrono::milliseconds delay) {
                if (!told && std::chrono::steady_clock::now() - asked >= std::chrono::seconds(10)) {
                    told = true;
                    post({Delivery::Notice, "Bot " + account->name + ": waiting for the host to hand out a join nonce (this can take about a minute)", {}, 0});
                }
                return pauseUntil(std::chrono::steady_clock::now() + delay);
            }, active ? &*active : nullptr);
        rta.close();
        const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - asked).count();
        if (!got.value) {
            log().warn(L"Bot: {} got no join nonce from the host's Xbox session after {} ms{}", std::wstring(account->name.begin(), account->name.end()), took,
                active ? L" (as an active member)" : L"");
            post({failed, "Bot join: " + got.error, job.name, job.generation}); return;
        }
        log().info(L"Bot: {} joined the host's Xbox session{} and got a join nonce ({} ms)", std::wstring(account->name.begin(), account->name.end()),
            active ? L" as an active member" : L"", took);
        nonce = std::move(*got.value);
    }
    if (job.work == Work::Foreign) {
        if (material.value->foreignToken.empty()) { post({failed, "Bot: no token for the game's key", job.name, job.generation}); return; }
        post({Delivery::ForeignReady, {}, account->name, job.generation, std::move(material.value->foreignToken), {}, {}, std::move(nonce)});
        return;
    }
    auto data = defaultClientData(material.value->gamertag, material.value->playFabId, job.address,
        account->mcDeviceId, stableUuid("SelfSignedId:" + account->xuid + ":" + account->mcDeviceId));
    if (!nonce.empty()) data["Nonce"] = nonce;
    const auto jwt = clientDataJwt(material.value->loginKey, data);
    const auto request = connectionRequest(loginJson(material.value->token), jwt);
    if (data.empty() || jwt.empty() || request.empty()) {
        post({failed, "Bot: cannot build login request", job.name, job.generation}); return;
    }
    post({Delivery::PreparedExtra, {request.begin(), request.end()}, account->name, job.generation, material.value->token,
          std::make_shared<crypto::EcKey>(std::move(material.value->loginKey))});
}
BotManager::Delivery BotManager::failureFor(Work work)
{
    if (work == Work::Foreign) return Delivery::ForeignFailed;
    if (work == Work::Skin) return Delivery::SkinFailed;
    return work == Work::JoinExtra ? Delivery::Failed : Delivery::Notice;
}
std::optional<guest::Login> BotManager::guestLogin(const std::string& name, const Extra* host, std::string& error)
{
    if (host && !host->user) {
        if (!host->guestHost.known()) { error = host->name() + "'s login is unknown"; return std::nullopt; }
        auto login = guest::fromHost(host->guestHost, name);
        if (!login) error = "cannot build the login request";
        return login;
    }
    auto login = guest::fromGame(host ? net::userRequest() : net::sentRequest(), gamekey::sign);
    if (!login) error = "your login is unknown; inject Tsukuyomi before joining the server";
    return login;
}
void BotManager::prepareSeat(bool relog, const std::string& name, std::uint64_t generation, const Extra* host)
{
    std::string error;
    auto login = guestLogin(name, host, error);
    if (!login) { post({relog ? Delivery::RelogFailed : Delivery::Failed, "Bot: " + name + " cannot join (" + error + ")", name, generation}); return; }
    Result result{relog ? Delivery::RelogReady : Delivery::SeatReady, {}, name, generation};
    result.seat = std::move(*login);
    post(std::move(result));
}
BotManager::Extra* BotManager::findExtra(const std::string& name)
{
    for (auto& extra : extras_) if (!extra->user && sameName(extra->name(), name)) return extra.get();
    return nullptr;
}
bool BotManager::isActive(const std::string& name)
{
    if (findBot(name) || (possessing_ && sameName(possessed_, name))) return true;
    if (!gameAccount_.empty() && sameName(gameAccount_, name)) return true;
    if (extraRelog_.phase != ExtraRelog::Phase::None && extraRelog_.toBot() && sameName(extraRelog_.target, name)) return true;
    for (const auto& extra : extras_)
        for (std::size_t sub = extra->user ? 1 : 0; sub < net::kSubCount; ++sub)
            if (occupied(extra->slots[sub]) && sameName(extra->slots[sub].name, name)) return true;
    return false;
}
bool BotManager::classicFor(const std::string& name) const { return !name.empty() && classic_.contains(lowerKey(name)); }
chatcommand::Reply BotManager::joinSub(std::string name, std::uint8_t sub)
{
    if (sub == 0 || sub >= net::kSubCount) return {{"Bot: the seat number is 1 to 3"}, true};
    const std::string hostLabel = gameAccount_.empty() ? std::string(kUserLabel) : gameAccount_;
    if (occupied(slots_[sub])) return {{"Bot: seat " + std::to_string(sub) + " of " + hostLabel + " is in use by " + slots_[sub].name}, true};
    if (possessing_ && sub == possessedSub_)
        return {{"Bot: seat " + std::to_string(sub) + " of " + hostLabel + " is the bot you are controlling (" + possessed_ + ")"}, true};
    if (botCount() == 0 && !possessing_) {
        markMainPlayer();
        dropped_ = 0; receivedIds_ = {}; sentIds_ = {}; loggedReceived_ = 0; loggedSent_ = 0;
    }
    if (!possessing_ && !net::mainStartGame()) if (const auto local = readLocalPlayerIds()) net::setKeeperSelf(0, local->runtimeId);
    auto& slot = slots_[sub];
    slot = {};
    slot.name = std::move(name); slot.session.prepare(); slot.generation = ++generation_;
    prepareSeat(false, slot.name, slot.generation, nullptr);
    return {{"Bot: preparing login for " + slot.name + " (seat " + std::to_string(sub) + ")"}, false};
}
chatcommand::Reply BotManager::joinExtra(std::string name)
{
    if (!gameconn::available() || gameconn::protocolVersion() <= 0)
        return {{"Bot: bots on their own connections are unavailable in this game version (up to 3 bots share your connection)"}, true};
    if (GameData::instance().integratedServer()) return {{"Bot: bots on their own connections need a server, not a local world (up to 3 bots share your connection)"}, true};
    void* const mainPeer = net::mainPeer();
    if (!mainPeer) return {{"Bot: join a world with the main player first"}, true};
    ExtraConnection::Target target;
    target.mainPeer = mainPeer;
    std::string address;
    if (const auto server = net::serverAddress()) address = server->host + ":" + std::to_string(server->port);
    std::uint64_t hostNetherNetId = 0;
    if (nethernet::available() && nethernet::isNetherNet(mainPeer)) {
        target.kind = ExtraConnection::Kind::NetherNet;
        if (address.empty())
            if (const auto host = nethernet::serverId(mainPeer); host && host->type == 1) hostNetherNetId = host->id;
    } else if (raknet::available() && raknet::isRakNet(mainPeer)) {
        const auto remote = raknet::remoteAddress(mainPeer);
        if (!remote) return {{"Bot: cannot read the server's address from your connection"}, true};
        target.kind = ExtraConnection::Kind::RakNet;
        target.address = *remote;
        if (address.empty()) address = (remote->host.find(':') != std::string::npos ? "[" + remote->host + "]" : remote->host) + ":" + std::to_string(remote->port);
    } else return {{"Bot: your connection is neither NetherNet nor RakNet; bots on their own connections are unavailable here"}, true};
    auto extra = std::make_unique<Extra>();
    extra->target = target;
    extra->hostNetherNetId = hostNetherNetId;
    extra->address = address;
    auto& primary = extra->slots[0];
    primary.name = std::move(name);
    primary.generation = ++generation_;
    primary.session.prepare();
    enqueue({Work::JoinExtra, primary.name, primary.generation, address, {}, {}, hostNetherNetId});
    const auto reply = "Bot: preparing login for " + primary.name + " (own connection, "
        + (target.kind == ExtraConnection::Kind::RakNet ? "RakNet" : "NetherNet") + ")";
    extras_.push_back(std::move(extra));
    return {{reply}, false};
}
chatcommand::Reply BotManager::joinExtraSub(std::string name, Extra& host, std::uint8_t sub)
{
    const std::string hostLabel = host.user ? std::string(kUserLabel) : host.name();
    if (host.slots[0].session.state() != State::Spawned) return {{"Bot: " + hostLabel + " has not spawned yet"}, true};
    if (sub == 0 || sub >= net::kSubCount) return {{"Bot: the seat number is 1 to 3"}, true};
    if (occupied(host.slots[sub])) return {{"Bot: seat " + std::to_string(sub) + " of " + hostLabel + " is in use by " + host.slots[sub].name}, true};
    auto& slot = host.slots[sub];
    slot = {};
    slot.name = std::move(name); slot.session.prepare(); slot.generation = ++generation_;
    prepareSeat(false, slot.name, slot.generation, &host);
    return {{"Bot: preparing login for " + slot.name + " (seat " + std::to_string(sub) + " of " + hostLabel + ")"}, false};
}
chatcommand::Reply BotManager::joinSeat(const std::string& hostName, std::uint8_t sub)
{
    std::string name = guestKey(hostName, sub);
    const std::string hostLabel = hostName.empty() ? std::string(kUserLabel) : hostName;
    if (const auto rider = std::find_if(riders_.begin(), riders_.end(), [&](const Rider& r) {
            return r.sub == sub && (hostName.empty() ? r.host.empty() : sameName(r.host, hostName)); });
        rider != riders_.end())
        return {{"Bot: seat " + std::to_string(sub) + " of " + hostLabel + " is kept for " + rider->name + " (it joins again after switching)"}, true};
    const bool onGame = hostName.empty() ? gameAccount_.empty() : sameName(hostName, gameAccount_);
    if (onGame) return joinSub(std::move(name), sub);
    Extra* const host = hostName.empty() ? userExtra() : findExtra(hostName);
    if (!host) {
        if (hostName.empty()) return {{"Bot: your account is not on the server yet"}, true};
        if (isActive(hostName)) return {{"Bot: " + hostName + " is in a seat; name a bot that joined with join <name> 0"}, true};
        return {{"Bot: " + hostName + " is not active (use your gamertag to share your account's connection)"}, true};
    }
    return joinExtraSub(std::move(name), *host, sub);
}
std::optional<std::string> BotManager::seatOccupant(const std::string& hostName, std::uint8_t sub)
{
    if (const auto guest = guestKey(hostName, sub); isActive(guest)
        || std::any_of(riders_.begin(), riders_.end(), [&](const Rider& r) { return sameName(r.name, guest); }))
        return guest;
    const bool onGame = hostName.empty() ? gameAccount_.empty() : sameName(hostName, gameAccount_);
    if (onGame) {
        if (occupied(slots_[sub])) return slots_[sub].name;
        if (possessing_ && sub == possessedSub_) return possessed_;
    } else if (Extra* const host = hostName.empty() ? userExtra() : findExtra(hostName); host && occupied(host->slots[sub]))
        return host->slots[sub].name;
    for (const auto& rider : riders_)
        if (rider.sub == sub && (hostName.empty() ? rider.host.empty() : sameName(rider.host, hostName))) return rider.name;
    return std::nullopt;
}
void BotManager::leaveExtraSub(Extra& extra, std::uint8_t sub, bool send)
{
    if (sub == 0 || sub >= net::kSubCount) return;
    auto& slot = extra.slots[sub];
    if (send && online(slot)) {
        if (auto packet = codec().disconnect(sub); !packet.empty()) { extra.connection.send({packet}); extra.connection.flush(); }
    }
    slot = {};
}
void BotManager::leaveExtra(std::size_t index, bool send, bool keepSession)
{
    if (index >= extras_.size()) return;
    auto& extra = *extras_[index];
    for (std::uint8_t sub = net::kSubCount - 1; sub > 0; --sub) leaveExtraSub(extra, sub, send);
    bool linger = false;
    if (send && online(extra.slots[0])) {
        if (auto packet = codec().disconnect(0); !packet.empty()) { extra.connection.send({packet}); extra.connection.flush(); linger = true; }
    }
    if (extra.hostNetherNetId && !extra.name().empty() && !keepSession)
        enqueue({Work::LeaveSession, extra.name(), 0, {}, {}, {}, extra.hostNetherNetId});
    if (linger) lingering_.push_back({std::move(extras_[index]), steadyMs() + kExtraLingerMs});
    else extra.connection.close("left");
    extras_.erase(extras_.begin() + static_cast<std::ptrdiff_t>(index));
}
void BotManager::closeLingering(bool all)
{
    const auto now = steadyMs();
    std::erase_if(lingering_, [&](Lingering& l) {
        if (!all && now < l.until) return false;
        l.extra->connection.close("left");
        return true;
    });
}
void BotManager::leaveExtras(bool send)
{
    while (!extras_.empty()) leaveExtra(extras_.size() - 1, send);
    mainGoneSince_ = 0;
}
void BotManager::pumpExtras()
{
    closeLingering(false);
    const auto now = steadyMs();
    for (std::size_t i = 0; i < extras_.size();) {
        auto& extra = *extras_[i];
        auto& primary = extra.slots[0];
        if (primary.session.state() == State::Preparing) { ++i; continue; }
        for (const auto& packet : extra.connection.pump(now)) {
            const auto header = protocol::readHeader(protocol::bytes(packet));
            if (!header || header->recipient >= net::kSubCount) continue;
            const auto body = protocol::bytes(packet).subspan(header->bodyOffset);
            if (header->recipient == 0) {
                if (header->id == 0x0b) {
                    if (const auto start = possess::readStartGameHead(body)) {
                        extra.keeper.setSelf(start->runtimeId);
                        extra.dimension = start->dimension;
                    }
                } else if (header->id == 0x3d) {
                    if (const auto change = codec().readChangeDimension(body)) extra.dimension = change->dimension;
                }
                if (possess::StateKeeper::wants(header->id)) {
                    extra.keeper.observe(header->id, body);
                }
            }
            auto& slot = extra.slots[header->recipient];
            if (const auto* const terrainOwner = gameterrain::owner()) {
                const bool ownerHere = std::any_of(extra.slots.begin(), extra.slots.end(), [&](const Slot& s) { return &s.session == terrainOwner; });
                if (header->id == 0xae && &slot.session == terrainOwner) { terrainExtra_.push_back(packet); continue; }
                if (header->id == 0x88 && header->recipient == 0 && ownerHere) terrainExtra_.push_back(packet);
            }
            if (!online(slot)) continue;
            ++slot.received;
            slot.session.onPacket(header->id, body);
        }
        const bool closed = extra.connection.phase() == ExtraConnection::Phase::Closed;
        for (std::uint8_t sub = 0; sub < net::kSubCount; ++sub) {
            auto& slot = extra.slots[sub];
            if (closed && online(slot)) slot.session.fail(extra.connection.reason());
            if (!closed && online(slot)) slot.session.onTick(now);
            const auto outgoing = slot.session.takeOutgoing();
            if (!closed && !outgoing.empty()) extra.connection.send(outgoing);
        }
        extra.connection.flush();
        for (std::uint8_t sub = 0; sub < net::kSubCount; ++sub) {
            auto& slot = extra.slots[sub];
            const std::string label = sub == 0 ? slot.name + " (own connection)" : slot.name + " (seat of " + primary.name + ")";
            for (const auto& event : slot.session.takeEvents()) {
                log().info(L"Bot: {}: {}", std::wstring(label.begin(), label.end()), std::wstring(event.begin(), event.end()));
                pendingChat_.push_back("[Tsukuyomi] Bot " + slot.name + ": " + event);
            }
            for (const auto& note : slot.session.takeNotes())
                log().info(L"Bot: {}: {}", std::wstring(label.begin(), label.end()), std::wstring(note.begin(), note.end()));
            if (sub != 0 && slot.session.state() == State::Disconnected) slot = {};
        }
        if (closed || primary.session.state() == State::Disconnected) {
            for (std::uint8_t sub = 1; sub < net::kSubCount; ++sub)
                if (occupied(extra.slots[sub])) pendingChat_.push_back("[Tsukuyomi] Bot " + extra.slots[sub].name + " left with " + primary.name);
            leaveExtra(i, false);
            continue;
        }
        ++i;
    }
}
BotManager::Slot* BotManager::findBot(const std::string& name)
{
    for (std::uint8_t sub = 1; sub < net::kSubCount; ++sub)
        if (occupied(slots_[sub]) && sameName(slots_[sub].name, name)) return &slots_[sub];
    return nullptr;
}
std::size_t BotManager::botCount() const
{
    return static_cast<std::size_t>(std::count_if(slots_.begin() + 1, slots_.end(), [](const Slot& slot) { return occupied(slot); }));
}
void BotManager::leaveSlot(std::uint8_t sub, bool send)
{
    auto& slot = slots_[sub];
    if (send && online(slot) && !sendBotDisconnect(sub))
        clientchat::printLocal("[Tsukuyomi] Bot: disconnect could not be sent");
    slot = {};
    net::deactivate(sub);
}
void BotManager::leaveAll(bool send)
{
    inventoryView_.abandon();
    const bool closeGame = send && (possessing_ || !gameAccount_.empty() || extraRelog_.phase == ExtraRelog::Phase::Rejoining);
    bool unsent = false;
    for (std::uint8_t sub = 0; sub < net::kSubCount; ++sub)
        if (send && online(slots_[sub]) && !sendBotDisconnect(sub)) unsent = true;
    if (unsent) clientchat::printLocal("[Tsukuyomi] Bot: disconnect could not be sent");
    gameterrain::end(true);
    net::setTerrainRoute(-1, false);
    terrainExtra_.clear();
    for (auto& slot : slots_) slot = {};
    net::deactivateAll();
    possessing_ = false; possessed_.clear(); net::setPumpWanted(false);
    leaveExtras(send);
    endGameAccount();
    riders_.clear(); pendingPossess_ = {}; homes_.clear();
    if (closeGame) net::schedule(gamecodec::disconnectToGame("Tsukuyomi stopped while you were controlling the bot"), 0);
}
void BotManager::markMainPlayer()
{
    playerSerial_ = GameData::instance().playerSerial();
    mainPlayer_ = GameData::instance().player();
    mainLogin_ = net::loginCount();
}
bool BotManager::mainPlayerReplaced()
{
    const auto serial = GameData::instance().playerSerial();
    if (serial == playerSerial_) return false;
    if (GameData::instance().player() == mainPlayer_ && net::loginCount() == mainLogin_) {
        if (loggedRegrab_ < 4) {
            ++loggedRegrab_;
            log().info(L"Bot: the main player was re-grabbed (serial {} -> {}, same player and login); bots stay", playerSerial_, serial);
        }
        playerSerial_ = serial;
        return false;
    }
    return true;
}
void BotManager::releaseDisconnected()
{
    for (std::uint8_t sub = 0; sub < net::kSubCount; ++sub) {
        if (slots_[sub].session.state() != State::Disconnected) continue;
        slots_[sub] = {};
        if (sub != 0) net::deactivate(sub);
    }
}
chatcommand::Reply BotManager::command(const std::vector<std::string>& args)
{
    if (!running_.load(std::memory_order_acquire)) return {{"Bot is unavailable"}, true};
    const auto words = syntax::toWords(args);
    if (!words) return {{"Bot: invalid name (check the quotes)"}, true};
    const auto parsed = syntax::parse(*words);
    if (!parsed.command) return {parsed.error, true};
    const auto& c = *parsed.command;
    using syntax::Verb;
    std::lock_guard lock(sessionMutex_);
    const bool action = c.verb == Verb::Walk || c.verb == Verb::Follow || c.verb == Verb::Stop || c.verb == Verb::Build
        || c.verb == Verb::Eat || c.verb == Verb::Use || c.verb == Verb::Hotbar || c.verb == Verb::Inventory;
    if ((action || c.verb == Verb::JoinAccount || c.verb == Verb::JoinGuest) && !gamecodec::instance())
        return {{"Bot: the game's packet functions are unavailable (see Tsukuyomi.log)"}, true};
    if (action) return actionCommand(c);
    const bool busy = relog_.phase != Relog::Phase::None;
    switch (c.verb) {
    case Verb::Status: return statusCommand();
    case Verb::Login: {
        { std::lock_guard workLock(workMutex_);
          if (loginBusy_) return {{"Bot login is already running"}, true};
          loginBusy_ = true; }
        enqueue({Work::Login, {}, 0});
        return {{"Bot: requesting device code"}, false};
    }
    case Verb::Accounts: enqueue({Work::Accounts, {}, 0}); return {};
    case Verb::Logout: enqueue({Work::Logout, c.account, 0}); return {};
    case Verb::JoinAccount: return joinAccountCommand(c, busy);
    case Verb::JoinGuest: return joinGuestCommand(c, busy);
    case Verb::LeaveAll: return leaveAllCommand(busy);
    default: break;
    }
    std::string target;
    if (c.target) {
        const auto resolved = resolveTarget(*c.target);
        if (resolved.error) return *resolved.error;
        target = resolved.name;
    }
    if (c.verb == Verb::Switch && (!gameAccount_.empty() || extraRelog_.phase != ExtraRelog::Phase::None)) {
        if (extraRelog_.phase != ExtraRelog::Phase::None) return {{"Bot: switching is in progress"}, true};
        if (possessing_) {
            if (!target.empty() && sameName(target, possessed_)) return {{"Bot: you are already controlling " + possessed_}, true};
            if (!target.empty() && sameName(target, gameAccount_))
                return {{"Bot: switch back to your account first (/tk bot switch), then to " + gameAccount_}, true};
        }
        if (target.empty() || sameName(target, gameAccount_)) return extraRelogSwitch({});
        if (findExtra(target)) return extraRelogSwitch(target);
        if (isActive(target)) {
            pendingPossess_ = {target, steadyMs()};
            auto reply = extraRelogSwitch({});
            if (reply.error) { pendingPossess_ = {}; return reply; }
            reply.lines.push_back("Bot: rejoining as your account first, then switching to " + target);
            return reply;
        }
        return {{"Bot: " + target + " is not active"}, true};
    }
    if (c.verb == Verb::Switch) {
        Extra* const extra = target.empty() ? nullptr : findExtra(target);
        if (extra) {
            if (relog_.phase != Relog::Phase::None) return {{"Bot: switching is in progress"}, true};
            return extraRelogSwitch(extra->name());
        }
        return relogSwitch(target);
    }
    if (target.empty()) return {{"Bot: that is your account; name a bot (or leaveall)"}, true};
    if (busy) return {{"Bot: switching is in progress"}, true};
    if (possessing_ && sameName(possessed_, target))
        return {{"Bot: you are controlling " + possessed_ + "; switch back to your account first (/tk bot switch)"}, true};
    setHome(target, {});
    if (const auto rider = std::find_if(riders_.begin(), riders_.end(), [&](const Rider& r) { return sameName(r.name, target); });
        rider != riders_.end()) {
        const auto name = rider->name;
        riders_.erase(rider);
        return {{"Bot: " + name + " left"}, false};
    }
    for (std::size_t i = 0; i < extras_.size(); ++i) {
        auto& extra = *extras_[i];
        if (!extra.user && sameName(extra.name(), target)) {
            const auto name = extra.name();
            std::string riders;
            for (std::uint8_t sub = 1; sub < net::kSubCount; ++sub)
                if (occupied(extra.slots[sub])) riders += (riders.empty() ? "" : ", ") + extra.slots[sub].name;
            leaveExtra(i, true);
            return {{"Bot: " + name + " left" + (riders.empty() ? std::string() : " (with " + riders + ")")}, false};
        }
        for (std::uint8_t sub = 1; sub < net::kSubCount; ++sub)
            if (occupied(extra.slots[sub]) && sameName(extra.slots[sub].name, target)) {
                const auto name = extra.slots[sub].name;
                leaveExtraSub(extra, sub, true);
                return {{"Bot: " + name + " left"}, false};
            }
    }
    Slot* const slot = findBot(target);
    if (!slot) return {{"Bot: " + target + " is not active"}, true};
    const auto name = slot->name;
    leaveSlot(static_cast<std::uint8_t>(slot - slots_.data()), true);
    return {{"Bot: " + name + " left"}, false};
}
chatcommand::Reply BotManager::statusCommand()
{
    std::vector<std::string> lines;
    const auto seat = [this](const std::string& name) {
        return "Bot " + name + (classicFor(name) ? " (classic)" : "");
    };
    if (possessing_)
        lines.push_back("Bot: you are controlling " + possessed_ + "; "
            + (gameAccount_.empty() ? std::string("your account") : gameAccount_) + ": "
            + (occupied(slots_[0]) ? std::string(stateName(slots_[0].session.state())) : std::string("(gone)")));
    for (std::uint8_t sub = 1; sub < net::kSubCount; ++sub) {
        const auto& slot = slots_[sub];
        if (!occupied(slot)) continue;
        lines.push_back(seat(slot.name) + ": " + stateName(slot.session.state()) + ", received " + std::to_string(slot.received));
    }
    for (const auto& extra : extras_) {
        const auto& primary = extra->slots[0];
        if (!extra->user)
            lines.push_back("Bot " + primary.name + " (own connection, " + (extra->target.kind == ExtraConnection::Kind::RakNet ? "RakNet" : "NetherNet")
                + (classicFor(primary.name) ? ", classic" : "") + "): " + stateName(primary.session.state()) + ", received " + std::to_string(primary.received));
        for (std::uint8_t sub = 1; sub < net::kSubCount; ++sub) {
            const auto& slot = extra->slots[sub];
            if (!occupied(slot)) continue;
            lines.push_back(seat(slot.name) + ": " + stateName(slot.session.state()) + ", received " + std::to_string(slot.received));
        }
    }
    if (!gameAccount_.empty()) {
        const Extra* const user = userExtra();
        lines.push_back((possessing_ ? std::string("Bot: your account: ") : "Bot: you are " + gameAccount_ + " (rejoined); your account: ")
            + (user ? std::string(stateName(user->slots[0].session.state())) + " on its own connection" : std::string("(gone)")));
    }
    for (const auto& rider : riders_)
        lines.push_back("Bot " + rider.name + ": waiting to rejoin in a seat of " + (rider.host.empty() ? std::string(kUserLabel) : rider.host));
    if (!pendingPossess_.name.empty()) lines.push_back("Bot: switching to " + pendingPossess_.name + " after getting it ready");
    if (extraRelog_.phase == ExtraRelog::Phase::Preparing) lines.push_back("Bot: preparing to rejoin");
    else if (extraRelog_.phase == ExtraRelog::Phase::Rejoining) lines.push_back("Bot: rejoining");
    if (relog_.phase == Relog::Phase::Preparing) lines.push_back("Bot: preparing to rejoin");
    else if (relog_.phase == Relog::Phase::Rejoining) lines.push_back("Bot: rejoining");
    if (lines.empty()) lines.push_back("Bot: no bot is active");
    if (dropped_) lines.push_back("Bot: dropped " + std::to_string(dropped_) + " received packet(s)");
    return {lines, false};
}
chatcommand::Reply BotManager::leaveAllCommand(bool busy)
{
    if (busy || extraRelog_.phase != ExtraRelog::Phase::None) return {{"Bot: switching is in progress"}, true};
    if (possessing_ || !gameAccount_.empty())
        return {{"Bot: you are controlling a bot; switch back to your account first (/tk bot switch)"}, true};
    std::vector<std::string> names;
    for (std::uint8_t sub = 1; sub < net::kSubCount; ++sub) if (occupied(slots_[sub])) names.push_back(slots_[sub].name);
    for (const auto& extra : extras_)
        for (std::uint8_t sub = extra->user ? 1 : 0; sub < net::kSubCount; ++sub)
            if (occupied(extra->slots[sub])) names.push_back(extra->slots[sub].name);
    for (const auto& rider : riders_) names.push_back(rider.name);
    if (names.empty()) return {{"Bot: no bot is active"}, true};
    for (const auto& name : names) setHome(name, {});
    leaveAll(true);
    std::string list;
    for (const auto& name : names) list += (list.empty() ? "" : ", ") + name;
    return {{"Bot: " + list + " left"}, false};
}
BotManager::ResolvedTarget BotManager::resolveTarget(const syntax::Target& target)
{
    const std::string& me = userGamertag();
    const bool isMe = !me.empty() && sameName(target.name, me);
    if (target.sub < 0 || target.sub >= static_cast<std::int32_t>(net::kSubCount))
        return {{}, chatcommand::Reply{{"Bot: the number is 0 (the account itself) or 1 to 3 (its seats)"}, true}};
    if (target.sub == 0) {
        if (isMe) return {};
        return {target.name, std::nullopt};
    }
    const auto sub = static_cast<std::uint8_t>(target.sub);
    const std::string host = isMe ? std::string() : target.name;
    if (const auto occupant = seatOccupant(host, sub)) return {*occupant, std::nullopt};
    std::string hint;
    if (!isMe && me.empty() && !findExtra(host) && !sameName(host, gameAccount_))
        hint = " (your own gamertag is unknown: inject before entering the world to use it)";
    return {{}, chatcommand::Reply{{"Bot: no bot is in seat " + std::to_string(sub) + " of " + (isMe ? std::string(kUserLabel) : host) + hint}, true}};
}
const std::string& BotManager::userGamertag()
{
    if (userGamertagLogin_ != net::loginCount()) {
        userGamertagLogin_ = net::loginCount();
        userGamertag_ = lowerKey(readUserGamertag());
    }
    return userGamertag_;
}
chatcommand::Reply BotManager::actionCommand(const syntax::Command& c)
{
    using syntax::Verb;
    if (relog_.phase != Relog::Phase::None || extraRelog_.phase != ExtraRelog::Phase::None)
        return {{"Bot: switching is in progress"}, true};
    const auto resolved = resolveTarget(*c.target);
    if (resolved.error) return *resolved.error;
    if (resolved.name.empty()) return {{"Bot: that is your account; name a bot"}, true};
    const auto& name = resolved.name;
    if (c.verb == Verb::Inventory && inventoryView_.isOpen()) {
        const auto opened = inventoryView_.botName();
        inventoryView_.close(true, inventorySession());
        return {{std::string(inventoryView_.isOpen() ? "Bot: closing " : "Bot: closed ") + opened + "'s inventory"}, false};
    }
    if (possessing_ && sameName(possessed_, name)) return {{"Bot: you are controlling " + name}, true};
    Slot* slot = findBot(name);
    if (!slot) {
        for (auto& extra : extras_) {
            for (std::size_t sub = 0; sub < extra->slots.size(); ++sub) {
                if (sub == 0 && extra->user) continue;
                auto& candidate = extra->slots[sub];
                if (occupied(candidate) && sameName(candidate.name, name)) { slot = &candidate; break; }
            }
            if (slot) break;
        }
    }
    if (!slot) return {{"Bot: " + name + " is not active"}, true};
    auto& session = slot->session;
    if (session.classic()) return {{"Bot: " + name + " uses the classic connection"}, true};
    if (!session.canAct()) return {{"Bot: " + name + " cannot act before spawning or while dead"}, true};
    const syntax::Origin origin{session.feet(), session.pitch(), session.yaw()};
    std::optional<std::string> reason;
    std::string accepted;
    switch (c.verb) {
    case Verb::Hotbar:
        if (c.slot < 1 || c.slot > 9) return {{"Bot: the hotbar slot is 1 to 9"}, true};
        reason = session.selectSlot(static_cast<std::uint8_t>(c.slot - 1));
        accepted = "holding hotbar slot " + std::to_string(c.slot);
        break;
    case Verb::Inventory: return inventoryView_.open(slot->name, session, steadyMs());
    case Verb::Follow:
        if (!gamepath::available()) return {{"Bot: the game's pathfinder is unavailable (see Tsukuyomi.log)"}, true};
        reason = session.follow();
        accepted = "following you";
        break;
    case Verb::Walk: {
        const auto target = syntax::resolve(c.position, origin);
        const bool path = c.walkMode == syntax::WalkMode::Path;
        if (path && !gamepath::available()) return {{"Bot: the game's pathfinder is unavailable (see Tsukuyomi.log); use straight"}, true};
        reason = session.walkTo(target, path ? WalkMode::Path : WalkMode::Straight);
        accepted = path ? std::format("walking to ({}, {}, {}) along a path", std::floor(target.x), std::floor(target.y), std::floor(target.z))
                        : std::format("walking to ({}, {})", target.x, target.z);
        break;
    }
    case Verb::Stop: session.stopActions(); accepted = "stopping"; break;
    case Verb::Build: {
        const auto block = syntax::resolveBlock(c.position, origin);
        reason = session.build({static_cast<float>(block[0]), static_cast<float>(block[1]), static_cast<float>(block[2])}, c.face);
        accepted = std::format("clicking the {} face of ({}, {}, {})", syntax::kFaces[c.face], block[0], block[1], block[2]);
        break;
    }
    case Verb::Eat: case Verb::Use: {
        const bool eat = c.verb == Verb::Eat;
        reason = session.useItem(eat); accepted = eat ? "eating" : "using";
        break;
    }
    default: return {syntax::usage({}), true};
    }
    if (reason) return {{"Bot " + slot->name + ": " + *reason}, true};
    return {{"Bot " + slot->name + ": " + accepted}, false};
}
std::optional<chatcommand::Reply> BotManager::joinBlocked(bool busy)
{
    if (busy) return chatcommand::Reply{{"Bot: switching is in progress"}, true};
    if (!net::connected()) return chatcommand::Reply{{"Bot: join a world with the main player first"}, true};
    return std::nullopt;
}
chatcommand::Reply BotManager::joinAccountCommand(const syntax::Command& c, bool busy)
{
    const std::string& me = userGamertag();
    if (isGuest(c.account)) return {{"Bot: " + c.account + " is not an account name"}, true};
    if (!me.empty() && sameName(c.account, me)) return {{"Bot: " + c.account + " is your account"}, true};
    if (auto blocked = joinBlocked(busy)) return *blocked;
    if (isActive(c.account)) return {{"Bot: " + c.account + " is already active"}, true};
    if (c.classic) classic_.insert(lowerKey(c.account)); else classic_.erase(lowerKey(c.account));
    return joinExtra(c.account);
}
chatcommand::Reply BotManager::joinGuestCommand(const syntax::Command& c, bool busy)
{
    if (c.seat < 1 || c.seat >= static_cast<std::int32_t>(net::kSubCount))
        return {{"Bot: the number is 0 (the account itself on its own connection) or 1 to 3 (a seat)"}, true};
    const auto seat = static_cast<std::uint8_t>(c.seat);
    const std::string& me = userGamertag();
    std::string hostName = me.empty() || !sameName(c.host, me) ? c.host : std::string();
    if (me.empty() && !sameName(hostName, gameAccount_) && !findExtra(hostName))
        return {{"Bot: " + c.host + " is not a bot on its own connection (your own gamertag is unknown: inject before entering the world to use it)"}, true};
    if (!hostName.empty()) {
        if (sameName(hostName, gameAccount_)) hostName = gameAccount_;
        else if (const Extra* const host = findExtra(hostName)) hostName = host->name();
    }
    if (auto blocked = joinBlocked(busy)) return *blocked;
    const std::string name = guestKey(hostName, seat);
    if (isActive(name)) return {{"Bot: " + name + " is already active"}, true};
    if (c.classic) classic_.insert(lowerKey(name)); else classic_.erase(lowerKey(name));
    return joinSeat(hostName, seat);
}
void BotManager::watchMainGone()
{
    if (!extras_.empty() && relog_.phase == Relog::Phase::None && extraRelog_.phase == ExtraRelog::Phase::None && !possessing_
        && !net::connected()) {
        if (!mainGoneSince_) mainGoneSince_ = steadyMs();
        else if (steadyMs() - mainGoneSince_ > 5000) {
            log().info(L"Bot: bots on their own connections are leaving with the main player");
            if (!gameAccount_.empty() && gameAccountHost_) enqueue({Work::LeaveSession, gameAccount_, 0, {}, {}, {}, gameAccountHost_});
            leaveExtras(true); endGameAccount(); riders_.clear(); pendingPossess_ = {};
            pendingChat_.push_back("[Tsukuyomi] Bot: bots on their own connections left with the main player");
        }
    } else mainGoneSince_ = 0;
}
void BotManager::onGameUpdate(void* game)
{
    if (!viewThread_) return;
    if (!gameUpdateChecked_) {
        gameUpdateChecked_ = true;
        gameUpdateUsable_ = GetCurrentThreadId() == viewThread_;
        if (gameUpdateUsable_) log().info(L"Bot: the game's main loop runs on the view-update thread ({})", viewThread_);
        else log().warn(L"Bot: the game's main loop runs on thread {}, not the view-update thread {} - bots stay after you leave until the game closes"
            L" and switching in a friend's world is unavailable", GetCurrentThreadId(), viewThread_);
    }
    if (!gameUpdateUsable_ || !running_.load(std::memory_order_acquire)) return;
    std::unique_lock lock(sessionMutex_, std::try_to_lock);
    if (!lock.owns_lock() || !running_.load(std::memory_order_acquire)) return;
    game_ = game;
    closeLingering(false);
    friendRejoinTick(game, steadyMs());
    rejoinTimeouts(steadyMs());
    watchMainGone();
}
void BotManager::scheduleRejoin(std::string packet, std::uint64_t friendWorld)
{
    if (friendWorld) friendRejoin_ = {FriendRejoin::Phase::Leave, friendWorld, 0};
    else net::schedule(std::move(packet), 0);
}
void BotManager::friendRejoinTick(void* game, std::uint64_t nowMs)
{
    auto& r = friendRejoin_;
    if (r.phase == FriendRejoin::Phase::None) return;
    if (r.phase == FriendRejoin::Phase::Leave) {
        net::clearRouting();
        const auto id = friendworld::idFor(game, r.netherNetId);
        if (!id) { failFriendRejoin("the world is no longer in your game's list of friends' worlds"); return; }
        friendworld::expectRejoin(r.netherNetId, GetTickCount64() + kRelogTimeoutMs);
        if (!friendworld::leaveThenJoin(game, *id)) { failFriendRejoin("the game did not leave the world"); return; }
        log().info(L"Bot: the game is leaving the friend's world and joining it again (list id {})", std::wstring(id->begin(), id->end()));
        r.phase = FriendRejoin::Phase::Joining; r.since = nowMs;
        return;
    }
    if (r.phase == FriendRejoin::Phase::Joining) {
        switch (friendworld::takeLookup()) {
        case friendworld::Lookup::Matched: r = {}; return;
        case friendworld::Lookup::Missing: failFriendRejoin("the world is no longer in your game's list of friends' worlds"); return;
        case friendworld::Lookup::Pending:
            if (nowMs - r.since > kRelogTimeoutMs) failFriendRejoin("the game did not leave the world");
            return;
        }
    }
}
void BotManager::failFriendRejoin(const std::string& why)
{
    log().warn(L"Bot: rejoining the friend's world failed ({})", std::wstring(why.begin(), why.end()));
    friendRejoin_ = {};
    friendworld::cancelRejoin();
    net::armRelog(false); net::setPumpWanted(false);
    if (extraRelog_.phase != ExtraRelog::Phase::None) endGameAccount();
    relog_ = {}; extraRelog_ = {};
    clientchat::printLocal("[Tsukuyomi] Bot: switching failed (" + why + ")");
}
void BotManager::rejoinTimeouts(std::uint64_t nowMs)
{
    if (net::connected()) return;
    if (relog_.phase == Relog::Phase::Rejoining && nowMs - relog_.since >= kRelogTimeoutMs) {
        net::armRelog(false); net::setPumpWanted(false);
        if (relog_.combined) endGameAccount();
        relog_ = {};
        log().warn(L"Bot: rejoining timed out outside a world");
        clientchat::printLocal("[Tsukuyomi] Bot: rejoining timed out");
    }
    if (extraRelog_.phase == ExtraRelog::Phase::Rejoining && nowMs - extraRelog_.since >= kRelogTimeoutMs) {
        endGameAccount(); net::setPumpWanted(false);
        log().warn(L"Bot: rejoining timed out outside a world");
        clientchat::printLocal("[Tsukuyomi] Bot: rejoining timed out");
    }
}
void BotManager::onPlayerViewUpdate(bool enabled)
{
    if (!viewThread_) viewThread_ = GetCurrentThreadId();
    if (!running_.load(std::memory_order_acquire)) return;
    std::lock_guard lock(sessionMutex_);
    if (!running_.load(std::memory_order_acquire)) return;
    if (static bool tried = false; !std::exchange(tried, true)) {
        gamepath::initialize();
        if (gamepacket::initialize() && gamecodec::initialize()) setCodec(gamecodec::instance());
    }
    for (const auto& line : std::exchange(pendingChat_, {})) clientchat::printLocal(line);
    if (leaveRequested_.exchange(false, std::memory_order_acq_rel)) {
        leaveAll(true);
        leaveDone_.store(true, std::memory_order_release);
        return;
    }
    if (possessing_) markMainPlayer();
    const bool any = possessing_ || std::any_of(slots_.begin(), slots_.end(), [](const Slot& slot) { return occupied(slot); });
    if (!enabled) { if (any || !extras_.empty()) leaveAll(true); }
    else if (const bool connected = net::connected(); any && (!connected || mainPlayerReplaced())) {
        const bool sameLogin = connected && mainLogin_ == net::loginCount();
        log().info(L"Bot: leaving with the main player (connected {}, same login {}, serial {} -> {})",
            connected, sameLogin, playerSerial_, GameData::instance().playerSerial());
        leaveAll(sameLogin); clientchat::printLocal("[Tsukuyomi] Bot: left with the main player");
    }
    std::vector<Result> results;
    { std::lock_guard workLock(workMutex_); results.swap(results_); }
    for (auto& result : results) {
        if (result.delivery == Delivery::Notice) { clientchat::printLocal("[Tsukuyomi] " + result.text); continue; }
        if (result.delivery == Delivery::SkinReady || result.delivery == Delivery::SkinFailed) { onSkinResult(std::move(result)); continue; }
        if (result.delivery == Delivery::RelogReady || result.delivery == Delivery::RelogFailed) { onRelogPrepared(std::move(result), enabled); continue; }
        if (result.delivery == Delivery::ForeignReady || result.delivery == Delivery::ForeignFailed) { onForeignPrepared(std::move(result), enabled); continue; }
        if (result.delivery == Delivery::PreparedExtra || result.delivery == Delivery::Failed) {
            const auto it = std::find_if(extras_.begin(), extras_.end(), [&](const auto& extra) {
                return extra->slots[0].generation == result.generation && extra->slots[0].session.state() == State::Preparing; });
            if (it != extras_.end()) {
                auto& extra = **it;
                auto& primary = extra.slots[0];
                if (!enabled || result.delivery == Delivery::Failed) {
                    if (result.delivery == Delivery::Failed) {
                        log().warn(L"Bot: {} could not join on its own connection ({})", std::wstring(result.name.begin(), result.name.end()),
                            std::wstring(result.text.begin(), result.text.end()));
                        clientchat::printLocal("[Tsukuyomi] " + result.text);
                    }
                    extras_.erase(it);
                    continue;
                }
                primary.name = std::move(result.name);
                std::string error;
                bool started = false;
                try {
                    extra.target.mainPeer = net::mainPeer();
                    extra.guestHost = guest::Host(result.token, result.key, extra.address);
                    started = extra.connection.start(extra.target, gameconn::protocolVersion(), std::move(result.text), std::move(result.token),
                                                     std::move(result.key), steadyMs(), error);
                } catch (const std::exception& e) { error = std::string("exception: ") + e.what(); }
                if (!started) {
                    clientchat::printLocal("[Tsukuyomi] Bot " + primary.name + ": cannot open its own connection (" + error + ")");
                    extras_.erase(it);
                    continue;
                }
                primary.session.setClassic(classicFor(primary.name));
                primary.session.beginPrimary();
                continue;
            }
            if (result.delivery == Delivery::PreparedExtra) continue;
        }
        Slot* extraSlot = nullptr;
        Extra* extraHost = nullptr;
        for (auto& extra : extras_)
            for (std::uint8_t sub = 1; sub < net::kSubCount && !extraSlot; ++sub)
                if (extra->slots[sub].generation == result.generation && extra->slots[sub].session.state() == State::Preparing) {
                    extraSlot = &extra->slots[sub]; extraHost = extra.get();
                }
        if (extraSlot) {
            if (!enabled || result.delivery == Delivery::Failed || !online(extraHost->slots[0])) {
                if (result.delivery == Delivery::Failed) clientchat::printLocal("[Tsukuyomi] " + result.text);
                *extraSlot = {};
                continue;
            }
            extraSlot->name = std::move(result.name);
            extraSlot->session.setClassic(classicFor(extraSlot->name));
            extraSlot->session.begin(static_cast<std::uint8_t>(extraSlot - extraHost->slots.data()), result.seat);
            continue;
        }
        const auto found = std::find_if(slots_.begin() + 1, slots_.end(), [&](const Slot& slot) {
            return slot.generation == result.generation && slot.session.state() == State::Preparing; });
        if (found == slots_.end()) continue;
        const auto sub = static_cast<std::uint8_t>(found - slots_.begin());
        if (!enabled) { *found = {}; continue; }
        if (result.delivery == Delivery::Failed) { *found = {}; clientchat::printLocal("[Tsukuyomi] " + result.text); continue; }
        found->name = std::move(result.name);
        found->session.setClassic(classicFor(found->name));
        net::activate(sub); found->session.begin(sub, result.seat);
    }
    relogTick(steadyMs());
    extraRelogTick(steadyMs());
    riderTick(steadyMs());
    possessTick(steadyMs());
    pump();
    watchMainGone();
    pumpExtras();
    pumpPlayerView(steadyMs());
    pumpInventoryView(steadyMs());
    if (enabled) servePaths();
    if (enabled) applySkins(steadyMs());
    for (const auto& line : std::exchange(pendingChat_, {})) clientchat::printLocal(line);
}
BotSession* BotManager::inventorySession()
{
    if (!inventoryView_.isBot()) return nullptr;
    const auto& name = inventoryView_.botName();
    for (auto& slot : slots_) if (occupied(slot) && sameName(slot.name, name)) return &slot.session;
    for (auto& extra : extras_) for (auto& slot : extra->slots)
        if (occupied(slot) && sameName(slot.name, name)) return &slot.session;
    return nullptr;
}
void BotManager::pumpPlayerView(std::uint64_t now)
{
    const auto request = playerinteract::takeRequest();
    if (!request || inventoryView_.isOpen()) return;
    const auto refused = [](const chatcommand::Reply& reply) {
        if (reply.error) for (const auto& line : reply.lines) clientchat::printLocal("[Tsukuyomi] " + line);
    };
    std::optional<std::string> botName;
    const auto match = [&](const Slot& slot) {
        if (!botName && occupied(slot) && slot.session.runtimeId() == request->runtimeId) botName = slot.name;
    };
    for (const auto& slot : slots_) match(slot);
    for (const auto& extra : extras_)
        for (std::size_t sub = 0; sub < extra->slots.size(); ++sub) if (sub != 0 || !extra->user) match(extra->slots[sub]);
    if (botName) {
        log().info(L"Bot: right-clicked bot {}", std::wstring(botName->begin(), botName->end()));
        syntax::Command command;
        command.verb = syntax::Verb::Inventory;
        command.target = syntax::Target{*botName, 0};
        refused(actionCommand(command));
        return;
    }
    if (!playerinteract::stillValid(*request)) {
        clientchat::printLocal("[Tsukuyomi] Bot: the player is gone");
        return;
    }
    auto snapshot = playerinteract::snapshot(request->actor);
    if (!snapshot) {
        clientchat::printLocal("[Tsukuyomi] Bot: cannot read the player's inventory (see the log)");
        return;
    }
    const auto name = net::playerName(0, request->runtimeId).value_or("Player");
    refused(inventoryView_.openSnapshot(name, std::move(*snapshot), now));
}
void BotManager::pumpInventoryView(std::uint64_t now)
{
    containerscreen::poll();
    if (!net::connected()) inventoryView_.abandon();
    auto* session = inventorySession();
    const auto drain = [&](BotSession& bot) {
        const auto responses = bot.takeStackResponses();
        if (&bot == session && bot.canAct())
            for (const auto& body : responses) inventoryView_.onResponse(protocol::bytes(body));
    };
    for (auto& slot : slots_) drain(slot.session);
    for (auto& extra : extras_) for (auto& slot : extra->slots) drain(slot.session);
    inventoryView_.tick(session, now);
}
void BotManager::servePaths()
{
    const auto now = steadyMs();
    const auto view = GameData::instance().playerView();
    const auto chunkOf = [](float value) { return static_cast<std::int32_t>(std::floor(value / 16.0f)); };
    {
        std::optional<protocol::Vec3> leader;
        if (float x = 0, y = 0, z = 0; GameData::instance().playerFeet(x, y, z)) leader = protocol::Vec3{x, y, z};
        const auto leaderDimension = leader ? gamepath::playerDimension() : std::nullopt;
        const auto give = [&](Slot& slot) { if (online(slot) && slot.session.following()) slot.session.setLeader(leader, leaderDimension); };
        for (auto& slot : slots_) give(slot);
        for (auto& extra : extras_) for (auto& slot : extra->slots) give(slot);
    }
    if (gameterrain::active()) {
        for (const auto& whole : net::takeTerrainPackets()) gameterrain::handPacket(protocol::bytes(whole));
        for (const auto& whole : std::exchange(terrainExtra_, {})) gameterrain::handPacket(protocol::bytes(whole));
        gameterrain::pump();
        if (gameterrain::active() && now - terrainUsedAt_ > kTerrainIdleMs) gameterrain::end(true);
        if (!gameterrain::active()) net::setTerrainRoute(-1, false);
    } else terrainExtra_.clear();
    if (!gameterrain::active()) { terrainHanded_.clear(); terrainHandedOwner_ = nullptr; terrainHandedDimension_ = -1; }
    const auto hand = [&](Slot& slot, const auto& chunksNear, std::int32_t cx, std::int32_t cz, std::int32_t dimension) {
        auto* const codec = gamecodec::instance();
        auto cached = net::gameCachedBlobs();
        const auto mx = chunkOf(view.x), mz = chunkOf(view.z);
        if (const auto mine = gamepath::playerDimension())
            for (const auto& own : net::keeperChunksOf(0, mx, mz, *mine, 1))
                if (const auto blobs = codec ? codec->readLevelChunkBlobs(protocol::bytes(own)) : std::nullopt; blobs && blobs->size() == 1)
                    cached.push_back(blobs->front());
        std::size_t handed = 0, substituted = 0, present = 0, replaced = 0;
        const auto radius = net::gameRadius();
        const auto mainDimension = gamepath::playerDimension();
        const auto mainSees = [&](std::int32_t x, std::int32_t z) {
            if (radius <= 0) return true;
            if (!mainDimension || *mainDimension != dimension) return false;
            const auto dx = std::int64_t{x} - mx, dz = std::int64_t{z} - mz, inner = std::int64_t{radius} - 1;
            return dx * dx + dz * dz <= inner * inner;
        };
        for (const auto& chunk : chunksNear(cx, cz, dimension, kFedChunkRadius)) {
            protocol::Reader r(protocol::bytes(chunk));
            const auto x = r.varInt32(), z = r.varInt32();
            if (!x || !z) continue;
            if (gameterrain::hasChunk(*x, *z)) {
                if (mainSees(*x, *z) || terrainHanded_.contains({*x, *z})) { ++present; continue; }
                ++replaced;
            }
            const auto blobs = codec ? codec->readLevelChunkBlobs(protocol::bytes(chunk)) : std::nullopt;
            const bool unknown = blobs && blobs->size() == 1 && std::find(cached.begin(), cached.end(), blobs->front()) == cached.end();
            bool ok = false;
            const auto blob = unknown && !cached.empty() ? std::optional<std::uint64_t>(cached.front()) : std::nullopt;
            const auto packet = gamecodec::levelChunkToProxy(protocol::bytes(chunk), blob);
            if (const auto head = packet.empty() ? std::nullopt : protocol::readHeader(protocol::bytes(packet))) {
                ok = gameterrain::handLevelChunk(protocol::bytes(packet).subspan(head->bodyOffset));
                if (ok && blob) ++substituted;
            } else if (!blob) ok = gameterrain::handLevelChunk(protocol::bytes(chunk));
            if (ok) { ++handed; terrainHanded_.insert({*x, *z}); }
        }
        if (g_fedLogs < 32) {
            ++g_fedLogs;
            log().info(L"Bot: handed {} chunk(s) around {} ({}, {}) in dimension {} to the borrowed chunk receiver ({} with a blob the game has, {} already there, "
                       L"{} replaced outside your view; main player at chunk {}, {} with radius {}; {} blob(s) known)",
                handed, std::wstring(slot.name.begin(), slot.name.end()), cx, cz, dimension, substituted, present, replaced, mx, mz, radius, cached.size());
        }
    };
    const auto serve = [&](Slot& slot, const auto& chunksNear, int routeSub) {
        if (!online(slot)) return;
        const auto mine = gamepath::playerDimension();
        const auto botDimension = slot.session.dimension();
        const bool otherDimension = botDimension && mine && *botDimension != *mine;
        const auto isDistant = [&](protocol::Vec3 feet) {
            return GameData::instance().hasPlayerView()
                && (std::max)(std::abs(chunkOf(feet.x) - chunkOf(view.x)), std::abs(chunkOf(feet.z) - chunkOf(view.z))) > kNearChunks;
        };
        if (slot.session.wantsSurroundings() && now - slot.liquidCheckedAt >= kLiquidCheckMs) {
            slot.liquidCheckedAt = now;
            const auto position = slot.session.position();
            const protocol::Vec3 feet{position.x, position.y - 1.62f, position.z};
            std::optional<bool> liquid;
            if (!otherDimension && !isDistant(feet)) liquid = gamepath::inLiquid(feet);
            else if (gameterrain::owner() == &slot.session)
                if (const auto world = gameterrain::world()) liquid = gamepath::inLiquidIn(world->level, world->dimension, world->source, feet);
            if (liquid.value_or(false) != slot.session.inLiquid() && g_liquidLogs < 32) {
                ++g_liquidLogs;
                log().info(L"Bot: {} is {} liquid at ({:.2f}, {:.2f}, {:.2f})", std::wstring(slot.name.begin(), slot.name.end()),
                    liquid.value_or(false) ? L"in" : L"out of", feet.x, feet.y, feet.z);
            }
            slot.session.setInLiquid(liquid);
        }
        const auto request = slot.session.pathRequest();
        if (!request) return;
        const auto cx = chunkOf(request->feet.x), cz = chunkOf(request->feet.z);
        const bool distant = isDistant(request->feet);
        if (!otherDimension && !distant) {
            std::string error;
            if (auto found = gamepath::find(request->feet, request->target, request->radius, request->inLiquid, error))
                slot.session.pathFound(request->serial, std::move(found->nodes), found->complete);
            else slot.session.pathFailed(request->serial, error);
            return;
        }
        const std::int32_t dimension = botDimension ? *botDimension : mine ? *mine : -1;
        if (dimension < 0) { slot.session.pathFailed(request->serial, "cannot tell which dimension the bot is in"); return; }
        if (routeSub == 0) {
            slot.session.pathFailed(request->serial, "path for this account works only near you in your dimension (straight works anywhere)");
            return;
        }
        terrainUsedAt_ = now;
        if (slot.fedSerial != request->serial) {
            slot.fedSerial = request->serial; slot.fedAt = 0; slot.askedAt = 0;
            std::string error;
            if (!gameterrain::begin(&slot.session, dimension, request->feet, error)) {
                net::setTerrainRoute(-1, false);
                slot.session.pathFailed(request->serial, error);
                return;
            }
            if (gameterrain::owner() != terrainHandedOwner_ || gameterrain::dimension() != terrainHandedDimension_) {
                terrainHanded_.clear(); terrainHandedOwner_ = gameterrain::owner(); terrainHandedDimension_ = gameterrain::dimension();
            }
            net::setTerrainRoute(routeSub, routeSub > 0);
            hand(slot, chunksNear, cx, cz, dimension);
            slot.fedAt = now;
        }
        if (slot.fedAt) {
            const auto cy = static_cast<std::int32_t>(std::floor(request->feet.y / 16.0f));
            const auto feet = gameterrain::status(cx, cz, 1, cy - kSubChunksDown, cy + kSubChunksUp);
            const bool finished = feet && feet->chunks == 9 && feet->finished == feet->chunks;
            const bool ready = finished && feet->waiting == 0 && feet->loading == 0;
            if (finished && !ready && feet->waiting > 0 && (!slot.askedAt || now - slot.askedAt >= kAskAgainMs)) {
                const bool sent = slot.session.requestSubChunks(dimension, kFedChunkRadius, kSubChunksDown, kSubChunksUp);
                slot.askedAt = now;
                if (g_fedLogs < 32) {
                    ++g_fedLogs;
                    log().info(L"Bot: {} asked for the sub-chunks around it in dimension {} ({} waiting) {} ms after handing chunks{}",
                        std::wstring(slot.name.begin(), slot.name.end()), dimension, feet->waiting, now - slot.fedAt, sent ? L"" : L" (could not send)");
                }
            }
            if (!ready && now - slot.fedAt < kFedWaitMs) return;
            if (g_fedLogs < 32) {
                ++g_fedLogs;
                const auto around = gameterrain::status(cx, cz, kFedChunkRadius, cy - kSubChunksDown, cy + kSubChunksUp);
                log().info(L"Bot: {} the bot's terrain {} ms after handing chunks ({} of {} chunk(s) finished, sub-chunks {} loaded / {} loading / {} waiting)",
                    ready ? L"filled" : L"gave up waiting for", now - slot.fedAt, around ? around->finished : -1, around ? around->chunks : -1,
                    around ? around->loaded : -1, around ? around->loading : -1, around ? around->waiting : -1);
            }
            slot.fedAt = 0;
        }
        const auto world = gameterrain::owner() == &slot.session ? gameterrain::world() : std::nullopt;
        if (!world) { slot.session.pathFailed(request->serial, "the bot's terrain is not loaded"); return; }
        std::string error;
        if (auto found = gamepath::findIn(world->level, world->dimension, world->source, request->feet, request->target, request->radius,
                request->inLiquid, error))
            slot.session.pathFound(request->serial, std::move(found->nodes), found->complete);
        else slot.session.pathFailed(request->serial, error);
    };
    for (std::uint8_t sub = 0; sub < net::kSubCount; ++sub)
        serve(slots_[sub], [sub](std::int32_t x, std::int32_t z, std::int32_t d, std::int32_t r) { return net::keeperChunksOf(sub, x, z, d, r); }, sub);
    for (auto& extra : extras_)
        for (std::size_t sub = 0; sub < extra->slots.size(); ++sub)
            serve(extra->slots[sub], [&extra, sub](std::int32_t x, std::int32_t z, std::int32_t d, std::int32_t r) {
                return sub == 0 ? extra->keeper.chunksNear(x, z, d, r) : std::vector<std::string>{}; }, -1);
}
void BotManager::pump()
{
    auto& main = slots_[0];
    if (relog_.phase == Relog::Phase::Rejoining && relog_.toBot() && !occupied(main) && net::relogRouted()) {
        main = {}; main.session.attach(0);
        possessing_ = true; possessed_ = relog_.target; possessedSub_ = relog_.viewSub;
        log().info(L"Bot: the main account now stays without a screen (rejoined as {})", std::wstring(possessed_.begin(), possessed_.end()));
    }
    if (possessing_ && relog_.phase == Relog::Phase::None && !net::relogRouted()) {
        leaveAll(false); pendingChat_.push_back("[Tsukuyomi] Bot: the rejoined connection ended");
        return;
    }
    if (std::none_of(slots_.begin(), slots_.end(), [](const Slot& slot) { return occupied(slot); })) return;
    auto incoming = net::takeIncoming();
    dropped_ += incoming.dropped;
    for (std::size_t id = 0; id < incoming.ids.size(); ++id) {
        const auto& receipt = incoming.ids[id];
        if (!receipt.count || receivedIds_[id]) continue;
        receivedIds_[id] = true;
        if (loggedReceived_ < 64) { ++loggedReceived_; log().info(L"Bot: received id {} ({} bytes)", id, receipt.size); }
    }
    for (std::uint8_t sub = 0; sub < net::kSubCount; ++sub) slots_[sub].received += incoming.perSub[sub];
    for (const auto& packet : incoming.packets)
        if (packet.recipient < net::kSubCount) slots_[packet.recipient].session.onPacket(packet.id, protocol::bytes(packet.body));
    const auto now = steadyMs();
    for (std::uint8_t sub = 0; sub < net::kSubCount; ++sub) {
        auto& slot = slots_[sub];
        if (online(slot)) slot.session.onTick(now);
        for (const auto& packet : slot.session.takeOutgoing()) {
            if (!net::sendRaw(packet)) { slot.session.fail("send failed"); break; }
            const auto header = protocol::readHeader(protocol::bytes(packet));
            if (header && !sentIds_[header->id]) {
                sentIds_[header->id] = true;
                if (loggedSent_ < 64) { ++loggedSent_; log().info(L"Bot: sent id {} ({} bytes)", header->id, packet.size()); }
            }
        }
    }
    for (std::uint8_t sub = 0; sub < net::kSubCount; ++sub) {
        const std::string label = sub == 0 ? std::string("(your account)") : slots_[sub].name;
        for (const auto& event : slots_[sub].session.takeEvents()) {
            log().info(L"Bot: {}: {}", std::wstring(label.begin(), label.end()), std::wstring(event.begin(), event.end()));
            pendingChat_.push_back("[Tsukuyomi] Bot " + label + ": " + event);
        }
        for (const auto& note : slots_[sub].session.takeNotes())
            log().info(L"Bot: {}: {}", std::wstring(label.begin(), label.end()), std::wstring(note.begin(), note.end()));
    }
    if (relog_.phase == Relog::Phase::Rejoining && relog_.toBot() && possessing_ && main.session.state() == State::Spawned && !relog_.botLoginSent) {
        relog_.botLoginSent = true;
        const auto target = std::find_if(relog_.members.begin(), relog_.members.end(), [&](const auto& m) { return sameName(m.name, relog_.target); });
        guest::Login seat = target != relog_.members.end() ? target->seat : guest::Login();
        if (target != relog_.members.end()) {
            std::string error;
            if (auto fresh = guestLogin(target->name, nullptr, error)) seat = std::move(*fresh);
            else log().warn(L"Bot: cannot build {}'s login after rejoining ({})", std::wstring(target->name.begin(), target->name.end()), std::wstring(error.begin(), error.end()));
        }
        const auto loginPacket = seat.empty() ? std::string{} : codec().subClientLogin(relog_.viewSub, seat);
        const bool sent = !loginPacket.empty() && net::sendRaw(loginPacket);
        log().info(L"Bot: sent the seat's login after rejoining ({})", sent ? L"ok" : L"failed");
        pendingChat_.push_back(sent ? "[Tsukuyomi] Bot: you are now " + possessed_ : std::string("[Tsukuyomi] Bot: cannot send the seat's login"));
        rejoinMembers();
        relog_ = {};
    }
    releaseDisconnected();
}
void BotManager::pumpFromReceive()
{
    auto& self = instance();
    if (!self.running_.load(std::memory_order_acquire)) return;
    std::unique_lock lock(self.sessionMutex_, std::try_to_lock);
    if (!lock.owns_lock() || !self.running_.load(std::memory_order_acquire)) return;
    self.pump();
    if (self.extraRelog_.phase != ExtraRelog::Phase::None) self.pumpExtras();
}
chatcommand::Reply BotManager::relogSwitch(const std::string& name)
{
    if (relog_.phase != Relog::Phase::None) {
        if (steadyMs() - relog_.since < kRelogTimeoutMs || relog_.phase == Relog::Phase::Preparing) return {{"Bot: switching is in progress"}, true};
        net::armRelog(false); relog_ = {};
    }
    std::string target;
    if (!name.empty() && !findBot(name) && !(possessing_ && sameName(name, possessed_))
        && (isActive(name) || std::any_of(riders_.begin(), riders_.end(), [&](const Rider& r) { return sameName(r.name, name); })))
        return moveThenPossess(name);
    if (name.empty() && !possessing_) return {{"Bot: you are already on your account (name the bot to control: /tk bot switch <name>)"}, true};
    std::uint8_t viewSub = 1;
    if (!possessing_ || (!name.empty() && !sameName(name, possessed_))) {
        const Slot* const slot = findBot(name);
        if (!slot || slot->session.state() != State::Spawned) return {{"Bot: " + name + " is not spawned"}, true};
        if (!relogUuids(slot->name)) return {{"Bot: " + slot->name + " has not seen your player yet (try again in a moment)"}, true};
        target = slot->name;
        viewSub = static_cast<std::uint8_t>(slot - slots_.data());
    }
    if (std::string error; !rejoinRoute(error, game_, viewThread_)) return {{"Bot: " + error}, true};
    relog_ = {};
    relog_.phase = Relog::Phase::Preparing; relog_.target = target; relog_.generation = ++relogGeneration_; relog_.since = steadyMs();
    relog_.viewSub = viewSub;
    if (possessing_) relog_.members.push_back({possessed_, {}, false, false, possessedSub_});
    for (std::uint8_t sub = 1; sub < net::kSubCount; ++sub)
        if (online(slots_[sub])) relog_.members.push_back({slots_[sub].name, {}, false, false, sub});
    for (const auto& member : relog_.members) prepareSeat(true, member.name, relog_.generation, nullptr);
    return {{relog_.toBot() ? "Bot: preparing to rejoin as " + target : std::string("Bot: preparing to rejoin as your account")}, false};
}
void BotManager::onRelogPrepared(Result result, bool enabled)
{
    if (relog_.phase != Relog::Phase::Preparing || result.generation != relog_.generation) return;
    const auto member = std::find_if(relog_.members.begin(), relog_.members.end(), [&](const auto& m) { return sameName(m.name, result.name); });
    if (member == relog_.members.end()) return;
    if (result.delivery == Delivery::RelogFailed) {
        member->failed = true;
        if (sameName(member->name, relog_.target)) {
            if (relog_.combined) extraRelog_ = {};
            relog_ = {};
            clientchat::printLocal("[Tsukuyomi] " + result.text + " (switching was cancelled)");
            return;
        }
        clientchat::printLocal("[Tsukuyomi] " + result.text + " (" + member->name + " will not rejoin)");
    } else { member->seat = std::move(result.seat); member->ready = true; }
    if (std::any_of(relog_.members.begin(), relog_.members.end(), [](const auto& m) { return !m.ready && !m.failed; })) return;
    if (std::string error; !enabled || !net::connected() || !rejoinRoute(error, game_, viewThread_)) {
        if (relog_.combined) extraRelog_ = {};
        relog_ = {};
        clientchat::printLocal("[Tsukuyomi] Bot: switching was cancelled");
        return;
    }
    if (relog_.combined) { startCombined(); return; }
    startRelog();
}
void BotManager::startRelog()
{
    std::string routeError;
    auto route = rejoinRoute(routeError, game_, viewThread_);
    if (route && route->friendWorld && route->friendId.empty()) { route.reset(); routeError = "switching is busy (try again in a moment)"; }
    if (!route) { relog_ = {}; clientchat::printLocal("[Tsukuyomi] Bot: switching was cancelled (" + routeError + ")"); return; }
    std::erase_if(relog_.members, [&](const auto& m) {
        return !(possessing_ && sameName(m.name, possessed_)) && !findBot(m.name); });
    std::optional<std::pair<possess::Uuid, possess::Uuid>> uuids;
    if (relog_.toBot()) {
        uuids = relogUuids(relog_.target);
        if (!uuids) {
            log().warn(L"Bot: player UUIDs are unknown; not rejoining as {}", std::wstring(relog_.target.begin(), relog_.target.end()));
            relog_ = {};
            clientchat::printLocal("[Tsukuyomi] Bot: switching was cancelled (the players' UUIDs are unknown; try again in a moment)");
            return;
        }
    }
    for (std::uint8_t sub = 1; sub < net::kSubCount; ++sub) if (occupied(slots_[sub])) leaveSlot(sub, true);
    slots_[0] = {};
    possessing_ = false; possessed_.clear();
    net::armRelog(relog_.toBot(), uuids, relog_.viewSub);
    net::setPumpWanted(relog_.toBot());
    scheduleRejoin(std::move(route->packet), route->netherNetId);
    relog_.phase = Relog::Phase::Rejoining; relog_.since = steadyMs(); relog_.serial = GameData::instance().playerSerial();
    log().info(L"Bot: rejoining {} as {} ({} bot(s) to rejoin)", std::wstring(route->where.begin(), route->where.end()),
        relog_.toBot() ? std::wstring(relog_.target.begin(), relog_.target.end()) : std::wstring(L"the main account"), relog_.members.size());
    clientchat::printLocal("[Tsukuyomi] Bot: rejoining as " + (relog_.toBot() ? relog_.target : std::string("your account")));
}
std::optional<std::pair<possess::Uuid, possess::Uuid>> BotManager::relogUuids(const std::string& target) const
{
    std::optional<std::uint64_t> mainRuntime;
    std::optional<std::int64_t> mainUnique;
    if (possessing_ && slots_[0].session.runtimeId()) { mainRuntime = slots_[0].session.runtimeId(); mainUnique = slots_[0].session.uniqueId(); }
    else if (const auto start = net::mainStartGame()) { mainRuntime = start->runtimeId; mainUnique = start->uniqueId; }
    else if (!possessing_) if (const auto local = readLocalPlayerIds()) { mainRuntime = local->runtimeId; mainUnique = local->uniqueId; }
    for (std::uint8_t sub = 1; sub < net::kSubCount; ++sub) {
        const auto& slot = slots_[sub];
        if (!occupied(slot) || !sameName(slot.name, target)) continue;
        auto mainUuid = mainRuntime ? net::playerUuid(sub, *mainRuntime) : std::nullopt;
        if (!mainUuid && mainUnique) mainUuid = net::listedUuid(*mainUnique);
        auto botUuid = slot.session.runtimeId() ? net::playerUuid(0, *slot.session.runtimeId()) : std::nullopt;
        if (!botUuid && slot.session.runtimeId()) botUuid = net::listedUuid(slot.session.uniqueId());
        if (!mainUuid || !botUuid) return std::nullopt;
        return std::pair{*mainUuid, *botUuid};
    }
    return std::nullopt;
}
void BotManager::rejoinMembers()
{
    std::string joined;
    const auto now = steadyMs();
    for (auto& member : relog_.members) {
        if (!member.ready || sameName(member.name, relog_.target)) continue;
        if (sendHome(member.name, member.sub, now)) { joined += (joined.empty() ? "" : ", ") + member.name; continue; }
        const auto sub = member.sub;
        if (sub == 0 || sub >= net::kSubCount || occupied(slots_[sub]) || (possessing_ && sub == possessedSub_)) {
            pendingChat_.push_back("[Tsukuyomi] Bot: seat " + std::to_string(sub) + " is in use, so " + member.name + " did not join again");
            continue;
        }
        std::string error;
        if (auto fresh = guestLogin(member.name, nullptr, error)) member.seat = std::move(*fresh);
        else { pendingChat_.push_back("[Tsukuyomi] Bot: " + member.name + " cannot join again (" + error + ")"); continue; }
        auto& slot = slots_[sub];
        slot = {};
        slot.name = member.name; slot.generation = ++generation_;
        slot.session.setClassic(classicFor(slot.name));
        net::activate(sub); slot.session.begin(sub, member.seat);
        joined += (joined.empty() ? "" : ", ") + member.name;
    }
    if (!joined.empty()) pendingChat_.push_back("[Tsukuyomi] Bot: " + joined + " joining again");
}
void BotManager::relogTick(std::uint64_t nowMs)
{
    if (relog_.phase != Relog::Phase::Rejoining) return;
    if (relog_.toBot()) {
        if (nowMs - relog_.since < kRelogTimeoutMs) return;
        net::armRelog(false); net::setPumpWanted(false); relog_ = {};
        clientchat::printLocal("[Tsukuyomi] Bot: rejoining timed out");
        return;
    }
    if (GameData::instance().playerSerial() == relog_.serial || !net::connected()) {
        if (nowMs - relog_.since >= kRelogTimeoutMs) { relog_ = {}; clientchat::printLocal("[Tsukuyomi] Bot: rejoining timed out"); }
        return;
    }
    if (!relog_.joinAt) { relog_.joinAt = nowMs + kRelogRejoinBotDelayMs; return; }
    if (nowMs < relog_.joinAt) return;
    markMainPlayer();
    dropped_ = 0; receivedIds_ = {}; sentIds_ = {}; loggedReceived_ = 0; loggedSent_ = 0;
    clientchat::printLocal("[Tsukuyomi] Bot: you are back on your account");
    rejoinMembers();
    relog_ = {};
}
std::optional<std::pair<std::uint64_t, std::int64_t>> BotManager::mainIds()
{
    if (possessing_) {
        const auto& session = slots_[0].session;
        if (online(slots_[0]) && session.runtimeId()) return std::pair{*session.runtimeId(), session.uniqueId()};
        return std::nullopt;
    }
    if (const auto start = net::mainStartGame()) return std::pair{start->runtimeId, start->uniqueId};
    if (const auto local = readLocalPlayerIds()) return std::pair{local->runtimeId, local->uniqueId};
    return std::nullopt;
}
BotManager::Extra* BotManager::userExtra()
{
    for (auto& extra : extras_) if (extra->user) return extra.get();
    return nullptr;
}
void BotManager::endGameAccount()
{
    gameAccount_.clear(); gameAccountHost_ = 0; extraRelog_ = {};
    net::disarmMainSwap(); gameidentity::disarmTokenSwap();
}
std::optional<chatcommand::Reply> BotManager::extraRelogBlocked(std::string& key)
{
    if (relog_.phase != Relog::Phase::None || extraRelog_.phase != ExtraRelog::Phase::None)
        return chatcommand::Reply{{"Bot: switching is in progress"}, true};
    if (GameData::instance().integratedServer()) return chatcommand::Reply{{"Bot: bots on their own connection are unavailable in a local world"}, true};
    if (!net::sendInternalTarget()) return chatcommand::Reply{{"Bot: switching accounts is unavailable (no send hook)"}, true};
    void* const peer = net::mainPeer();
    if (!peer || !net::connected()) return chatcommand::Reply{{"Bot: join a world with the main player first"}, true};
    const bool nether = nethernet::available() && nethernet::isNetherNet(peer);
    if (!nether && !(raknet::available() && raknet::isRakNet(peer)))
        return chatcommand::Reply{{"Bot: your connection is neither NetherNet nor RakNet; switching to a bot on its own connection is unavailable here"}, true};
    if (nether ? !gameidentity::ctorTarget() : !gamekey::secretTarget())
        return chatcommand::Reply{{"Bot: switching to a bot on its own connection is unavailable in this game version"}, true};
    key = net::gameLoginKey();
    if (std::string error; !rejoinRoute(error, game_, viewThread_) || key.empty() || net::userRequest().empty() || !(nether ? gameidentity::held() : gamekey::held()))
        return chatcommand::Reply{{"Bot: the game's login is unknown (inject Tsukuyomi before joining the server)"}, true};
    return std::nullopt;
}
void BotManager::rememberUserUuid(const Extra* to)
{
    if (!gameAccount_.empty()) return;
    if (const auto main = mainIds()) {
        auto uuid = to ? to->keeper.listedUuid(main->second) : std::nullopt;
        if (!uuid) uuid = net::listedUuid(main->second);
        if (uuid) userUuid_ = uuid;
    }
}
chatcommand::Reply BotManager::extraRelogSwitch(const std::string& targetName)
{
    std::string key;
    if (auto blocked = extraRelogBlocked(key)) return std::move(*blocked);
    ExtraRelog next;
    next.generation = ++extraRelogGeneration_; next.since = steadyMs(); next.key = key;
    if (!gameAccount_.empty()) next.rejoin.push_back(gameAccount_);
    if (targetName.empty()) {
        if (gameAccount_.empty()) return {{"Bot: you are already on your account"}, true};
        extraRelog_ = std::move(next);
        extraRelog_.phase = ExtraRelog::Phase::Preparing;
        startExtraRelog();
        return {};
    }
    Extra* const to = findExtra(targetName);
    if (!to) return {{"Bot: " + targetName + " is not on its own connection"}, true};
    if (to->slots[0].session.state() != State::Spawned) return {{"Bot: " + to->name() + " is not spawned"}, true};
    rememberUserUuid(to);
    auto targetUuid = to->keeper.listedUuid(to->slots[0].session.uniqueId());
    if (!targetUuid) targetUuid = net::listedUuid(to->slots[0].session.uniqueId());
    if (!userUuid_ || !targetUuid)
        return {{std::string("Bot: player UUIDs are unknown (your account: ") + (userUuid_ ? "ok" : "?") + ", " + to->name() + ": "
                 + (targetUuid ? "ok" : "?") + "); try again in a moment"}, true};
    next.target = to->name(); next.targetUuid = *targetUuid; next.host = to->hostNetherNetId;
    next.phase = ExtraRelog::Phase::Preparing;
    extraRelog_ = std::move(next);
    enqueue({Work::Foreign, extraRelog_.target, extraRelog_.generation, {}, key, {}, extraRelog_.host});
    return {{"Bot: preparing to rejoin as " + extraRelog_.target}, false};
}
void BotManager::onForeignPrepared(Result result, bool enabled)
{
    if (extraRelog_.phase != ExtraRelog::Phase::Preparing || result.generation != extraRelog_.generation) return;
    if (result.delivery == Delivery::ForeignFailed || !enabled) {
        extraRelog_ = {};
        if (relog_.combined) relog_ = {};
        if (result.delivery == Delivery::ForeignFailed) clientchat::printLocal("[Tsukuyomi] " + result.text + " (switching was cancelled)");
        return;
    }
    if (net::gameLoginKey() != extraRelog_.key) {
        extraRelog_ = {};
        if (relog_.combined) relog_ = {};
        clientchat::printLocal("[Tsukuyomi] Bot: switching was cancelled (the game's login changed)");
        return;
    }
    extraRelog_.token = std::move(result.token);
    extraRelog_.nonce = std::move(result.nonce);
    if (relog_.combined) { startCombined(); return; }
    startExtraRelog();
}
void BotManager::startExtraRelog()
{
    const auto cancel = [this](const std::string& why) {
        extraRelog_ = {};
        clientchat::printLocal("[Tsukuyomi] Bot: switching was cancelled (" + why + ")");
    };
    std::string routeError;
    auto route = rejoinRoute(routeError, game_, viewThread_);
    if (route && route->friendWorld && route->friendId.empty()) { route.reset(); routeError = "switching is busy (try again in a moment)"; }
    if (!net::connected()) { cancel("not connected"); return; }
    if (!route) { cancel(routeError); return; }
    if (extraRelog_.toBot() && !findExtra(extraRelog_.target)) { cancel(extraRelog_.target + " left"); return; }
    leaveRiders();
    const auto leavingUnique = [](const Extra* extra) { return extra && extra->slots[0].session.state() == State::Spawned ? extra->slots[0].session.uniqueId() : 0; };
    const auto lingeringNow = [this](const Extra* extra) {
        return std::any_of(lingering_.begin(), lingering_.end(), [&](const Lingering& l) { return l.extra.get() == extra; }) ? extra : nullptr;
    };
    if (extraRelog_.toBot()) {
        const auto it = std::find_if(extras_.begin(), extras_.end(), [&](const auto& e) { return !e->user && sameName(e->name(), extraRelog_.target); });
        const Extra* const closing = it->get();
        extraRelog_.leavingUnique = leavingUnique(closing);
        leaveExtra(static_cast<std::size_t>(it - extras_.begin()), true, !extraRelog_.nonce.empty());
        extraRelog_.leavingConnection = lingeringNow(closing);
        net::armMainSwap(extraRelog_.token, extraRelog_.key, *userUuid_, extraRelog_.targetUuid, extraRelog_.nonce);
        gameidentity::armTokenSwap(extraRelog_.token);
    } else {
        net::disarmMainSwap(); gameidentity::disarmTokenSwap();
        const auto it = std::find_if(extras_.begin(), extras_.end(), [](const auto& e) { return e->user; });
        if (it != extras_.end()) {
            const Extra* const closing = it->get();
            extraRelog_.leavingUnique = leavingUnique(closing);
            leaveExtra(static_cast<std::size_t>(it - extras_.begin()), true);
            extraRelog_.leavingConnection = lingeringNow(closing);
        }
    }
    net::setPumpWanted(true);
    extraRelog_.transfer = std::move(route->packet); extraRelog_.friendWorld = route->netherNetId; extraRelog_.where = route->where;
    if ((extraRelog_.leavingUnique && net::listedUuid(extraRelog_.leavingUnique)) || extraRelog_.leavingConnection) {
        extraRelog_.phase = ExtraRelog::Phase::Leaving; extraRelog_.since = steadyMs();
        return;
    }
    rejoinAfterLeaving(steadyMs(), false);
}
void BotManager::rejoinAfterLeaving(std::uint64_t nowMs, bool timedOut)
{
    const auto waited = extraRelog_.phase == ExtraRelog::Phase::Leaving ? nowMs - extraRelog_.since : 0;
    scheduleRejoin(std::exchange(extraRelog_.transfer, {}), extraRelog_.friendWorld);
    extraRelog_.phase = ExtraRelog::Phase::Rejoining; extraRelog_.since = nowMs; extraRelog_.serial = GameData::instance().playerSerial();
    const auto who = relog_.combined ? relog_.target + " (in a seat of " + extraRelog_.target + ")"
        : extraRelog_.toBot() ? extraRelog_.target : std::string(kUserLabel);
    log().info(L"Bot: rejoining {} as {} (waited {} ms for the closed connection to leave{})", std::wstring(extraRelog_.where.begin(), extraRelog_.where.end()),
        std::wstring(who.begin(), who.end()), waited, timedOut ? L"; timed out" : L"");
    clientchat::printLocal("[Tsukuyomi] Bot: rejoining as " + who);
}
chatcommand::Reply BotManager::combinedSwitch(const std::string& name, Extra& host, std::uint8_t sub)
{
    std::string key;
    if (auto blocked = extraRelogBlocked(key)) return std::move(*blocked);
    if (!gameAccount_.empty() || host.user || sub == 0 || sub >= net::kSubCount) return {{"Bot: switch back to your account first (/tk bot switch)"}, true};
    const auto& slot = host.slots[sub];
    if (!occupied(slot) || !sameName(slot.name, name)) return {{"Bot: " + name + " is not on " + host.name() + "'s connection"}, true};
    if (slot.session.state() != State::Spawned) return {{"Bot: " + slot.name + " is not spawned"}, true};
    if (host.slots[0].session.state() != State::Spawned) return {{"Bot: " + host.name() + " is not spawned"}, true};
    rememberUserUuid(&host);
    const auto listed = [&](std::int64_t unique) {
        auto uuid = host.keeper.listedUuid(unique);
        return uuid ? uuid : net::listedUuid(unique);
    };
    const auto hostUuid = listed(host.slots[0].session.uniqueId());
    const auto botUuid = listed(slot.session.uniqueId());
    if (!userUuid_ || !hostUuid || !botUuid)
        return {{std::string("Bot: player UUIDs are unknown (your account: ") + (userUuid_ ? "ok" : "?") + ", " + host.name() + ": "
                 + (hostUuid ? "ok" : "?") + ", " + slot.name + ": " + (botUuid ? "ok" : "?") + "); try again in a moment"}, true};
    ExtraRelog next;
    next.generation = ++extraRelogGeneration_; next.since = steadyMs(); next.key = key;
    next.target = host.name(); next.targetUuid = *hostUuid; next.host = host.hostNetherNetId;
    next.phase = ExtraRelog::Phase::Preparing;
    extraRelog_ = std::move(next);
    relog_ = {};
    relog_.phase = Relog::Phase::Preparing; relog_.target = slot.name; relog_.generation = ++relogGeneration_; relog_.since = steadyMs();
    relog_.combined = true; relog_.host = host.name(); relog_.viewSub = sub; relog_.uuids = std::pair{*userUuid_, *botUuid};
    relog_.members.push_back({slot.name});
    prepareSeat(true, slot.name, relog_.generation, nullptr);
    enqueue({Work::Foreign, extraRelog_.target, extraRelog_.generation, {}, key, {}, extraRelog_.host});
    log().info(L"Bot: preparing to rejoin as {} (seat {} of {})", std::wstring(slot.name.begin(), slot.name.end()), sub,
        std::wstring(extraRelog_.target.begin(), extraRelog_.target.end()));
    return {{"Bot: preparing to rejoin as " + slot.name + " (in a seat of " + host.name() + ")"}, false};
}
void BotManager::startCombined()
{
    if (!relog_.combined || relog_.phase != Relog::Phase::Preparing || extraRelog_.phase != ExtraRelog::Phase::Preparing) return;
    if (extraRelog_.token.empty() || std::any_of(relog_.members.begin(), relog_.members.end(), [](const auto& m) { return !m.ready; })) return;
    const auto cancel = [this](const std::string& why) {
        relog_ = {}; extraRelog_ = {};
        clientchat::printLocal("[Tsukuyomi] Bot: switching was cancelled (" + why + ")");
    };
    if (std::string error; !net::connected() || !rejoinRoute(error, game_, viewThread_)) { cancel(net::connected() ? error : std::string("not connected")); return; }
    if (net::gameLoginKey() != extraRelog_.key) { cancel("the game's login changed"); return; }
    Extra* const host = findExtra(relog_.host);
    if (!host) { cancel(relog_.host + " left"); return; }
    const auto sub = relog_.viewSub;
    if (!occupied(host->slots[sub]) || !sameName(host->slots[sub].name, relog_.target)) { cancel(relog_.target + " is no longer on " + relog_.host); return; }
    leaveExtraSub(*host, sub, true);
    setHome(relog_.target, relog_.host);
    startExtraRelog();
    if (extraRelog_.phase != ExtraRelog::Phase::Rejoining && extraRelog_.phase != ExtraRelog::Phase::Leaving) { relog_ = {}; return; }
    net::armRelog(true, relog_.uuids, sub);
    relog_.phase = Relog::Phase::Rejoining; relog_.since = steadyMs(); relog_.serial = GameData::instance().playerSerial();
}
bool BotManager::startUserExtra(std::string& error)
{
    auto request = net::userRequest();
    void* const peer = net::mainPeer();
    if (request.empty()) { error = "the game's login is unknown"; return false; }
    if (!peer) { error = "not connected"; return false; }
    auto extra = std::make_unique<Extra>();
    extra->user = true;
    if (nethernet::available() && nethernet::isNetherNet(peer)) {
        if (!gameidentity::held()) { error = "the game's identity is unknown"; return false; }
        extra->target.kind = ExtraConnection::Kind::NetherNet;
        extra->target.mainPeer = peer;
    } else if (raknet::available() && raknet::isRakNet(peer)) {
        if (!gamekey::held()) { error = "the game's login key is unknown"; return false; }
        const auto remote = raknet::remoteAddress(peer);
        if (!remote) { error = "cannot read the server's address"; return false; }
        extra->target.kind = ExtraConnection::Kind::RakNet;
        extra->target.address = *remote;
    } else { error = "the connection is neither NetherNet nor RakNet"; return false; }
    auto& primary = extra->slots[0];
    primary.name = kUserLabel; primary.generation = ++generation_; primary.session.prepare();
    bool started = false;
    try {
        started = extra->connection.start(extra->target, gameconn::protocolVersion(), std::move(request), {}, nullptr, steadyMs(), error);
    } catch (const std::exception& e) { error = std::string("exception: ") + e.what(); }
    if (!started) return false;
    primary.session.beginPrimary();
    const bool rak = extra->target.kind == ExtraConnection::Kind::RakNet;
    extras_.push_back(std::move(extra));
    log().info(L"Bot: your account is joining on its own connection ({}; the game's login and {})",
        rak ? L"RakNet" : L"NetherNet", rak ? L"key exchange" : L"identity");
    return true;
}
void BotManager::extraRelogTick(std::uint64_t nowMs)
{
    if (extraRelog_.phase == ExtraRelog::Phase::Leaving) {
        if (extraRelog_.leavingConnection && std::none_of(lingering_.begin(), lingering_.end(), [&](const Lingering& l) { return l.extra.get() == extraRelog_.leavingConnection; }))
            extraRelog_.leavingConnection = nullptr;
        const bool gone = !extraRelog_.leavingConnection && !(extraRelog_.leavingUnique && net::listedUuid(extraRelog_.leavingUnique));
        if (gone || nowMs - extraRelog_.since >= kLeavingWaitMs) rejoinAfterLeaving(nowMs, !gone);
        return;
    }
    if (extraRelog_.phase != ExtraRelog::Phase::Rejoining) return;
    if (GameData::instance().playerSerial() == extraRelog_.serial || !net::connected()) {
        if (nowMs - extraRelog_.since >= kRelogTimeoutMs) {
            endGameAccount(); net::setPumpWanted(false);
            clientchat::printLocal("[Tsukuyomi] Bot: rejoining timed out");
        }
        return;
    }
    markMainPlayer();
    net::setPumpWanted(possessing_ || relog_.phase != Relog::Phase::None);
    gameidentity::disarmTokenSwap();
    ridersNotBefore_ = nowMs + kRelogRejoinBotDelayMs;
    auto rejoin = std::move(extraRelog_.rejoin);
    if (extraRelog_.toBot()) {
        if (net::mainSwapped()) { gameAccount_ = extraRelog_.target; gameAccountHost_ = extraRelog_.host; }
        else {
            const auto error = net::takeMainSwapError();
            pendingChat_.push_back("[Tsukuyomi] Bot: could not rejoin as " + extraRelog_.target + " (" + (error.empty() ? std::string("unknown") : error)
                + "); you are on your account");
            gameAccount_.clear();
            rejoin.push_back(extraRelog_.target);
        }
    } else { gameAccount_.clear(); gameAccountHost_ = 0; }
    extraRelog_ = {};
    if (!gameAccount_.empty() && !userExtra()) {
        std::string error;
        if (!startUserExtra(error)) pendingChat_.push_back("[Tsukuyomi] Bot: your account cannot join on its own connection (" + error + ")");
    }
    if (!possessing_ && relog_.phase == Relog::Phase::None)
        pendingChat_.push_back(gameAccount_.empty() ? std::string("[Tsukuyomi] Bot: you are back on your account") : "[Tsukuyomi] Bot: you are now " + gameAccount_);
    for (const auto& name : rejoin) {
        if (isActive(name)) continue;
        for (const auto& line : joinExtra(name).lines) pendingChat_.push_back("[Tsukuyomi] " + line);
    }
}
void BotManager::leaveRiders()
{
    const auto now = steadyMs();
    std::string names;
    const auto keep = [&](const std::string& name, const std::string& host, std::uint8_t sub) {
        riders_.push_back({name, host, now, sub});
        names += (names.empty() ? "" : ", ") + name;
    };
    for (std::uint8_t sub = 1; sub < net::kSubCount; ++sub)
        if (occupied(slots_[sub])) { keep(slots_[sub].name, gameAccount_, sub); leaveSlot(sub, true); }
    if (possessing_) {
        keep(possessed_, takeHome(possessed_), possessedSub_);
        slots_[0] = {};
        possessing_ = false; possessed_.clear();
        net::armRelog(false);
    }
    Extra* const closing = extraRelog_.toBot() ? findExtra(extraRelog_.target) : userExtra();
    if (closing)
        for (std::uint8_t sub = 1; sub < net::kSubCount; ++sub)
            if (occupied(closing->slots[sub])) {
                keep(closing->slots[sub].name, closing->user ? std::string() : closing->name(), sub);
                leaveExtraSub(*closing, sub, true);
            }
    if (names.empty()) return;
    log().info(L"Bot: {} left for switching; rejoining them afterwards", std::wstring(names.begin(), names.end()));
    clientchat::printLocal("[Tsukuyomi] Bot: " + names + " left for switching (they join again after it)");
}
void BotManager::setHome(const std::string& name, const std::string& host)
{
    std::erase_if(homes_, [&](const Home& home) { return sameName(home.name, name); });
    if (!host.empty()) homes_.push_back({name, host});
}
std::string BotManager::takeHome(const std::string& name)
{
    const auto it = std::find_if(homes_.begin(), homes_.end(), [&](const Home& home) { return sameName(home.name, name); });
    if (it == homes_.end()) return {};
    auto host = std::move(it->host);
    homes_.erase(it);
    return host;
}
bool BotManager::sendHome(const std::string& name, std::uint8_t sub, std::uint64_t nowMs)
{
    const auto host = takeHome(name);
    if (host.empty()) return false;
    if (sameName(host, gameAccount_)) return false;
    if (!findExtra(host)) {
        pendingChat_.push_back("[Tsukuyomi] Bot: " + host + " is gone, so " + name + " joins your connection instead");
        return false;
    }
    riders_.push_back({name, host, nowMs, sub});
    log().info(L"Bot: {} goes back to {} (it was moved from there to be controlled)", std::wstring(name.begin(), name.end()),
        std::wstring(host.begin(), host.end()));
    return true;
}
void BotManager::riderTick(std::uint64_t nowMs)
{
    if (riders_.empty() || extraRelog_.phase != ExtraRelog::Phase::None || relog_.phase != Relog::Phase::None)
        return;
    for (auto it = riders_.begin(); it != riders_.end();) {
        const Rider rider = *it;
        const std::string hostLabel = rider.host.empty() ? std::string(kUserLabel) : rider.host;
        if (isActive(rider.name)) { it = riders_.erase(it); continue; }
        const bool hostIsUser = rider.host.empty();
        const bool onGame = hostIsUser ? gameAccount_.empty() : sameName(rider.host, gameAccount_);
        Extra* const host = onGame ? nullptr : hostIsUser ? userExtra() : findExtra(rider.host);
        if (!onGame && !host) {
            pendingChat_.push_back("[Tsukuyomi] Bot: " + rider.name + " did not join again (" + hostLabel + " is gone)");
            it = riders_.erase(it); continue;
        }
        if (nowMs - rider.since > kRiderTimeoutMs) {
            pendingChat_.push_back("[Tsukuyomi] Bot: " + rider.name + " did not join again (" + hostLabel + " did not spawn in time)");
            it = riders_.erase(it); continue;
        }
        chatcommand::Reply reply;
        if (onGame) {
            if (nowMs < ridersNotBefore_ || !net::connected()) { ++it; continue; }
            reply = joinSub(rider.name, rider.sub);
        } else {
            if (host->slots[0].session.state() != State::Spawned) { ++it; continue; }
            reply = joinExtraSub(rider.name, *host, rider.sub);
        }
        log().info(L"Bot: joining {} again in a seat of {}", std::wstring(rider.name.begin(), rider.name.end()),
            std::wstring(hostLabel.begin(), hostLabel.end()));
        for (const auto& line : reply.lines) pendingChat_.push_back("[Tsukuyomi] " + line);
        it = riders_.erase(it);
    }
}
chatcommand::Reply BotManager::moveThenPossess(const std::string& name)
{
    if (const auto rider = std::find_if(riders_.begin(), riders_.end(), [&](const Rider& r) { return sameName(r.name, name); }); rider != riders_.end()) {
        pendingPossess_ = {rider->name, steadyMs()};
        return {{"Bot: switching to " + rider->name + " after it joins " + (rider->host.empty() ? std::string("your connection") : rider->host + "'s connection")
                 + " again"}, false};
    }
    for (auto& extra : extras_) {
        if (extra->user) continue;
        for (std::uint8_t sub = 1; sub < net::kSubCount; ++sub)
            if (occupied(extra->slots[sub]) && sameName(extra->slots[sub].name, name)) return combinedSwitch(extra->slots[sub].name, *extra, sub);
    }
    return {{"Bot: switching to " + name + " is not supported here"}, true};
}
void BotManager::possessTick(std::uint64_t nowMs)
{
    if (pendingPossess_.name.empty()) return;
    if (extraRelog_.phase != ExtraRelog::Phase::None || relog_.phase != Relog::Phase::None) return;
    const auto name = pendingPossess_.name;
    const auto giveUp = [&](const std::string& why) {
        pendingPossess_ = {};
        pendingChat_.push_back("[Tsukuyomi] Bot: not switching to " + name + " (" + why + ")");
    };
    if (nowMs - pendingPossess_.since > kPendingPossessTimeoutMs) { giveUp("it did not get ready in time"); return; }
    if (!gameAccount_.empty()) { giveUp("you are still " + gameAccount_); return; }
    if (possessing_ && sameName(possessed_, name)) { pendingPossess_ = {}; return; }
    if (const Slot* const slot = findBot(name)) {
        if (slot->session.state() != State::Spawned) return;
        if (!relogUuids(name)) {
            if (!pendingPossess_.uuidSince) {
                pendingPossess_.uuidSince = nowMs;
                log().info(L"Bot: {} spawned on your connection; waiting for the players' UUIDs before switching", std::wstring(name.begin(), name.end()));
            } else if (nowMs - pendingPossess_.uuidSince > kPendingPossessUuidTimeoutMs) giveUp("it has not seen your player");
            return;
        }
        pendingPossess_ = {};
        log().info(L"Bot: {} is ready on your connection; switching to it", std::wstring(name.begin(), name.end()));
        for (const auto& line : relogSwitch(name).lines) pendingChat_.push_back("[Tsukuyomi] " + line);
        return;
    }
    if (std::any_of(riders_.begin(), riders_.end(), [&](const Rider& r) { return sameName(r.name, name); })) return;
    const bool onExtra = std::any_of(extras_.begin(), extras_.end(), [&](const auto& extra) {
        return std::any_of(extra->slots.begin() + 1, extra->slots.end(), [&](const Slot& slot) { return occupied(slot) && sameName(slot.name, name); }); });
    if (onExtra) {
        pendingPossess_ = {};
        const auto reply = moveThenPossess(name);
        for (const auto& line : reply.lines) pendingChat_.push_back("[Tsukuyomi] " + line);
        return;
    }
    giveUp(name + " is not active");
}
std::string BotManager::currentAccount() const
{
    if (possessing_) return possessed_;
    return gameAccount_.empty() ? std::string(kUserLabel) : gameAccount_;
}
bool BotManager::loadSkinSettings(std::string& message)
{
    std::error_code error;
    std::filesystem::create_directories(paths::dataDir() / "skins", error);
    const auto path = skinsPath();
    const bool existed = std::filesystem::exists(path, error);
    std::string reason;
    auto loaded = skin::loadSettings(path, reason);
    if (!loaded) { message = "Bot: cannot read BotSkins.json (" + reason + "); the skins stay as they were"; return false; }
    if (!existed && !error) skin::saveSettings(path, {});
    skinSettings_ = std::move(*loaded);
    const auto show = [](const std::string& spec) { return spec.empty() ? std::string("-") : spec; };
    message = "Bot: skins reloaded (current " + show(skinSettings_.current) + ", main " + show(skinSettings_.main) + ", sub "
        + show(skinSettings_.sub) + ", " + std::to_string(skinSettings_.names.size()) + " by name)";
    return true;
}
std::string BotManager::resolveSkin(const std::string& key, std::uint8_t sub) const
{
    const auto& names = skinSettings_.names;
    if (const auto it = names.find(key); it != names.end()) return it->second;
    std::string host = key, suffix;
    if (isGuest(key)) { host = key.substr(0, key.size() - 4); suffix = key.substr(key.size() - 4); }
    if (host == lowerKey(kUserLabel) && !userGamertag_.empty()) host = userGamertag_;
    for (const auto& alias : {host + suffix, suffix.empty() ? std::string() : host.substr(0, 16 - suffix.size()) + suffix})
        if (const auto it = names.find(alias); !alias.empty() && it != names.end()) return it->second;
    if (!skinSettings_.current.empty() && key == lowerKey(currentAccount())) return skinSettings_.current;
    return sub == 0 ? skinSettings_.main : skinSettings_.sub;
}
void BotManager::requestSkin(const std::string& spec)
{
    skinBuilds_[spec] = {};
    enqueue({Work::Skin, spec, skinGeneration_});
}
chatcommand::Reply BotManager::skin(const std::vector<std::string>& args)
{
    if (!running_.load(std::memory_order_acquire)) return {{"Bot is unavailable"}, true};
    std::lock_guard lock(sessionMutex_);
    return skinCommand(args);
}
chatcommand::Reply BotManager::skinCommand(const std::vector<std::string>& args)
{
    if (!args.empty() && args[0] == "steal") return stealSkin(args);
    if (args.size() != 1 || args[0] != "reload")
        return {{"/tk skin reload (set the skins in Tsukuyomi/BotSkins.json: current / main / sub / names, with the names in Tsukuyomi/skins/skins.json)"
                 " | skin steal <gamertag> [name]"}, true};
    std::string message;
    if (!loadSkinSettings(message)) return {{message}, true};
    ++skinGeneration_;
    skinBuilds_.clear();
    skinReported_.clear();
    std::set<std::string> specs{skinSettings_.current, skinSettings_.main, skinSettings_.sub};
    for (const auto& [name, spec] : skinSettings_.names) specs.insert(spec);
    for (const auto& spec : specs) if (!spec.empty()) requestSkin(spec);
    return {{message}, false};
}
chatcommand::Reply BotManager::stealSkin(const std::vector<std::string>& args)
{
    constexpr const char* usage = "/tk skin steal <gamertag> [name] (saves the player's skin to Tsukuyomi/skins/<name>; "
                                  "quote names with spaces like /tp: \"Some Player\"; without a name the player's name is used)";
    if (args.size() != 2 && args.size() != 3) return {{usage}, true};
    const auto unquote = [](const std::string& value) -> std::optional<std::string> {
        if (!value.starts_with('"')) return value;
        const auto parsed = nlohmann::json::parse(value, nullptr, false);
        if (!parsed.is_string()) return std::nullopt;
        return parsed.get<std::string>();
    };
    const bool named = args.size() == 3;
    const auto name = named ? unquote(args[2]) : std::optional<std::string>(std::string());
    auto gamertag = unquote(args[1]);
    if (!name || !gamertag || gamertag->empty()) return {{usage}, true};
    if (gamertag->starts_with('@')) {
        if (*gamertag != "@s") return {{"Bot: give a player name, not a selector (only @s is accepted, for your account)"}, true};
        *gamertag = readUserGamertag();
        if (gamertag->empty()) return {{"Bot: your gamertag is unknown (inject before joining the server)"}, true};
    }
    if (named && !skin::validSkinName(*name))
        return {{"Bot: invalid skin name (up to 64 characters, without \\ / : * ? \" < > |)"}, true};
    const auto player = net::listedSkin(*gamertag);
    if (!player || !player->skin) {
        std::string seen;
        const auto names = net::listedSkinNames();
        for (std::size_t i = 0; i < names.size() && i < 12; ++i) seen += (i ? ", " : "") + names[i];
        if (names.size() > 12) seen += ", ...";
        return {{"Bot: no skin of " + *gamertag + " has been received (" + (seen.empty() ? std::string("no players yet") : "players: " + seen)
                 + "). Skins arrive when a player joins or changes skin, so inject before joining the server"}, true};
    }
    const auto saveName = named ? *name : player->name;
    if (!skin::validSkinName(saveName))
        return {{"Bot: " + player->name + " cannot be used as a skin name; give one: /tk skin steal \"" + player->name + "\" <name>"}, true};
    enqueue({Work::SkinSave, saveName, 0, player->name, {}, player->skin});
    return {{"Bot: saving " + player->name + "'s skin as " + saveName + (player->present ? std::string() : " (the player has left)")}, false};
}
void BotManager::saveStolenSkin(const Job& job)
{
    skin::SaveResult saved;
    try { saved = skin::saveSkinPack(skinRoots().front(), job.name, *job.skin, job.address); }
    catch (const std::exception&) { saved.error = "cannot write the files"; }
    if (!saved.error.empty()) { post({Delivery::Notice, "Bot: cannot save the skin " + job.name + " (" + saved.error + ")", {}, 0}); return; }
    std::string detail = saved.geometryName + (saved.geometryFile ? ", own geometry" : "") + (saved.cape ? ", cape" : "");
    if (saved.fullSkin) detail += ", persona (kept as received in skin.bin)";
    else if (!job.skin->animations.empty()) detail += ", " + std::to_string(job.skin->animations.size()) + " animation image(s) not saved";
    log().info(L"Bot: saved {}'s skin to {} ({}x{}, {}{})", std::wstring(job.address.begin(), job.address.end()), saved.dir.wstring(),
               job.skin->image.width, job.skin->image.height, std::wstring(detail.begin(), detail.end()), saved.replaced ? L", replaced" : L"");
    const auto loaded = skin::load(job.name, skinRoots(), skinGeometryRoots());
    if (!loaded.skin) {
        post({Delivery::Notice, "Bot: saved " + job.address + "'s skin to Tsukuyomi/skins/" + job.name + ", but it cannot be loaded (" + loaded.error + ")", {}, 0});
        return;
    }
    post({Delivery::Notice, "Bot: " + std::string(saved.replaced ? "replaced " : "saved ") + job.address + "'s skin as " + job.name + " (" + detail
          + "). Put \"" + job.name + "\" in Tsukuyomi/BotSkins.json and run /tk skin reload", {}, 0});
}
void BotManager::onSkinResult(Result result)
{
    if (result.generation != skinGeneration_) return;
    const auto& spec = result.name;
    auto& build = skinBuilds_[spec];
    const bool ok = result.delivery == Delivery::SkinReady && result.skin;
    build.ready = ok; build.failed = !ok;
    build.skin = ok ? result.skin : nullptr;
    if (ok) build.label = result.text; else build.error = result.text;
    if (ok) log().info(L"Bot: loaded the skin {}", std::wstring(build.label.begin(), build.label.end()));
    else clientchat::printLocal("[Tsukuyomi] Bot: cannot load the skin " + spec + " (" + build.error + ")");
}
void BotManager::applySkins(std::uint64_t nowMs)
{
    if (skinSettings_.empty() && std::none_of(skinSent_.begin(), skinSent_.end(), [](const auto& s) { return !s.second.spec.empty(); })) return;
    if (relog_.phase != Relog::Phase::None || extraRelog_.phase != ExtraRelog::Phase::None) return;
    struct Live {
        std::string key, stamp;
        std::uint8_t sub = 0;
        Extra* extra = nullptr;
        std::optional<possess::Uuid> uuid;
    };
    if (!skinSettings_.names.empty()) userGamertag();
    std::vector<Live> lives;
    const auto add = [&](const std::string& name, std::string stamp, std::uint8_t sub, Extra* extra, std::optional<possess::Uuid> uuid) {
        auto key = lowerKey(name);
        if (std::any_of(lives.begin(), lives.end(), [&](const Live& l) { return l.key == key; })) return;
        lives.push_back({std::move(key), std::move(stamp), sub, extra, uuid});
    };
    for (std::uint8_t sub = 0; sub < net::kSubCount; ++sub) {
        const auto& slot = slots_[sub];
        if (slot.session.state() != State::Spawned || !slot.session.spawnSerial()) continue;
        const std::string name = sub == 0 ? (gameAccount_.empty() ? std::string(kUserLabel) : gameAccount_) : slot.name;
        add(name, "m" + std::to_string(slot.session.spawnSerial()), sub, nullptr, net::listedUuid(slot.session.uniqueId()));
    }
    for (auto& extra : extras_) {
        for (std::uint8_t sub = 0; sub < net::kSubCount; ++sub) {
            const auto& slot = extra->slots[sub];
            if (slot.session.state() != State::Spawned || !slot.session.spawnSerial()) continue;
            const std::string name = sub == 0 && extra->user ? std::string(kUserLabel) : slot.name;
            auto uuid = extra->keeper.listedUuid(slot.session.uniqueId());
            if (!uuid) uuid = net::listedUuid(slot.session.uniqueId());
            add(name, "e" + std::to_string(slot.session.spawnSerial()), sub, extra.get(), uuid);
        }
    }
    if (net::connected()) {
        std::optional<possess::Uuid> uuid;
        if (possessing_) {
            if (const auto ids = net::currentIds(); ids.active && ids.uuids) uuid = ids.uuidB;
        } else if (const auto main = mainIds()) {
            uuid = net::listedUuid(main->second);
            if (!uuid) {
                static std::int64_t cachedId = 0;
                static std::optional<possess::Uuid> cached;
                if (cachedId != main->second || !cached) {
                    possess::Uuid id{};
                    cachedId = main->second;
                    cached = levelplayers::uuidOf(main->second, id.data()) ? std::optional<possess::Uuid>(id) : std::nullopt;
                }
                uuid = cached;
            }
        }
        const std::uint8_t sub = possessing_ ? possessedSub_ : 0;
        add(currentAccount(), "g" + std::to_string(net::loginCount()) + "." + std::to_string(GameData::instance().playerSerial()) + "." + std::to_string(sub),
            sub, nullptr, uuid);
    }
    for (const auto& live : lives) {
        const std::string spec = resolveSkin(live.key, live.sub);
        auto& sent = skinSent_[live.key];
        if (spec.empty() ? sent.spec.empty() : sent.stamp == live.stamp && sent.spec == spec && sent.generation == skinGeneration_) continue;
        std::shared_ptr<const skin::Skin> chosen;
        if (spec.empty()) {
            std::optional<skin::Skin> login;
            const std::string user = lowerKey(kUserLabel);
            if (live.key == user || live.key.starts_with(user + " (")) {
                auto request = net::userRequest();
                if (request.empty()) request = net::sentRequest();
                if (const auto parts = splitConnectionRequest(request))
                    if (const auto payload = jwt::readPayload(parts->second)) login = skin::fromClientData(*payload);
                if (!login) {
                    sent.spec.clear();
                    pendingChat_.push_back("[Tsukuyomi] Bot: your login skin is unknown; it comes back when you join again");
                    continue;
                }
            } else login = skin::defaultSkin();
            chosen = std::make_shared<const skin::Skin>(std::move(*login));
        } else {
            const auto build = skinBuilds_.find(spec);
            if (build == skinBuilds_.end()) { requestSkin(spec); continue; }
            if (!build->second.ready) continue;
            chosen = build->second.skin;
        }
        if (sent.seenStamp != live.stamp) { sent.seenStamp = live.stamp; sent.seenAt = nowMs; }
        if (nowMs - sent.seenAt < 1500) continue;
        if (!live.uuid) {
            if (nowMs - sent.seenAt >= 10000 && skinReported_.insert(live.key + "|" + live.stamp).second) {
                pendingChat_.push_back("[Tsukuyomi] Bot: cannot send " + live.key + "'s skin yet (its player id is unknown)");
                log().warn(L"Bot: cannot send {}'s skin yet (no PlayerList row for it)", std::wstring(live.key.begin(), live.key.end()));
            }
            continue;
        }
        const auto packet = skin::playerSkinPacket(live.sub, *live.uuid, *chosen);
        bool ok = false;
        if (live.extra) { ok = live.extra->connection.send({packet}); live.extra->connection.flush(); }
        else ok = net::sendRaw(packet);
        log().info(L"Bot: sent PlayerSkin for {} (sub {}, {} bytes, {}, {})", std::wstring(live.key.begin(), live.key.end()), live.sub,
                   packet.size(), spec.empty() ? std::wstring(L"login skin") : std::wstring(spec.begin(), spec.end()), ok ? L"ok" : L"failed");
        if (!ok) continue;
        sent.stamp = live.stamp; sent.spec = spec; sent.generation = skinGeneration_;
    }
}
}
