#include "async_logger.hpp"

#include <chrono>
#include <ctime>
#include <iostream>

namespace kv {

AsyncLogger& AsyncLogger::instance() {
    static AsyncLogger logger;
    return logger;
}

void AsyncLogger::set_min_level(Level level) {
    min_level_.store(static_cast<int>(level));
}

void AsyncLogger::set_request_log(bool on) { request_log_.store(on); }

bool AsyncLogger::request_log() const { return request_log_.load(); }

void AsyncLogger::log(Level level, const std::string& msg) {
    if (static_cast<int>(level) < min_level_.load()) return;

    const char* tag = "INFO";
    switch (level) {
        case Level::DEBUG: tag = "DEBUG"; break;
        case Level::INFO:  tag = "INFO";  break;
        case Level::WARN:  tag = "WARN";  break;
        case Level::ERROR: tag = "ERROR"; break;
    }

    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%F %T", &tm);

    std::lock_guard<std::mutex> lock(mu_);
    std::cout << '[' << buf << "] [" << tag << "] " << msg << std::endl;
}

}  // namespace kv
