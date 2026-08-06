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

void Logger::setNotifier(std::function<void()> notifier)
{
    std::lock_guard lock(m_mutex);
    m_notifier = std::move(notifier);
}

void Logger::write(LogLevel level, std::wstring text)
{
    SYSTEMTIME now{};
    GetLocalTime(&now);

    LogEntry entry;
    entry.level = level;
    entry.timestamp = std::format(L"{:02}:{:02}:{:02}", now.wHour, now.wMinute, now.wSecond);
    entry.text = std::move(text);

    std::function<void()> notifier;
    {
        std::lock_guard lock(m_mutex);

        if (!m_fileTried) {
            m_fileTried = true;
            const auto path = paths::logFile();
            if (!path.empty()) {
                m_file.open(path, std::ios::binary | std::ios::trunc);
            }
        }

        if (m_file.is_open()) {
            m_file << toUtf8(entry.timestamp) << " [" << levelTag(level) << "] "
                   << toUtf8(entry.text) << "\r\n";
            m_file.flush();
        }

        m_entries.push_back(std::move(entry));
        if (m_entries.size() > kMaxEntries) {
            m_entries.erase(m_entries.begin(),
                            m_entries.begin() + static_cast<ptrdiff_t>(m_entries.size() - kMaxEntries));
        }

        notifier = m_notifier;
    }

    if (notifier) {
        notifier();
    }
}

std::vector<LogEntry> Logger::snapshot() const
{
    std::lock_guard lock(m_mutex);
    return m_entries;
}

}
