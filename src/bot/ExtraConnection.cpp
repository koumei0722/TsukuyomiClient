#include "ExtraConnection.h"
#include "Base64.h"
#include "Crypto.h"
#include "GameCodec.h"
#include "GameKey.h"
#include "Jwt.h"
#include "core/Logger.h"

namespace tsukuyomi::bot {

bool ExtraConnection::start(const Target& target, std::int32_t protocol, std::string request, std::string token,
                            std::shared_ptr<crypto::EcKey> key, std::uint64_t nowMs, std::string& error)
{
    if (phase_ != Phase::Idle) { error = "already started"; return false; }
    if (!gameconn::available()) { error = "the game's connection functions were not found"; return false; }
    if (protocol <= 0 || request.empty()) { error = "no protocol version or login request"; return false; }
    if (!key && !token.empty()) { error = "no login key"; return false; }
    if (!key && target.kind == Kind::RakNet && !gamekey::held()) { error = "the game's login key is unknown"; return false; }
    kind_ = target.kind; protocol_ = protocol; request_ = std::move(request); key_ = key;
    const bool opened = kind_ == Kind::NetherNet ? nether_.open(target.mainPeer, std::move(token), std::move(key), error)
                                                 : rak_.open(target.address, error);
    if (!opened) { phase_ = Phase::Closed; reason_ = error; return false; }
    phase_ = Phase::Opening; since_ = nowMs;
    return true;
}

gameconn::Transport* ExtraConnection::transport()
{
    return kind_ == Kind::NetherNet ? static_cast<gameconn::Transport*>(&nether_) : static_cast<gameconn::Transport*>(&rak_);
}
bool ExtraConnection::transportOpen()
{
    return kind_ == Kind::NetherNet ? nether_.state() == nethernet::Link::State::Open : rak_.state() == raknet::Link::State::Open;
}
bool ExtraConnection::transportClosed(std::string& why)
{
    if (kind_ == Kind::NetherNet) {
        if (nether_.state() != nethernet::Link::State::Closed) return false;
        why = nether_.closeReason();
    } else {
        if (rak_.state() != raknet::Link::State::Closed) return false;
        why = rak_.closeReason();
    }
    return true;
}

bool ExtraConnection::send(const std::vector<std::string>& packets)
{
    if (packets.empty()) return true;
    if (phase_ == Phase::Closed || phase_ == Phase::Idle || !game_.isOpen()) return false;
    for (const auto& packet : packets) {
        if (packet.empty()) { fail("cannot build a packet with the game's functions"); return false; }
        if (!game_.send(packet)) { fail("send failed"); return false; }
    }
    return true;
}

void ExtraConnection::flush() { if (game_.isOpen()) game_.flush(); }

void ExtraConnection::onServerHandshake(protocol::Bytes body)
{
    if (kind_ == Kind::RakNet) {
        protocol::Reader r(body);
        const auto token = r.string();
        const auto header = token ? jwt::readHeader(*token) : std::nullopt;
        const auto payload = token ? jwt::readPayload(*token) : std::nullopt;
        const auto text = [](const std::optional<nlohmann::json>& json, const char* name) -> std::string {
            return json && json->contains(name) && (*json)[name].is_string() ? (*json)[name].get<std::string>() : std::string();
        };
        const auto x5u = text(header, "x5u");
        auto salt = base64::decode(text(payload, "salt"));
        if (!salt) salt = base64::decodeUrl(text(payload, "salt"));
        std::optional<std::vector<std::uint8_t>> secret;
        if (key_) { if (const auto serverKey = base64::decode(x5u)) secret = key_->sharedSecret(*serverKey); }
        else secret = gamekey::sharedSecret(x5u);
        if (!salt || !secret) { fail("cannot read the server's handshake"); return; }
        std::vector<std::uint8_t> input(*salt);
        input.insert(input.end(), secret->begin(), secret->end());
        const auto digest = crypto::sha256(input);
        if (digest.size() != 32 || !game_.enableEncryption(std::string(digest.begin(), digest.end()))) { fail("cannot enable encryption"); return; }
        log().info(L"Bot: extra connection: encryption enabled by the game's connection ({})", key_ ? L"the bot's key" : L"the game's key");
    } else {
        log().info(L"Bot: extra connection got ServerToClientHandshake; replied without enabling encryption");
    }
    send({gamecodec::clientToServerHandshake()});
}

std::vector<std::string> ExtraConnection::pump(std::uint64_t nowMs)
{
    std::vector<std::string> upward;
    if (phase_ == Phase::Idle || phase_ == Phase::Closed) return upward;
    std::string why;
    if (phase_ == Phase::Opening) {
        if (kind_ == Kind::NetherNet) nether_.tick(); else rak_.tick();
        if (transportClosed(why)) { fail(why); return upward; }
        if (!transportOpen()) {
            if (nowMs - since_ > kOpenTimeoutMs) fail(kind_ == Kind::NetherNet ? "the NetherNet session did not open" : "the RakNet connection did not open");
            return upward;
        }
        std::string error;
        if (!game_.open(*transport(), error)) { fail("cannot build the game's connection (" + error + ")"); return upward; }
        if (!send({gamecodec::requestNetworkSettings(protocol_)})) return upward;
        game_.flush();
        phase_ = Phase::Settings; since_ = nowMs;
        log().info(L"Bot: extra connection sent RequestNetworkSettings (protocol {})", protocol_);
    }
    if (phase_ == Phase::Settings && nowMs - since_ > kSettingsTimeoutMs) { fail("no NetworkSettings"); return upward; }
    if (kind_ == Kind::RakNet) rak_.tick();
    if (transportClosed(why)) { fail(why); return upward; }
    for (int count = 0; count < 4096 && phase_ != Phase::Closed; ++count) {
        const auto packet = game_.receive();
        if (!packet) break;
        const auto header = protocol::readHeader(protocol::bytes(*packet));
        if (!header) continue;
        const auto body = protocol::bytes(*packet).subspan(header->bodyOffset);
        switch (header->id) {
        case 0x8f:
            if (const auto settings = gamecodec::readNetworkSettings(body)) {
                if (!game_.enableCompression(settings->threshold, settings->algorithm)) { fail("cannot set compression"); return upward; }
                if (!send({gamecodec::login(protocol_, request_)})) return upward;
                request_.clear();
                phase_ = Phase::LoggingIn;
                log().info(L"Bot: extra connection got NetworkSettings (threshold {}, algorithm {}); sent Login", settings->threshold, settings->algorithm);
            }
            continue;
        case 0x03:
            onServerHandshake(body);
            continue;
        case 0x9c: {
            protocol::Reader r(body);
            const auto type = r.varInt32(), severity = r.varInt32(), id = r.varInt32();
            const auto context = r.string();
            const std::string text = context ? *context : std::string("?");
            log().warn(L"Bot: extra connection got PacketViolationWarning (type {}, severity {}, packet {}): {}", type.value_or(-1),
                severity.value_or(-1), id.value_or(-1), std::wstring(text.begin(), text.end()));
            continue;
        }
        case 0x06:
            if (!send({gamecodec::resourcePackResponse(2)})) return upward;
            continue;
        case 0x07:
            if (!send({gamecodec::resourcePackResponse(3)})) return upward;
            phase_ = Phase::Ready;
            continue;
        default:
            if (header->id < seenIds_.size() && !seenIds_[header->id] && loggedIds_ < 64) {
                seenIds_[header->id] = true; ++loggedIds_;
                log().info(L"Bot: extra connection received id {} (sub {}, {} bytes)", header->id, header->recipient, packet->size());
            }
            upward.push_back(*packet);
        }
    }
    if (phase_ != Phase::Closed) game_.flush();
    return upward;
}

void ExtraConnection::fail(std::string reason)
{
    if (phase_ == Phase::Closed) return;
    log().warn(L"Bot: extra connection closed: {}", std::wstring(reason.begin(), reason.end()));
    close(std::move(reason));
}

void ExtraConnection::close(std::string reason)
{
    if (game_.isOpen()) game_.flush();
    game_.close();
    nether_.close();
    rak_.close();
    if (reason_.empty()) reason_ = reason.empty() ? std::string("closed") : std::move(reason);
    phase_ = Phase::Closed;
}

}
