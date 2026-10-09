#include "thread_pool.hpp"
#include "async_logger.hpp"

namespace kv {

ThreadPool::ThreadPool(size_t thread_count) {
    if (thread_count == 0) thread_count = 1;
    workers_.reserve(thread_count);
    for (size_t i = 0; i < thread_count; ++i) {
        workers_.emplace_back([this] {
            for (;;) {
                Task task;
                {
                    std::unique_lock<std::mutex> lock(mu_);
                    cv_.wait(lock, [this] { return stopped_ || !tasks_.empty(); });
                    if (stopped_ && tasks_.empty()) return;
                    task = std::move(tasks_.front());
                    tasks_.pop();
                }
                task();
            }
        });
    }
    LOG_INFO("ThreadPool started with ", thread_count, " workers");
}

ThreadPool::~ThreadPool() {
    stop();
}

bool ThreadPool::enqueue(Task task) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (stopped_) return false;
        tasks_.push(std::move(task));
    }
    cv_.notify_one();
    return true;
}

void ThreadPool::stop() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (stopped_) return;
        stopped_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
    workers_.clear();
}

}  // namespace kv
