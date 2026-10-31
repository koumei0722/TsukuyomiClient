#include "InventoryView.h"
#include "BotNet.h"
#include "ContainerScreen.h"
#include "GameCodec.h"
#include "core/Logger.h"
#include "game/ClientChat.h"
#include "game/ContainerUi.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <limits>
#include <mutex>
#include <utility>

namespace tsukuyomi::bot {
namespace {
std::atomic<bool> g_intercept{false};
std::mutex g_queueMutex;
struct Queued { std::uint16_t id; std::string body; };
std::vector<Queued> g_queue;
void toGame(std::string packet) { net::schedule(std::move(packet), 0); }
int g_logs = 0;
bool canLog() { return g_logs < 80 && ++g_logs; }
}
bool InventoryView::filterSend(std::uint16_t id, protocol::Bytes body)
{
    if (!g_intercept.load(std::memory_order_acquire)) return false;
    if (id != 0x93 && !(id == 0x2f && !body.empty() && body[0] == kWindow)) return false;
    std::lock_guard lock(g_queueMutex);
    if (!g_intercept.load(std::memory_order_relaxed)) return false;
    g_queue.push_back({id, std::string(reinterpret_cast<const char*>(body.data()), body.size())});
    return true;
}
InventoryView::Shown InventoryView::shown(const Item& item)
{
    return item.empty() ? Shown{} : Shown{false, item.netId, item.count};
}
std::optional<std::string> InventoryView::prepare(const std::string& name)
{
    if (!gamecodec::inventoryViewAvailable()) return "the game's inventory view packets are unavailable (see Tsukuyomi.log)";
    if (!containerscreen::available()) return "the chest screen is unavailable (see Tsukuyomi.log)";
    if (!net::connected()) return "the main player is not connected";
    if (containerui::hasScreen()) return "close the current container screen first";
    const auto input = net::lastGameInput();
    if (!input) return "the main player's position is unknown";
    const double x = std::floor(input->position.x), y = std::floor(static_cast<double>(input->position.y) - 1.62),
                 z = std::floor(input->position.z);
    const auto inRange = [](double n) { return std::isfinite(n) && n >= (std::numeric_limits<std::int32_t>::min)()
        && n <= (std::numeric_limits<std::int32_t>::max)(); };
    if (!inRange(x) || !inRange(y) || !inRange(z)) return "the main player's position is invalid";
    position_ = {static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), static_cast<std::int32_t>(z)};
    log().info(L"Bot: inventory view of {} at ({}, {}, {})", std::wstring(name.begin(), name.end()), position_[0], position_[1], position_[2]);
    return std::nullopt;
}
void InventoryView::begin(const std::string& name, std::uint64_t now)
{
    botName_ = name; shown_ = {}; cursor_ = {};
    openAt_ = now; state_ = State::Opening;
    std::lock_guard lock(g_queueMutex);
    g_queue.clear(); g_intercept.store(true, std::memory_order_release);
}
chatcommand::Reply InventoryView::open(const std::string& botName, BotSession& session, std::uint64_t now)
{
    const auto refuse = [](std::string reason) -> chatcommand::Reply { return {{"Bot: " + reason}, true}; };
    if (isOpen()) return refuse("an inventory view is already open");
    if (!session.canAct()) return refuse("cannot open inventory before spawning, while dead, or on a classic connection");
    if (auto reason = prepare(botName)) return refuse(std::move(*reason));
    if (!session.openInventoryOnServer()) { abandon(); return refuse("cannot open the bot's inventory on the server"); }
    session.takeStackResponses();
    snapshot_.reset(); spawnSerial_ = session.spawnSerial();
    begin(botName, now);
    return {{"Bot: opening " + botName + "'s inventory"}, false};
}
chatcommand::Reply InventoryView::openSnapshot(const std::string& name, BotSession::InventorySnapshot snapshot, std::uint64_t now)
{
    const auto refuse = [](std::string reason) -> chatcommand::Reply { return {{"Bot: " + reason}, true}; };
    if (isOpen()) return refuse("an inventory view is already open");
    if (auto reason = prepare(name)) return refuse(std::move(*reason));
    snapshot.cursor = {};
    snapshot_ = std::move(snapshot); spawnSerial_ = 0;
    begin(name, now);
    return {{"Bot: opening " + name + "'s inventory (read only)"}, false};
}
void InventoryView::updateShown(const BotSession::InventorySnapshot& snapshot, bool initial)
{
    int pushed = 0;
    for (std::uint8_t f = 0; f < shown_.size(); ++f) {
        auto c = kChestContainer, s = f;
        Item item;
        if (inventorylayout::toBot(c, s)) {
            if (c == 28 || c == 29) item = snapshot.slots[s];
            else if (c == 6) item = snapshot.armor[s];
            else if (c == 34) item = snapshot.offhand;
        }
        if (!item.empty() && !item.handle) continue;
        const auto value = shown(item);
        if (initial || shown_[f] != value) {
            auto packet = gamecodec::inventorySlotToGame(kWindow, f, item);
            if (!packet.empty()) { toGame(std::move(packet)); shown_[f] = value; ++pushed; }
        }
    }
    const auto& item = snapshot.cursor;
    const bool cursorUnknown = !item.empty() && !item.handle;
    bool cursorPushed = false;
    if (!cursorUnknown) {
        const auto value = shown(item);
        if (initial || cursor_ != value) {
            auto packet = gamecodec::inventorySlotToGame(124, 0, item);
            if (!packet.empty()) { toGame(std::move(packet)); cursor_ = value; cursorPushed = true; }
        }
    }
    if ((pushed || cursorPushed) && canLog())
        log().info(L"Bot: inventory view sent {} slot(s){} (bot's cursor: {})", pushed, cursorPushed ? L" and the cursor" : L"",
            item.empty() ? std::wstring(L"empty") : std::format(L"{} x{}{}", item.netId, item.count, cursorUnknown ? L" without a descriptor" : L""));
}
void InventoryView::tick(BotSession* session, std::uint64_t now)
{
    if (!isOpen()) return;
    if (snapshot_) session = nullptr;
    else if (!session || !session->canAct() || session->spawnSerial() != spawnSerial_) { close(true, session); return; }
    if (session) {
        std::erase_if(pendingRequests_, [&](const auto& entry) {
            return session->tick() - entry.second >= BotSession::kInventoryResponseWaitTicks;
        });
    }
    std::vector<Queued> queue;
    { std::lock_guard lock(g_queueMutex); queue.swap(g_queue); }
    if (state_ == State::Closing) { close(closeServerInitiated_, session); return; }
    for (const auto& packet : queue) {
        if (packet.id == 0x2f) { close(false, session); return; }
        if (snapshot_) {
            std::vector<std::int32_t> ids;
            gamecodec::translateStackRequest(protocol::bytes(packet.body), 0, [](std::uint8_t&, std::uint8_t&) { return false; }, ids);
            toGame(gamecodec::stackFailureToGame(ids));
            forgetShown();
            if (canLog()) log().info(L"Bot: read-only inventory view refused request {}", ids.empty() ? 0 : ids[0]);
            continue;
        }
        std::vector<std::int32_t> ids;
        const auto map = [](std::uint8_t& c, std::uint8_t& slot) {
            if (c != kChestContainer && c != 59) {
                static bool logged = false;
                if (!std::exchange(logged, true))
                    log().info(L"Bot: inventory view refuses requests that touch container {} (only the chest {} and the cursor 59 are the bot's)", c, kChestContainer);
            }
            return inventorylayout::toBot(c, slot);
        };
        auto translated = gamecodec::translateStackRequest(protocol::bytes(packet.body), session->sub(), map, ids);
        if (translated && session->sendPacket(std::move(*translated), "itemStackRequest")) {
            for (const auto id : ids) pendingRequests_[id] = session->tick();
            if (canLog()) log().info(L"Bot: inventory view sent request {} as the bot", ids.empty() ? 0 : ids[0]);
            continue;
        }
        toGame(gamecodec::stackFailureToGame(ids));
        forgetShown();
        if (canLog()) log().info(L"Bot: inventory view refused request {} ({} bytes)", ids.empty() ? 0 : ids[0], packet.body.size());
    }
    if (state_ == State::Opening) {
        if (auto reason = containerscreen::open(botName_, kWindow, position_[0], position_[1], position_[2])) {
            clientchat::printLocal("[Tsukuyomi] Bot: " + *reason);
            if (session) session->closeInventoryOnServer();
            abandon();
            return;
        }
        state_ = State::Assigning; openAt_ = now;
        return;
    }
    if (containerscreen::gone()) { close(false, session); return; }
    if (state_ == State::Assigning) {
        if (!containerscreen::ready()) {
            if (now - openAt_ > 2000) {
                log().warn(L"Bot: the chest screen did not take the window number; closing the inventory view");
                close(true, session);
            }
            return;
        }
        state_ = State::Open;
        updateShown(snapshot_ ? *snapshot_ : session->inventory(), true);
    } else updateShown(snapshot_ ? *snapshot_ : session->inventory(), false);
}
void InventoryView::onResponse(protocol::Bytes body)
{
    if (!isOpen()) return;
    if (const auto responses = codec().readItemStackResponse(body))
        for (const auto& response : *responses) pendingRequests_.erase(response.requestId);
    if (state_ == State::Closing) return;
    std::vector<gamecodec::ResponseSlot> seen;
    auto translated = gamecodec::translateStackResponseToGame(body, inventorylayout::toChest, seen);
    if (canLog()) {
        std::wstring statuses;
        if (const auto responses = codec().readItemStackResponse(body))
            for (const auto& r : *responses) statuses += std::format(L" {}:{}", r.requestId, r.status);
        log().info(L"Bot: inventory view got a response{} ({} slot(s){})", statuses, seen.size(), translated ? L"" : L"; not handed to the game");
    }
    if (!translated) { forgetShown(); return; }
    toGame(std::move(*translated));
    if (const auto responses = codec().readItemStackResponse(body);
        !responses || std::any_of(responses->begin(), responses->end(), [](const StackResponse& r) { return r.status != 0; })) {
        forgetShown();
        return;
    }
    for (const auto& slot : seen) {
        const auto value = slot.count && slot.netId ? Shown{false, slot.netId, slot.count} : Shown{};
        if (slot.container == kChestContainer && slot.slot < shown_.size()) shown_[slot.slot] = value;
        else if (slot.container == 59 && slot.slot == 0) cursor_ = value;
    }
}
void InventoryView::forgetShown()
{
    const Shown unknown{false, -1, 0xffff};
    shown_.fill(unknown); cursor_ = unknown;
}
void InventoryView::close(bool serverInitiated, BotSession* session)
{
    if (!isOpen()) return;
    state_ = State::Closing;
    closeServerInitiated_ = closeServerInitiated_ || serverInitiated;
    if (session && session->canAct() && session->spawnSerial() == spawnSerial_) {
        if (!pendingRequests_.empty()) return;
        const auto snapshot = session->inventory();
        if (!cursorReturnSent_ && !snapshot.cursor.empty() && snapshot.cursor.netId && snapshot.cursor.count <= 255) {
            std::optional<std::pair<std::uint8_t, std::uint8_t>> destination;
            for (std::uint8_t i = 0; i < snapshot.slots.size() && !destination; ++i)
                if (snapshot.slots[i].empty()) destination = std::pair<std::uint8_t, std::uint8_t>{static_cast<std::uint8_t>(i < 9 ? 28 : 29), i};
            static std::int32_t nextRequest = -1000001;
            const auto request = nextRequest; nextRequest -= 2;
            const bool sent = session->sendPacket(gamecodec::cursorRequest(session->sub(), request, static_cast<std::uint8_t>(snapshot.cursor.count),
                                                                        snapshot.cursor.netId, destination), "itemStackRequest");
            cursorReturnSent_ = true;
            if (sent) pendingRequests_[request] = session->tick();
            if (canLog()) log().info(L"Bot: inventory view {} the bot's cursor ({} x{}){}", destination ? L"put back" : L"dropped",
                snapshot.cursor.netId, snapshot.cursor.count, sent ? L"" : L" (cannot build the request)");
        }
        if (!pendingRequests_.empty()) return;
        session->closeInventoryOnServer();
    }
    toGame(gamecodec::containerCloseToGame(kWindow, 0, closeServerInitiated_));
    toGame(gamecodec::inventorySlotToGame(124, 0, {}));
    abandon();
}
void InventoryView::abandon()
{
    std::lock_guard lock(g_queueMutex);
    g_intercept.store(false, std::memory_order_release); g_queue.clear();
    if (state_ != State::Closed) containerscreen::finish();
    state_ = State::Closed; botName_.clear(); snapshot_.reset(); shown_ = {}; cursor_ = {};
    spawnSerial_ = 0; openAt_ = 0; position_ = {};
    pendingRequests_.clear(); closeServerInitiated_ = false; cursorReturnSent_ = false;
}
}
