#pragma once
#include <cstdint>
#include <string>
#include <mutex>
#include <atomic>
#include <cstdarg>

class Logger
{
public:
    enum class Level { DEBUG = 0, WARN = 1, ERR = 2, SYSTEM = 3 };

    static Logger& instance();

    void setDirectory(std::wstring dir);
    void setLevel(Level level);
    void log(const wchar_t* type, Level level, const wchar_t* format, ...);
    void vlog(const wchar_t* type, Level level, const wchar_t* format, va_list va);
    void logHex(const wchar_t* type, Level level, const wchar_t* tag, const uint8_t* data, int size);

private:
    Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void writeEntry(const wchar_t* type, Level level, const wchar_t* message);
    static const wchar_t* levelToString(Level level);

    std::wstring          _directory{ L"Logs" };
    Level                 _minLevel{ Level::DEBUG };
    std::mutex            _mutex;
    std::atomic<uint64_t> _counter{ 0 };
};

void Log(const wchar_t* type, Logger::Level level, const wchar_t* format, ...);
void LogHex(const wchar_t* type, Logger::Level level, const wchar_t* tag, const uint8_t* data, int size);
