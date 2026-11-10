#pragma once

#include <atomic>
#include <mutex>
#include <sstream>
#include <string>

namespace kv {

// 名字沿用 AsyncLogger，当前实现是互斥锁保护的同步 stdout，不是异步落盘。
// 请求路径日志会打在事件循环线程上，演示时有用，压测时应关掉。
// 异步队列 + 文件滚动仍未做。
class AsyncLogger {
public:
    enum class Level { DEBUG, INFO, WARN, ERROR };

    static AsyncLogger& instance();

    void set_min_level(Level level);
    void set_request_log(bool on);
    bool request_log() const;

    void log(Level level, const std::string& msg);

    template <typename... Args>
    void info(Args&&... args) {
        log(Level::INFO, join(std::forward<Args>(args)...));
    }

    template <typename... Args>
    void warn(Args&&... args) {
        log(Level::WARN, join(std::forward<Args>(args)...));
    }

    template <typename... Args>
    void error(Args&&... args) {
        log(Level::ERROR, join(std::forward<Args>(args)...));
    }

private:
    AsyncLogger() = default;

    std::atomic<int> min_level_{static_cast<int>(Level::INFO)};
    std::atomic<bool> request_log_{true};

    template <typename T>
    static void append(std::ostringstream& oss, T&& v) {
        oss << std::forward<T>(v);
    }

    template <typename T, typename... Rest>
    static void append(std::ostringstream& oss, T&& v, Rest&&... rest) {
        oss << std::forward<T>(v);
        append(oss, std::forward<Rest>(rest)...);
    }

    template <typename... Args>
    static std::string join(Args&&... args) {
        std::ostringstream oss;
        append(oss, std::forward<Args>(args)...);
        return oss.str();
    }

    std::mutex mu_;
};

#define LOG_INFO(...) ::kv::AsyncLogger::instance().info(__VA_ARGS__)
#define LOG_WARN(...) ::kv::AsyncLogger::instance().warn(__VA_ARGS__)
#define LOG_ERROR(...) ::kv::AsyncLogger::instance().error(__VA_ARGS__)
// 每条请求的路径日志。--quiet 或 set_request_log(false) 时不输出。
#define LOG_REQ(...)                                                       \
    do {                                                                   \
        if (::kv::AsyncLogger::instance().request_log()) {                 \
            ::kv::AsyncLogger::instance().info(__VA_ARGS__);               \
        }                                                                  \
    } while (0)

}  // namespace kv
