#pragma once
#include "BotNet.h"
#include "ExtraConnection.h"
#include "BotCommandSyntax.h"
#include "BotSession.h"
#include "InventoryView.h"
#include "SkinPack.h"
#include "XboxAuth.h"
#include "game/ChatCommand.h"
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace tsukuyomi::bot {
std::vector<std::string> savedAccountNames();
std::string readUserGamertag();
std::vector<std::string> completionNames();
void noteAccountCompletion();
class BotManager {
public:
    static BotManager& instance();
    void initialize();
    void shutdown();
    chatcommand::Reply command(const std::vector<std::string>& args);
    chatcommand::Reply skin(const std::vector<std::string>& args);
    void onPlayerViewUpdate(bool enabled);
    void onGameUpdate(void* game);
private:
    BotManager() = default;
    ~BotManager();
    enum class Work { Login, Accounts, Logout, JoinExtra, Foreign, Skin, SkinSave, LeaveSession };
    struct Job { Work work; std::string name; std::uint64_t generation = 0; std::string address; std::string key = {};
                 std::shared_ptr<const skin::Skin> skin = {}; std::uint64_t hostNetherNetId = 0; };
    enum class Delivery { Notice, SeatReady, Failed, RelogReady, RelogFailed, PreparedExtra, ForeignReady, ForeignFailed, SkinReady, SkinFailed };
    static Delivery failureFor(Work);
    struct Result { Delivery delivery; std::string text, name; std::uint64_t generation = 0;
                    std::string token; std::shared_ptr<crypto::EcKey> key; std::shared_ptr<const skin::Skin> skin = {}; std::string nonce = {};
                    guest::Login seat = {}; };
    void worker();
    void run(const Job&);
    struct Extra;
    std::optional<guest::Login> guestLogin(const std::string& name, const Extra* host, std::string& error);
    void prepareSeat(bool relog, const std::string& name, std::uint64_t generation, const Extra* host);
    void post(Result);
    void enqueue(Job);
    bool pauseUntil(std::chrono::steady_clock::time_point);
    std::mutex workMutex_;
    std::condition_variable wake_;
    std::deque<Job> jobs_;
    std::vector<Result> results_;
    std::thread thread_;
    bool stopping_ = false, loginBusy_ = false;
    std::map<std::string, AuthCache> authCache_;
    Cooldown httpCooldown_;
    std::chrono::steady_clock::time_point lastNetworkJob_{};
    struct Slot {
        std::string name;
        BotSession session;
        std::uint64_t generation = 0;
        std::uint64_t received = 0;
        std::uint64_t fedSerial = 0, fedAt = 0, askedAt = 0;
        std::uint64_t liquidCheckedAt = 0;
    };
    static bool occupied(const Slot& slot) { return slot.session.state() != State::Idle; }
    static bool online(const Slot& slot)
    {
        const auto state = slot.session.state();
        return state == State::LoggingIn || state == State::Spawning || state == State::Spawned;
    }
    Slot* findBot(const std::string& name);
    std::size_t botCount() const;
    void leaveSlot(std::uint8_t sub, bool send);
    void leaveAll(bool send);
    void releaseDisconnected();
    void markMainPlayer();
    bool mainPlayerReplaced();
    struct Extra {
        ExtraConnection connection;
        ExtraConnection::Target target;
        std::uint64_t hostNetherNetId = 0;
        std::array<Slot, net::kSubCount> slots;
        const std::string& name() const { return slots[0].name; }
        possess::StateKeeper keeper;
        std::optional<std::int32_t> dimension;
        bool user = false;
        std::string address;
        guest::Host guestHost;
    };
    std::vector<std::unique_ptr<Extra>> extras_;
    struct Lingering { std::unique_ptr<Extra> extra; std::uint64_t until = 0; };
    std::vector<Lingering> lingering_;
    void closeLingering(bool all);
    Extra* findExtra(const std::string& name);
    bool isActive(const std::string& name);
    chatcommand::Reply joinExtra(std::string name);
    chatcommand::Reply joinExtraSub(std::string name, Extra& host, std::uint8_t sub);
    chatcommand::Reply joinSub(std::string name, std::uint8_t sub);
    chatcommand::Reply joinSeat(const std::string& host, std::uint8_t sub);
    chatcommand::Reply joinAccountCommand(const syntax::Command& command, bool busy);
    chatcommand::Reply joinGuestCommand(const syntax::Command& command, bool busy);
    std::optional<chatcommand::Reply> joinBlocked(bool busy);
    chatcommand::Reply actionCommand(const syntax::Command& command);
    chatcommand::Reply statusCommand();
    chatcommand::Reply leaveAllCommand(bool busy);
    struct ResolvedTarget {
        std::string name;
        std::optional<chatcommand::Reply> error;
    };
    ResolvedTarget resolveTarget(const syntax::Target& target);
    const std::string& userGamertag();
    std::set<std::string> classic_;
    bool classicFor(const std::string& name) const;
    std::optional<std::string> seatOccupant(const std::string& host, std::uint8_t sub);
    void leaveExtra(std::size_t index, bool send, bool keepSession = false);
    void leaveExtras(bool send);
    void leaveExtraSub(Extra& extra, std::uint8_t sub, bool send);
    void pumpExtras();
    void servePaths();
    std::vector<std::string> terrainExtra_;
    std::uint64_t terrainUsedAt_ = 0;
    std::set<std::pair<std::int32_t, std::int32_t>> terrainHanded_;
    const void* terrainHandedOwner_ = nullptr;
    std::int32_t terrainHandedDimension_ = -1;
    std::uint64_t mainGoneSince_ = 0;
    void watchMainGone();
    std::uint32_t viewThread_ = 0;
    bool gameUpdateChecked_ = false, gameUpdateUsable_ = false;
    std::string gameAccount_;
    std::uint64_t gameAccountHost_ = 0;
    void* game_ = nullptr;
    struct FriendRejoin {
        enum class Phase { None, Leave, Joining } phase = Phase::None;
        std::uint64_t netherNetId = 0, since = 0;
    } friendRejoin_;
    void friendRejoinTick(void* game, std::uint64_t nowMs);
    void failFriendRejoin(const std::string& why);
    void scheduleRejoin(std::string packet, std::uint64_t friendWorld);
    void rejoinTimeouts(std::uint64_t nowMs);
    std::optional<possess::Uuid> userUuid_;
    struct ExtraRelog {
        enum class Phase { None, Preparing, Leaving, Rejoining } phase = Phase::None;
        std::string target;
        std::string token;
        std::string key;
        std::string nonce;
        std::uint64_t host = 0;
        possess::Uuid targetUuid{};
        std::vector<std::string> rejoin;
        std::uint64_t generation = 0, since = 0, serial = 0;
        std::int64_t leavingUnique = 0;
        const Extra* leavingConnection = nullptr;
        std::string transfer, where;
        std::uint64_t friendWorld = 0;
        bool toBot() const { return !target.empty(); }
    };
    ExtraRelog extraRelog_;
    std::uint64_t extraRelogGeneration_ = 0;
    chatcommand::Reply extraRelogSwitch(const std::string& target);
    std::optional<chatcommand::Reply> extraRelogBlocked(std::string& key);
    void rememberUserUuid(const Extra* to);
    chatcommand::Reply combinedSwitch(const std::string& name, Extra& host, std::uint8_t sub);
    void startCombined();
    void onForeignPrepared(Result, bool enabled);
    void startExtraRelog();
    void rejoinAfterLeaving(std::uint64_t nowMs, bool timedOut);
    void extraRelogTick(std::uint64_t nowMs);
    Extra* userExtra();
    bool startUserExtra(std::string& error);
    struct Rider { std::string name, host; std::uint64_t since = 0; std::uint8_t sub = 1; };
    std::vector<Rider> riders_;
    struct Home { std::string name, host; };
    std::vector<Home> homes_;
    void setHome(const std::string& name, const std::string& host);
    std::string takeHome(const std::string& name);
    bool sendHome(const std::string& name, std::uint8_t sub, std::uint64_t nowMs);
    std::uint64_t ridersNotBefore_ = 0;
    void leaveRiders();
    void riderTick(std::uint64_t nowMs);
    struct PendingPossess { std::string name; std::uint64_t since = 0, uuidSince = 0; };
    PendingPossess pendingPossess_;
    chatcommand::Reply moveThenPossess(const std::string& name);
    void possessTick(std::uint64_t nowMs);
    void endGameAccount();
    std::optional<std::pair<std::uint64_t, std::int64_t>> mainIds();
    std::mutex sessionMutex_;
    InventoryView inventoryView_;
    BotSession* inventorySession();
    void pumpInventoryView(std::uint64_t now);
    void pumpPlayerView(std::uint64_t now);
    std::array<Slot, net::kSubCount> slots_;
    std::string possessed_;
    std::uint64_t generation_ = 0, playerSerial_ = 0, dropped_ = 0;
    const void* mainPlayer_ = nullptr;
    std::uint64_t mainLogin_ = 0;
    unsigned loggedRegrab_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<bool> leaveRequested_{false}, leaveDone_{false};
    bool possessing_ = false;
    std::uint8_t possessedSub_ = 1;
    chatcommand::Reply relogSwitch(const std::string& target);
    void onRelogPrepared(Result, bool enabled);
    void startRelog();
    std::optional<std::pair<possess::Uuid, possess::Uuid>> relogUuids(const std::string& target) const;
    void rejoinMembers();
    void relogTick(std::uint64_t nowMs);
    void pump();
    static void pumpFromReceive();
    struct Relog {
        enum class Phase { None, Preparing, Rejoining } phase = Phase::None;
        std::string target;
        bool botLoginSent = false;
        struct Member { std::string name; guest::Login seat = {}; bool ready = false, failed = false; std::uint8_t sub = 1; };
        std::vector<Member> members;
        std::uint64_t generation = 0, since = 0, serial = 0, joinAt = 0;
        bool combined = false;
        std::string host;
        std::uint8_t viewSub = 1;
        std::optional<std::pair<possess::Uuid, possess::Uuid>> uuids;
        bool toBot() const { return !target.empty(); }
    };
    Relog relog_;
    std::uint64_t relogGeneration_ = 0;
    std::vector<std::string> pendingChat_;
    chatcommand::Reply skinCommand(const std::vector<std::string>& args);
    chatcommand::Reply stealSkin(const std::vector<std::string>& args);
    void saveStolenSkin(const Job&);
    bool loadSkinSettings(std::string& message);
    std::string currentAccount() const;
    std::string resolveSkin(const std::string& key, std::uint8_t sub) const;
    void requestSkin(const std::string& spec);
    void onSkinResult(Result);
    void applySkins(std::uint64_t nowMs);
    skin::SkinConfig skinSettings_;
    std::string userGamertag_;
    std::uint64_t userGamertagLogin_ = ~0ull;
    struct SkinBuild { bool ready = false, failed = false; std::shared_ptr<const skin::Skin> skin; std::string label, error; };
    std::map<std::string, SkinBuild> skinBuilds_;
    struct SkinSent { std::string stamp, spec, seenStamp; std::uint64_t seenAt = 0, generation = 0; };
    std::map<std::string, SkinSent> skinSent_;
    std::uint64_t skinGeneration_ = 0;
    std::set<std::string> skinReported_;
};
}
