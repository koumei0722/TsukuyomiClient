#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tsukuyomi::bot::crypto { class EcKey; }

namespace tsukuyomi::bot::guest {
class Login;
class Host;
using Sign = std::function<std::optional<std::vector<std::uint8_t>>(const std::string&, const std::string&)>;
std::optional<Login> fromGame(const std::string& gameRequest, const Sign& sign);
std::optional<Login> fromHost(const Host& host, const std::string& name);

class Login {
public:
    Login() = default;
    const std::string& bytes() const { return bytes_; }
    bool empty() const { return bytes_.empty(); }
private:
    explicit Login(std::string bytes) : bytes_(std::move(bytes)) {}
    friend std::optional<Login> fromGame(const std::string&, const Sign&);
    friend std::optional<Login> fromHost(const Host&, const std::string&);
    std::string bytes_;
};

class Host {
public:
    Host() = default;
    Host(std::string token, std::shared_ptr<crypto::EcKey> key, std::string address)
        : token_(std::move(token)), key_(std::move(key)), address_(std::move(address)) {}
    bool known() const { return !token_.empty() && key_; }
private:
    friend std::optional<Login> fromHost(const Host&, const std::string&);
    std::string token_;
    std::shared_ptr<crypto::EcKey> key_;
    std::string address_;
};
}
