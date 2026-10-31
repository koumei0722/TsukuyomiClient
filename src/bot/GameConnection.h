#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace tsukuyomi::bot::gameconn {

class Transport {
public:
    virtual ~Transport() = default;
    virtual bool sendBatch(const std::string& bytes, int reliability) = 0;
    virtual std::optional<std::string> receiveBatch() = 0;
    virtual bool encryptsItself() const = 0;
};

bool initialize();
bool available();
std::int32_t protocolVersion();

class Connection {
public:
    Connection();
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    bool open(Transport& transport, std::string& error);
    bool isOpen() const { return connection_ != nullptr; }
    bool send(const std::string& packet);
    void flush();
    std::optional<std::string> receive();
    bool enableCompression(std::uint16_t threshold, std::uint16_t algorithm);
    bool enableEncryption(const std::string& key);
    void close();
    struct Peer;
private:
    std::unique_ptr<Peer> peer_;
    void* connection_ = nullptr;
    void* outer_ = nullptr;
    void* compressed_ = nullptr;
    void* encrypted_ = nullptr;
    alignas(8) unsigned char received_[32]{};
};

}
