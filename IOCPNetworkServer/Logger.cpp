#include "Logger.h"
#include <Windows.h>
#include <strsafe.h>
#include <filesystem>
#include <fstream>

Logger& Logger::instance()
{
    static Logger inst;
    return inst;
}

void Logger::setDirectory(std::wstring dir)
{
    std::lock_guard lock(_mutex);
    _directory = std::move(dir);
    std::filesystem::create_directories(_directory);
}

void Logger::setLevel(Level level)
{
    std::lock_guard lock(_mutex);
    _minLevel = level;
}

void Logger::log(const wchar_t* type, Level level, const wchar_t* format, ...)
{
    va_list va;
    va_start(va, format);
    vlog(type, level, format, va);
    va_end(va);
}

void Logger::vlog(const wchar_t* type, Level level, const wchar_t* format, va_list va)
{
    {
        std::lock_guard lock(_mutex);
        if (static_cast<int>(level) < static_cast<int>(_minLevel)) return;
    }

    wchar_t message[1024];
    StringCchVPrintfW(message, 1024, format, va);
    writeEntry(type, level, message);
}

void Logger::logHex(const wchar_t* type, Level level, const wchar_t* tag, const uint8_t* data, int size)
{
    {
        std::lock_guard lock(_mutex);
        if (static_cast<int>(level) < static_cast<int>(_minLevel)) return;
    }

    std::wstring hex;
    hex.reserve(static_cast<size_t>(size) * 3 + 64);
    hex += tag;
    hex += L" | ";

    wchar_t buf[4];
    for (int i = 0; i < size; ++i)
    {
        StringCchPrintfW(buf, 4, L"%02X ", data[i]);
        hex += buf;
    }

    writeEntry(type, level, hex.c_str());
}

void Logger::writeEntry(const wchar_t* type, Level level, const wchar_t* message)
{
    SYSTEMTIME st;
    GetLocalTime(&st);

    uint64_t counter = ++_counter;

    wchar_t line[2048];
    StringCchPrintfW(line, 2048, L"[%s] [%04d-%02d-%02d %02d:%02d:%02d / %s / %09llu] %s\n",
        type,
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond,
        levelToString(level), counter, message);

    wchar_t yearMonth[8];
    StringCchPrintfW(yearMonth, 8, L"%04d%02d", st.wYear, st.wMonth);

    std::lock_guard lock(_mutex);

    wprintf(L"%s", line);

    std::wstring path = _directory + L"\\" + yearMonth + L"_" + type + L".txt";
    std::wofstream file(path, std::ios::app);
    if (file.is_open())
        file << line;
}

const wchar_t* Logger::levelToString(Level level)
{
    switch (level)
    {
    case Level::DEBUG:  return L"DEBUG";
    case Level::WARN:   return L"WARNG";
    case Level::ERR:    return L"ERROR";
    case Level::SYSTEM: return L"SYSTM";
    default:            return L"UNKNW";
    }
}

void Log(const wchar_t* type, Logger::Level level, const wchar_t* format, ...)
{
    va_list va;
    va_start(va, format);
    Logger::instance().vlog(type, level, format, va);
    va_end(va);
}

void LogHex(const wchar_t* type, Logger::Level level, const wchar_t* tag, const uint8_t* data, int size)
{
    Logger::instance().logHex(type, level, tag, data, size);
}
