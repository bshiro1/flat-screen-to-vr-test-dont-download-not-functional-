#include "logging.h"
#include <iostream>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace vrc {

std::ofstream Log::file_;
LogLevel Log::min_level_ = LogLevel::Debug;
std::mutex Log::mutex_;
bool Log::initialized_ = false;

void Log::init(const std::filesystem::path& log_path, LogLevel min_level) {
    {
        std::lock_guard lock(mutex_);
        if (initialized_) return;
        min_level_ = min_level;
        file_.open(log_path, std::ios::out | std::ios::app);
        if (!file_.is_open()) {
            std::cerr << "[VRC] Failed to open log file: " << log_path.string() << std::endl;
        }
        initialized_ = true;
    }
    // Mutex released before calling info() to avoid recursive lock
    info("Logger initialized");
}

void Log::shutdown() {
    std::lock_guard lock(mutex_);
    if (file_.is_open()) {
        info("Logger shutting down");
        file_.close();
    }
    initialized_ = false;
}

void Log::log(LogLevel level, const std::string& msg) {
    if (level < min_level_) return;
    std::lock_guard lock(mutex_);
    auto ts = timestamp();
    auto lv = level_name(level);
    auto line = std::format("[{}] {}: {}", ts, lv, msg);
    if (file_.is_open()) {
        file_ << line << std::endl;
        file_.flush();
    }
    std::cout << line << std::endl;
}

const char* Log::level_name(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
        default: return "UNKNOWN";
    }
}

std::string Log::timestamp() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    std::tm tm;
    localtime_s(&tm, &t);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

} // namespace vrc
