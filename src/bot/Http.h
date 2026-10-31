#pragma once
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tsukuyomi::bot {
struct HttpRequest {
    std::string method, url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
};
struct HttpResponse {
    int status = 0;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body, error;
    std::optional<std::string> header(std::string_view name) const;
};
class IHttp {
public:
    virtual ~IHttp() = default;
    virtual HttpResponse send(const HttpRequest&) = 0;
};
class WinHttp final : public IHttp {
public:
    WinHttp();
    ~WinHttp() override;
    WinHttp(const WinHttp&) = delete;
    WinHttp& operator=(const WinHttp&) = delete;
    HttpResponse send(const HttpRequest&) override;
private:
    void* session_ = nullptr;
};
struct Cooldown {
    std::chrono::steady_clock::time_point until{};
    std::chrono::seconds backoff{0};
};
class CoolingHttp final : public IHttp {
public:
    CoolingHttp(IHttp& inner, Cooldown& state) : inner_(inner), state_(state) {}
    HttpResponse send(const HttpRequest&) override;
private:
    IHttp& inner_;
    Cooldown& state_;
};
}
