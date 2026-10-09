#pragma once

#include "common.hpp"

#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace kv {

// 最小可用线程池：固定线程数 + 任务队列
// TODO(学生): 支持动态扩缩容、任务优先级、优雅停机超时
class ThreadPool : noncopyable {
public:
    using Task = std::function<void()>;

    explicit ThreadPool(size_t thread_count = 4);
    ~ThreadPool();

    // 提交任务；若已 stop 则直接丢弃并返回 false
    bool enqueue(Task task);

    template <typename F, typename... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<typename std::result_of<F(Args...)>::type> {
        using Ret = typename std::result_of<F(Args...)>::type;
        auto packaged = std::make_shared<std::packaged_task<Ret()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        std::future<Ret> fut = packaged->get_future();
        enqueue([packaged]() { (*packaged)(); });
        return fut;
    }

    void stop();

private:
    std::vector<std::thread> workers_;
    std::queue<Task> tasks_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool stopped_{false};
};

}  // namespace kv
