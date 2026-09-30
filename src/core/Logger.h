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

class Logger {
public:
    static Logger& instance();

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

private:
    Logger() = default;

    std::mutex m_mutex;
    std::ofstream m_file;
    bool m_fileTried = false;
};

inline Logger& log()
{
    return Logger::instance();
}

}
