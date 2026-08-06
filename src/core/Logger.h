#pragma once

#include <format>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace tsukuyomi {

enum class LogLevel {
    Info,
    Success,
    Warning,
    Error,
};

struct LogEntry {
    LogLevel level = LogLevel::Info;
    std::wstring timestamp;
    std::wstring text;
};

class Logger {
public:
    static Logger& instance();

    void setNotifier(std::function<void()> notifier);

    void write(LogLevel level, std::wstring text);

    template <class... Args>
    void info(std::wformat_string<Args...> fmt, Args&&... args)
    {
        write(LogLevel::Info, std::format(fmt, std::forward<Args>(args)...));
    }

    template <class... Args>
    void success(std::wformat_string<Args...> fmt, Args&&... args)
    {
        write(LogLevel::Success, std::format(fmt, std::forward<Args>(args)...));
    }

    template <class... Args>
    void warn(std::wformat_string<Args...> fmt, Args&&... args)
    {
        write(LogLevel::Warning, std::format(fmt, std::forward<Args>(args)...));
    }

    template <class... Args>
    void error(std::wformat_string<Args...> fmt, Args&&... args)
    {
        write(LogLevel::Error, std::format(fmt, std::forward<Args>(args)...));
    }

    std::vector<LogEntry> snapshot() const;

private:
    Logger() = default;

    static constexpr size_t kMaxEntries = 500;

    mutable std::mutex m_mutex;
    std::vector<LogEntry> m_entries;
    std::function<void()> m_notifier;
    std::ofstream m_file;
    bool m_fileTried = false;
};

inline Logger& log()
{
    return Logger::instance();
}

}
