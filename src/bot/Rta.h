#pragma once
#include <string>

namespace tsukuyomi::bot {
class Rta {
public:
    Rta() = default;
    ~Rta();
    Rta(const Rta&) = delete;
    Rta& operator=(const Rta&) = delete;
    std::string connectionId(const std::string& authorization, std::string& error);
    void close();
private:
    void* session_ = nullptr;
    void* connection_ = nullptr;
    void* socket_ = nullptr;
    std::string id_;
};
}
