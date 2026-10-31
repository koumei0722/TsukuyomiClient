#include "Rta.h"
#include "core/Strings.h"
#include <nlohmann/json.hpp>
#include <Windows.h>
#include <winhttp.h>
#include <vector>

namespace tsukuyomi::bot {
namespace {
constexpr wchar_t kHost[] = L"rta.xboxlive.com";
constexpr char kConnections[] = "https://sessiondirectory.xboxlive.com/connections/";
constexpr int kMaxMessages = 16;
constexpr std::size_t kMaxMessage = 64 * 1024;

std::string winHttpError(const char* stage, DWORD code = GetLastError())
{
    return "WinHTTP " + std::to_string(code) + " at RTA " + stage;
}
bool receiveMessage(HINTERNET socket, std::string& message, std::string& error)
{
    message.clear();
    char buffer[4096];
    for (;;) {
        DWORD read = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
        if (const DWORD code = WinHttpWebSocketReceive(socket, buffer, sizeof buffer, &read, &type); code != NO_ERROR) {
            error = winHttpError("receive", code);
            return false;
        }
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) { error = "RTA closed the connection"; return false; }
        message.append(buffer, read);
        if (message.size() > kMaxMessage) { error = "RTA message too large"; return false; }
        if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) return true;
    }
}
}

Rta::~Rta() { close(); }

void Rta::close()
{
    if (socket_) {
        WinHttpWebSocketClose(static_cast<HINTERNET>(socket_), WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
        WinHttpCloseHandle(static_cast<HINTERNET>(socket_));
        socket_ = nullptr;
    }
    if (connection_) { WinHttpCloseHandle(static_cast<HINTERNET>(connection_)); connection_ = nullptr; }
    if (session_) { WinHttpCloseHandle(static_cast<HINTERNET>(session_)); session_ = nullptr; }
    id_.clear();
}

std::string Rta::connectionId(const std::string& authorization, std::string& error)
{
    if (!id_.empty()) return id_;
    close();
    if (authorization.find_first_of("\r\n") != std::string::npos) { error = "RTA invalid authorization"; return {}; }
    session_ = WinHttpOpen(L"Tsukuyomi", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session_) { error = winHttpError("open"); return {}; }
    if (!WinHttpSetTimeouts(static_cast<HINTERNET>(session_), 15000, 15000, 15000, 15000)) { error = winHttpError("timeouts"); close(); return {}; }
    connection_ = WinHttpConnect(static_cast<HINTERNET>(session_), kHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connection_) { error = winHttpError("connect"); close(); return {}; }
    const HINTERNET request = WinHttpOpenRequest(static_cast<HINTERNET>(connection_), L"GET", L"/connect", nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) { error = winHttpError("open request"); close(); return {}; }
    bool upgraded = false;
    {
        DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        const auto headers = toUtf16("Authorization: " + authorization + "\r\nSec-WebSocket-Protocol: rta.xboxlive.com.V2\r\n");
        DWORD status = 0, size = sizeof status;
        if (!WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof redirect)
            || !WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) error = winHttpError("upgrade option");
        else if (!WinHttpAddRequestHeaders(request, headers.c_str(), static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE))
            error = winHttpError("headers");
        else if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) error = winHttpError("send");
        else if (!WinHttpReceiveResponse(request, nullptr)) error = winHttpError("receive response");
        else if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                     &status, &size, WINHTTP_NO_HEADER_INDEX)) error = winHttpError("status");
        else if (status != 101) error = "HTTP " + std::to_string(status) + " at RTA connect";
        else if (socket_ = WinHttpWebSocketCompleteUpgrade(request, 0); !socket_) error = winHttpError("complete upgrade");
        else upgraded = true;
    }
    WinHttpCloseHandle(request);
    if (!upgraded) { close(); return {}; }
    const std::string subscribe = nlohmann::json::array({1, 1, kConnections}).dump();
    if (const DWORD code = WinHttpWebSocketSend(static_cast<HINTERNET>(socket_), WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
            const_cast<char*>(subscribe.data()), static_cast<DWORD>(subscribe.size())); code != NO_ERROR) {
        error = winHttpError("send subscribe", code); close(); return {};
    }
    for (int i = 0; i < kMaxMessages; ++i) {
        std::string message;
        if (!receiveMessage(static_cast<HINTERNET>(socket_), message, error)) { close(); return {}; }
        const auto reply = nlohmann::json::parse(message, nullptr, false);
        if (!reply.is_array() || reply.size() < 3 || reply[0] != 1 || reply[1] != 1) continue;
        if (!reply[2].is_number_integer() || reply[2].get<int>() != 0) {
            error = "RTA refused the subscription (status " + reply[2].dump() + ")"; close(); return {};
        }
        const auto& data = reply.size() > 4 ? reply[4] : nlohmann::json();
        if (data.is_object() && data.contains("ConnectionId") && data["ConnectionId"].is_string() && !data["ConnectionId"].get<std::string>().empty()) {
            id_ = data["ConnectionId"].get<std::string>();
            return id_;
        }
        error = "RTA answered without a connection id"; close(); return {};
    }
    error = "RTA did not answer the subscription";
    close();
    return {};
}
}
