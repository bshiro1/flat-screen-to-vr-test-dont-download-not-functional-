#pragma once

#include "types.h"
#include <string>
#include <format>
#include <fstream>
#include <mutex>
#include <filesystem>
#include <source_location>

namespace vrc {

class Log {
public:
    static void init(const std::filesystem::path& log_path, LogLevel min_level = LogLevel::Debug);
    static void shutdown();

    template<typename... Args>
    static void trace(std::string_view fmt, Args&&... args) {
        log(LogLevel::Trace, std::vformat(fmt, std::make_format_args(args...)));
    }

    template<typename... Args>
    static void debug(std::string_view fmt, Args&&... args) {
        log(LogLevel::Debug, std::vformat(fmt, std::make_format_args(args...)));
    }

    template<typename... Args>
    static void info(std::string_view fmt, Args&&... args) {
        log(LogLevel::Info, std::vformat(fmt, std::make_format_args(args...)));
    }

    template<typename... Args>
    static void warn(std::string_view fmt, Args&&... args) {
        log(LogLevel::Warn, std::vformat(fmt, std::make_format_args(args...)));
    }

    template<typename... Args>
    static void error(std::string_view fmt, Args&&... args) {
        log(LogLevel::Error, std::vformat(fmt, std::make_format_args(args...)));
    }

    template<typename... Args>
    static void fatal(std::string_view fmt, Args&&... args) {
        log(LogLevel::Fatal, std::vformat(fmt, std::make_format_args(args...)));
    }

    static void log(LogLevel level, const std::string& msg);

private:
    static const char* level_name(LogLevel level);
    static std::string timestamp();

    static std::ofstream file_;
    static LogLevel min_level_;
    static std::mutex mutex_;
    static bool initialized_;
};

} // namespace vrc
