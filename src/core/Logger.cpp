#include "core/Logger.h"

#include "core/Paths.h"
#include "core/Strings.h"

#include <Windows.h>

namespace tsukuyomi {

namespace {

const char* levelTag(LogLevel level)
{
    switch (level) {
    case LogLevel::Success: return "ok  ";
    case LogLevel::Warning: return "warn";
    case LogLevel::Error:   return "err ";
    case LogLevel::Info:
    default:                return "info";
    }
}

}

Logger& Logger::instance()
{
    static Logger logger;
    return logger;
}

void Logger::write(LogLevel level, std::wstring text)
{
    SYSTEMTIME now{};
    GetLocalTime(&now);

    const std::wstring timestamp = std::format(L"{:02}:{:02}:{:02}", now.wHour, now.wMinute, now.wSecond);

    std::lock_guard lock(m_mutex);

    if (!m_fileTried) {
        m_fileTried = true;
        const auto path = paths::logFile();
        if (!path.empty()) {
            m_file.open(path, std::ios::binary | std::ios::trunc);
        }
    }

    if (m_file.is_open()) {
        m_file << toUtf8(timestamp) << " [" << levelTag(level) << "] "
               << toUtf8(text) << "\r\n";
        m_file.flush();
    }
}

}
