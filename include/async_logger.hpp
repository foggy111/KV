#pragma once

#include <mutex>
#include <sstream>
#include <string>

namespace kv {

// 简易 stdout 日志：带互斥锁，足够学习用。
// TODO(学生): 改成异步队列 + 后台线程写文件，避免阻塞 Reactor 线程。
class AsyncLogger {
public:
    enum class Level { DEBUG, INFO, WARN, ERROR };

    static AsyncLogger& instance();

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

}  // namespace kv
