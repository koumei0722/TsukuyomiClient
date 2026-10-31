#include "Http.h"
#include "core/Strings.h"
#include <Windows.h>
#include <winhttp.h>
#include <algorithm>
#include <limits>

namespace tsukuyomi::bot {
namespace {
char lowerAscii(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; }
bool equalName(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) if (lowerAscii(a[i]) != lowerAscii(b[i])) return false;
    return true;
}
struct Handle {
    HINTERNET value;
    ~Handle() { if (value) WinHttpCloseHandle(value); }
};
HttpResponse failure(std::string_view stage)
{
    return {0, {}, {}, "WinHTTP " + std::to_string(GetLastError()) + " at " + std::string(stage)};
}
}
std::optional<std::string> HttpResponse::header(std::string_view name) const
{
    for (const auto& entry : headers) if (equalName(entry.first, name)) return entry.second;
    return std::nullopt;
}
WinHttp::WinHttp()
{
    session_ = WinHttpOpen(L"Tsukuyomi", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session_ && !WinHttpSetTimeouts(session_, 15000, 15000, 15000, 15000)) {
        WinHttpCloseHandle(session_); session_ = nullptr;
    }
}
WinHttp::~WinHttp() { if (session_) WinHttpCloseHandle(session_); }
HttpResponse WinHttp::send(const HttpRequest& request)
{
    if (!session_) return {0, {}, {}, "WinHTTP session unavailable"};
    const auto url = toUtf16(request.url);
    if (url.size() > (std::numeric_limits<DWORD>::max)() || request.body.size() > (std::numeric_limits<DWORD>::max)())
        return {0, {}, {}, "WinHTTP request too large"};
    URL_COMPONENTS parts{}; parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    parts.dwUserNameLength = parts.dwPasswordLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts)) return failure("URL");
    if (parts.nScheme != INTERNET_SCHEME_HTTPS || parts.dwUserNameLength || parts.dwPasswordLength)
        return {0, {}, {}, "WinHTTP requires HTTPS without URL credentials"};
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (path.empty()) path = L"/";
    if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    Handle connection{WinHttpConnect(session_, host.c_str(), parts.nPort, 0)};
    if (!connection.value) return failure("connect");
    const auto method = toUtf16(request.method);
    Handle handle{WinHttpOpenRequest(connection.value, method.c_str(), path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
    if (!handle.value) return failure("open request");
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(handle.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect))) return failure("redirect policy");
    for (const auto& entry : request.headers) {
        if (entry.first.find_first_of("\r\n:") != std::string::npos || entry.second.find_first_of("\r\n") != std::string::npos)
            return {0, {}, {}, "WinHTTP invalid header"};
        const auto header = toUtf16(entry.first + ": " + entry.second);
        if (!WinHttpAddRequestHeaders(handle.value, header.c_str(), static_cast<DWORD>(-1),
            WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE)) return failure("request headers");
    }
    const auto length = static_cast<DWORD>(request.body.size());
    if (!WinHttpSendRequest(handle.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        length ? const_cast<char*>(request.body.data()) : WINHTTP_NO_REQUEST_DATA, length, length, 0)) return failure("send");
    if (!WinHttpReceiveResponse(handle.value, nullptr)) return failure("receive");
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(handle.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) return failure("status");
    HttpResponse response; response.status = static_cast<int>(status);
    size = 0;
    WinHttpQueryHeaders(handle.value, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
        nullptr, &size, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) return failure("response headers");
    std::vector<wchar_t> raw(size / sizeof(wchar_t) + 1, L'\0');
    if (!WinHttpQueryHeaders(handle.value, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
        raw.data(), &size, WINHTTP_NO_HEADER_INDEX)) return failure("response headers");
    const auto headers = toUtf8(raw.data());
    for (std::size_t begin = headers.find("\r\n") + 2; begin < headers.size();) {
        const auto end = headers.find("\r\n", begin);
        if (end == std::string::npos || end == begin) break;
        const auto colon = headers.find(':', begin);
        if (colon < end) {
            auto value = colon + 1;
            while (value < end && (headers[value] == ' ' || headers[value] == '\t')) ++value;
            auto last = end;
            while (last > value && (headers[last - 1] == ' ' || headers[last - 1] == '\t')) --last;
            response.headers.emplace_back(headers.substr(begin, colon - begin), headers.substr(value, last - value));
        }
        begin = end + 2;
    }
    char buffer[8192];
    for (;;) {
        DWORD read = 0;
        if (!WinHttpReadData(handle.value, buffer, sizeof(buffer), &read)) return failure("body");
        if (!read) break;
        response.body.append(buffer, read);
    }
    return response;
}
HttpResponse CoolingHttp::send(const HttpRequest& request)
{
    constexpr std::chrono::seconds kFirstBackoff{60}, kMaxBackoff{900};
    const auto now = std::chrono::steady_clock::now();
    if (now < state_.until) {
        const auto left = std::chrono::ceil<std::chrono::seconds>(state_.until - now).count();
        return {429, {}, {}, "not sent: waiting " + std::to_string(left) + " s after the server's HTTP 429 (too many requests)"};
    }
    auto response = inner_.send(request);
    if (response.status == 429) {
        std::chrono::seconds wait{0};
        if (const auto value = response.header("Retry-After")) {
            std::size_t used = 0;
            try { const auto seconds = std::stoll(*value, &used); if (used == value->size() && seconds > 0) wait = std::chrono::seconds(seconds); }
            catch (const std::exception&) { }
        }
        state_.backoff = state_.backoff.count() ? (std::min)(state_.backoff * 2, kMaxBackoff) : kFirstBackoff;
        state_.until = std::chrono::steady_clock::now() + (std::min)((std::max)(wait, state_.backoff), kMaxBackoff);
    } else if (response.status >= 200 && response.status < 300) {
        state_.backoff = std::chrono::seconds{0};
    }
    return response;
}
}
