#include "render/PackTexture.h"

#include "core/Logger.h"

#include <Windows.h>

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <filesystem>
#include <atomic>
#include <mutex>
#include <unordered_map>

#pragma comment(lib, "windowscodecs.lib")

namespace tsukuyomi::pack {

constexpr wchar_t SEP = static_cast<wchar_t>(92);

namespace {

std::wstring versionDir()
{
    wchar_t path[MAX_PATH]{};
    const DWORD chars = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (chars == 0 || chars >= MAX_PATH) {
        return {};
    }
    std::wstring full(path, chars);
    const std::size_t cut = full.find_last_of(std::wstring{SEP} + L"/");
    return cut == std::wstring::npos ? std::wstring{} : full.substr(0, cut);
}

std::string runningVersion()
{
    const std::wstring dir = versionDir();
    const std::size_t cut = dir.find_last_of(std::wstring{SEP} + L"/");
    if (cut == std::wstring::npos) {
        return {};
    }
    const std::wstring tail = dir.substr(cut + 1);
    std::string out;
    out.reserve(tail.size());
    for (const wchar_t c : tail) {
        out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    }
    return out;
}

const std::vector<std::wstring>& packDirs()
{
    static const std::vector<std::wstring> dirs = [] {
        std::vector<std::wstring> out;
        const std::wstring root = versionDir();
        if (root.empty()) {
            log().warn(L"PackTexture: could not work out where the resource packs are");
            return out;
        }
        const std::filesystem::path base =
            std::filesystem::path(root) / L"data" / L"resource_packs";
        const int limit = versionRank(runningVersion());
        std::vector<std::pair<int, std::wstring>> ranked;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(base, ec)) {
            if (!entry.is_directory(ec)) {
                continue;
            }
            const std::wstring wide = entry.path().filename().wstring();
            std::string name;
            name.reserve(wide.size());
            for (const wchar_t c : wide) {
                name.push_back(c < 0x80 ? static_cast<char>(c) : '?');
            }
            const int rank = packRank(name, limit);
            if (rank >= 0) {
                ranked.emplace_back(rank, entry.path().wstring());
            }
        }
        std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
            return a.first != b.first ? a.first > b.first : a.second > b.second;
        });
        out.reserve(ranked.size());
        for (auto& one : ranked) {
            out.push_back(std::move(one.second));
        }
        return out;
    }();
    return dirs;
}

Microsoft::WRL::ComPtr<IWICImagingFactory>& factory()
{
    static Microsoft::WRL::ComPtr<IWICImagingFactory> one;
    static std::once_flag once;
    std::call_once(once, [] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(one.GetAddressOf()));
    });
    return one;
}

std::mutex g_openedMutex;
std::unordered_map<std::string, std::wstring> g_opened;
std::atomic<unsigned long long> g_openCalls{0};
std::atomic<unsigned long long> g_imageOpens{0};
thread_local bool t_ourOwnOpen = false;

struct OwnOpenGuard {
    OwnOpenGuard() { t_ourOwnOpen = true; }
    ~OwnOpenGuard() { t_ourOwnOpen = false; }
};

constexpr std::size_t kMaxOpened = 4096;

bool looksLikeImageName(const wchar_t* text, std::size_t chars)
{
    if (text == nullptr || chars < 5) {
        return false;
    }
    const wchar_t* const tail = text + chars - 4;
    const auto same = [](const wchar_t* a, const wchar_t* b) {
        for (int i = 0; i < 4; ++i) {
            wchar_t c = a[i];
            if (c >= L'A' && c <= L'Z') {
                c = static_cast<wchar_t>(c - L'A' + L'a');
            }
            if (c != b[i]) {
                return false;
            }
        }
        return true;
    };
    return same(tail, L".png") || same(tail, L".tga");
}

}

