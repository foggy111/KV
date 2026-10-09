#pragma once

#include "common.hpp"
#include "epoller.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace kv {

class Channel;

// EventLoop：单线程事件循环（one loop per thread）
class EventLoop : noncopyable {
public:
    using Functor = std::function<void()>;

    EventLoop();
    ~EventLoop();

    void loop();   // 阻塞跑起来
    void quit();   // 线程安全：请求退出

    void update_channel(Channel* channel);
    void remove_channel(Channel* channel);

    // 把任务投递到本 loop 线程执行（跨线程安全）
    void run_in_loop(Functor cb);
    void queue_in_loop(Functor cb);

    bool is_in_loop_thread() const {
        return thread_id_ == std::this_thread::get_id();
    }

private:
    void do_pending_functors();
    void wakeup();
    void handle_wakeup();

    std::atomic<bool> looping_{false};
    std::atomic<bool> quit_{false};
    const std::thread::id thread_id_;

    std::unique_ptr<Epoller> epoller_;
    std::vector<Channel*> active_channels_;

    // 唤醒用 eventfd
    int wakeup_fd_;
    std::unique_ptr<Channel> wakeup_channel_;

    std::mutex pending_mu_;
    std::vector<Functor> pending_functors_;
    std::atomic<bool> calling_pending_{false};
};

}  // namespace kv
