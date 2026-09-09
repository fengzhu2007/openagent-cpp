#pragma once
#include <string>
#include <fstream>
#include <mutex>

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

class Logger {
public:
    static Logger &instance();

    void setLevel(LogLevel level);
    void setLevel(const std::string &levelStr);
    LogLevel level() const { return m_level; }

    // Enable file output
    bool setLogFile(const std::string &filePath);
    void closeLogFile();

    void log(LogLevel level, const char *file, int line, const std::string &msg);

private:
    Logger();
    ~Logger();

    static const char *levelStr(LogLevel l);

    LogLevel m_level = LogLevel::Info;
    std::ofstream m_file;
    std::mutex m_mutex;
};

#define LOG_DEBUG(msg) Logger::instance().log(LogLevel::Debug, __FILE__, __LINE__, msg)
#define LOG_INFO(msg)  Logger::instance().log(LogLevel::Info, __FILE__, __LINE__, msg)
#define LOG_WARN(msg)  Logger::instance().log(LogLevel::Warn, __FILE__, __LINE__, msg)
#define LOG_ERROR(msg) Logger::instance().log(LogLevel::Error, __FILE__, __LINE__, msg)
