#pragma once

#include "common.hpp"

#include <functional>
#include <sys/epoll.h>

namespace kv {

class EventLoop;

// Channel：把 fd 与其关心的事件、回调绑定在一起（Reactor 核心组件）
class Channel : noncopyable {
public:
    using EventCallback = std::function<void()>;

    Channel(EventLoop* loop, int fd);
    ~Channel();

    void handle_event();

    void set_read_callback(EventCallback cb) { read_cb_ = std::move(cb); }
    void set_write_callback(EventCallback cb) { write_cb_ = std::move(cb); }
    void set_error_callback(EventCallback cb) { error_cb_ = std::move(cb); }
    void set_close_callback(EventCallback cb) { close_cb_ = std::move(cb); }

    int fd() const { return fd_; }
    uint32_t events() const { return events_; }
    void set_revents(uint32_t rev) { revents_ = rev; }

    int index() const { return index_; }
    void set_index(int idx) { index_ = idx; }

    void enable_reading();
    void enable_writing();
    void disable_writing();
    void disable_all();
    void remove();

    EventLoop* owner_loop() const { return loop_; }

private:
    void update();

    EventLoop* loop_;
    const int fd_;
    uint32_t events_{0};
    uint32_t revents_{0};
    int index_{-1};  // -1 = 尚未加入 epoll

    EventCallback read_cb_;
    EventCallback write_cb_;
    EventCallback error_cb_;
    EventCallback close_cb_;
};

}  // namespace kv