void noteOpenedFile(const wchar_t* ntPath, std::size_t chars)
{
    g_openCalls.fetch_add(1, std::memory_order_relaxed);
    if (t_ourOwnOpen) {
        return;
    }
    if (!looksLikeImageName(ntPath, chars) || chars > 1000) {
        return;
    }
    g_imageOpens.fetch_add(1, std::memory_order_relaxed);
    std::size_t at = std::wstring::npos;
    for (std::size_t i = chars; i-- > 8;) {
        if ((ntPath[i - 8] == L'\\' || ntPath[i - 8] == L'/')
            && (ntPath[i - 7] == L't' || ntPath[i - 7] == L'T') && ntPath[i - 6] == L'e'
            && ntPath[i - 5] == L'x' && ntPath[i - 4] == L't' && ntPath[i - 3] == L'u'
            && ntPath[i - 2] == L'r' && ntPath[i - 1] == L'e') {
            at = i - 7;
            break;
        }
    }
    if (at == std::wstring::npos) {
        return;
    }
    std::string key;
    key.reserve(chars - at);
    for (std::size_t i = at; i + 4 < chars; ++i) {
        const wchar_t c = ntPath[i];
        if (c == L'\\') {
            key.push_back('/');
        } else if (c < 0x80) {
            key.push_back(static_cast<char>(c));
        } else {
            return;
        }
    }
    if (key.size() < 9 || key.compare(0, 9, "textures/") != 0) {
        return;
    }
    std::size_t from = 0;
    if (chars >= 4 && ntPath[0] == L'\\' && ntPath[1] == L'?' && ntPath[2] == L'?'
        && ntPath[3] == L'\\') {
        from = 4;
    }
    std::wstring full(ntPath + from, chars - from);
    const std::lock_guard<std::mutex> lock{g_openedMutex};
    if (g_opened.size() >= kMaxOpened && g_opened.find(key) == g_opened.end()) {
        return;
    }
    g_opened[key] = std::move(full);
}

namespace {

std::wstring observedFile(const std::string& resourcePath)
{
    const std::lock_guard<std::mutex> lock{g_openedMutex};
    const auto it = g_opened.find(resourcePath);
    return it != g_opened.end() ? it->second : std::wstring{};
}

}

std::wstring findFile(const std::string& resourcePath)
{
    if (resourcePath.empty() || resourcePath.size() > 200) {
        return {};
    }
    if (resourcePath.find("..") != std::string::npos) {
        return {};
    }
    std::wstring relative;
    relative.reserve(resourcePath.size());
    for (const char c : resourcePath) {
        if (c == '/') {
            relative.push_back(SEP);
        } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                   || c == '_' || c == '-' || c == '.') {
            relative.push_back(static_cast<wchar_t>(c));
        } else {
            static std::atomic<int> told{0};
            if (told.fetch_add(1, std::memory_order_relaxed) < 4) {
                log().warn(L"PackTexture: the path holds a character that cannot be used "
                           L"{:#04x} ({})",
                           static_cast<unsigned>(static_cast<unsigned char>(c)),
                           std::wstring(resourcePath.begin(), resourcePath.end()));
            }
            return {};
        }
    }
    {
        const OwnOpenGuard guard;
        const std::wstring seen = observedFile(resourcePath);
        if (seen.empty()) {
            static std::atomic<int> told{0};
            if (told.fetch_add(1, std::memory_order_relaxed) < 3) {
                std::string sample;
                {
                    const std::lock_guard<std::mutex> lock{g_openedMutex};
                    for (const auto& one : g_opened) {
                        sample = one.first;
                        break;
                    }
                }
            }
        }
        if (!seen.empty()) {
            std::error_code ec;
            if (std::filesystem::exists(seen, ec)) {
                static std::atomic<int> told{0};
                return seen;
            }
        }
    }
    const std::vector<std::wstring>& dirs = packDirs();
    bool first = true;
    for (const std::wstring& dir : dirs) {
        for (const wchar_t* ext : {L".png", L".tga"}) {
            const std::wstring full = dir + SEP + relative + ext;
            if (first) {
                static std::atomic<bool> told{false};
                first = false;
            }
            std::error_code ec;
            if (std::filesystem::exists(full, ec)) {
                static std::atomic<int> told{0};
                return full;
            }
        }
    }
    return {};
}

bool decodeImage(const std::wstring& file, std::vector<std::uint8_t>& rgba, std::uint32_t& width,
                 std::uint32_t& height)
{
    rgba.clear();
    width = 0;
    height = 0;
    auto& wic = factory();
    if (!wic) {
        return false;
    }
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wic->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ,
                                              WICDecodeMetadataCacheOnDemand,
                                              decoder.GetAddressOf()))) {
        return false;
    }
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.GetAddressOf()))) {
        return false;
    }
    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    if (FAILED(wic->CreateFormatConverter(converter.GetAddressOf()))) {
        return false;
    }
    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
        return false;
    }
    UINT w = 0;
    UINT h = 0;
    if (FAILED(converter->GetSize(&w, &h)) || w == 0 || h == 0 || w > 8192 || h > 8192) {
        return false;
    }
    const std::size_t stride = static_cast<std::size_t>(w) * 4;
    rgba.resize(stride * h);
    if (FAILED(converter->CopyPixels(nullptr, static_cast<UINT>(stride),
                                     static_cast<UINT>(rgba.size()), rgba.data()))) {
        rgba.clear();
        return false;
    }
    width = w;
    height = h;
    return true;
}

}
