#pragma once

#include <atomic>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>

namespace vibe_motion {

enum class LogLevel { error = 0, warning = 1, info = 2, debug = 3 };

class Logger {
  public:
    static Logger& instance() {
        static Logger logger;
        return logger;
    }

    void set_level(LogLevel level) noexcept {
        level_.store(level);
    }
    void set_file(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        file_.open(path, std::ios::app);
        if (!file_) {
            throw std::runtime_error("cannot open log file: " + path);
        }
    }

    template <typename... Values> void write(LogLevel level, Values&&... values) {
        if (static_cast<int>(level) > static_cast<int>(level_.load())) {
            return;
        }
        std::ostringstream message;
        (message << ... << std::forward<Values>(values));
        std::lock_guard<std::mutex> lock(mutex_);
        std::cerr << label(level) << message.str() << '\n';
        if (file_) {
            file_ << label(level) << message.str() << '\n';
            file_.flush();
        }
    }

  private:
    static const char* label(LogLevel level) noexcept {
        switch (level) {
        case LogLevel::error:
            return "ERROR ";
        case LogLevel::warning:
            return "WARN  ";
        case LogLevel::info:
            return "INFO  ";
        case LogLevel::debug:
            return "DEBUG ";
        }
        return "";
    }

    std::atomic<LogLevel> level_{LogLevel::info};
    std::mutex mutex_;
    std::ofstream file_;
};

} // namespace vibe_motion
