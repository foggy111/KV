#include "async_logger.hpp"

#include <chrono>
#include <ctime>
#include <iostream>

namespace kv {

AsyncLogger& AsyncLogger::instance() {
    static AsyncLogger logger;
    return logger;
}

void AsyncLogger::log(Level level, const std::string& msg) {
    const char* tag = "INFO";
    switch (level) {
        case Level::DEBUG: tag = "DEBUG"; break;
        case Level::INFO:  tag = "INFO";  break;
        case Level::WARN:  tag = "WARN";  break;
        case Level::ERROR: tag = "ERROR"; break;
    }

    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%F %T", std::localtime(&t));

    std::lock_guard<std::mutex> lock(mu_);
    std::cout << '[' << buf << "] [" << tag << "] " << msg << std::endl;
}

}  // namespace kv
