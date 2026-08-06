#include "core/Paths.h"

#include <Windows.h>

#include <system_error>
#include <vector>

namespace tsukuyomi::paths {

namespace {

std::filesystem::path executablePath()
{
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD written = GetModuleFileNameW(nullptr, buffer.data(),
                                                 static_cast<DWORD>(buffer.size()));
        if (written == 0) {
            return {};
        }
        if (written < buffer.size()) {
            return std::filesystem::path(buffer.data(), buffer.data() + written);
        }
        if (buffer.size() >= 32768) {
            return {};
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::filesystem::path computeDataDir()
{
    const std::filesystem::path exe = executablePath();
    if (exe.empty()) {
        return {};
    }

    std::filesystem::path dir = exe.parent_path() / L"Tsukuyomi";

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

}

const std::filesystem::path& dataDir()
{
    static const std::filesystem::path dir = computeDataDir();
    return dir;
}

std::filesystem::path configFile()
{
    const auto& dir = dataDir();
    return dir.empty() ? std::filesystem::path{} : dir / L"Tsukuyomi.json";
}

std::filesystem::path logFile()
{
    const auto& dir = dataDir();
    return dir.empty() ? std::filesystem::path{} : dir / L"Tsukuyomi.log";
}

}
