#include "util/logger.h"
#include <iostream>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <algorithm>

Logger &Logger::instance()
{
    static Logger inst;
    return inst;
}

Logger::Logger() = default;

Logger::~Logger()
{
    closeLogFile();
}

void Logger::setLevel(LogLevel level)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_level = level;
}

void Logger::setLevel(const std::string &levelStr)
{
    std::string lower = levelStr;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower == "debug" || lower == "dbg")
        m_level = LogLevel::Debug;
    else if (lower == "info" || lower == "inf")
        m_level = LogLevel::Info;
    else if (lower == "warn" || lower == "warning" || lower == "wrn")
        m_level = LogLevel::Warn;
    else if (lower == "error" || lower == "err")
        m_level = LogLevel::Error;
}

bool Logger::setLogFile(const std::string &filePath)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_file.open(filePath, std::ios::app);
    return m_file.is_open();
}

void Logger::closeLogFile()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_file.is_open())
        m_file.close();
}

const char *Logger::levelStr(LogLevel l)
{
    switch (l) {
    case LogLevel::Debug: return "DBG";
    case LogLevel::Info:  return "INF";
    case LogLevel::Warn:  return "WRN";
    case LogLevel::Error: return "ERR";
    }
    return "???";
}

void Logger::log(LogLevel level, const char *file, int line, const std::string &msg)
{
    if (level < m_level) return;

    // Timestamp
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::ostringstream oss;
    oss << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << ms.count()
        << " [" << levelStr(level) << "] "
        << msg;

    std::string formatted = oss.str();

    std::lock_guard<std::mutex> lock(m_mutex);

    // Console output
    std::cout << formatted << std::endl;

    // File output
    if (m_file.is_open()) {
        m_file << formatted << std::endl;
    }
}
