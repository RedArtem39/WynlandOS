#ifndef HYPRUTILS_CLI_LOGGER_HPP
#define HYPRUTILS_CLI_LOGGER_HPP

#include <string>
#include <format>
#include <iostream>
#include <hyprutils/memory/SharedPtr.hpp>
#include <hyprutils/memory/WeakPtr.hpp>

namespace Hyprutils {
namespace CLI {

enum eLogLevel {
    LOG_NONE  = 0,
    LOG_TRACE = 1,
    LOG_DEBUG = 2,
    LOG_INFO  = 3,
    LOG_WARN  = 4,
    LOG_ERR   = 5,
    LOG_CRIT  = 6
};

class CLogger {
public:
    template <typename... Args>
    void log(eLogLevel level, const std::string& fmt, Args&&... args) {
        // Freestanding log wrapper
    }
    void setLogLevel(eLogLevel level) {}
    void log(eLogLevel level, const std::string_view& str) {}
    void setOutputFile(const std::string& str) {}
    void setEnableRolling(bool b) {}
    void setEnableColor(bool b) {}
    void setEnableStdout(bool b) {}
    void setTime(bool b) {}
    const std::string& rollingLog() { static std::string s; return s; }
};

class CLoggerConnection {
public:
    CLoggerConnection(CLogger& logger) {}
    CLoggerConnection(CLogger* logger) {}
    CLoggerConnection(Memory::CSharedPointer<CLogger> logger) {}
    CLoggerConnection(Memory::CWeakPointer<CLogger> logger) {}
    ~CLoggerConnection() = default;

    void setLogLevel(eLogLevel level) {}
    void setName(const std::string& name) {}
};

}
}

using eLogLevel = Hyprutils::CLI::eLogLevel;
using Hyprutils::CLI::CLogger;
using Hyprutils::CLI::CLoggerConnection;

#endif /* HYPRUTILS_CLI_LOGGER_HPP */
