#include "event_loop.hpp"
#include "channel.hpp"
#include "async_logger.hpp"

#include <sys/eventfd.h>
#include <unistd.h>

namespace kv {

namespace {
int create_eventfd() {
    int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd < 0) throw_sys_error("eventfd");
    return fd;
}
}  // namespace

EventLoop::EventLoop()
    : thread_id_(std::this_thread::get_id()),
      epoller_(std::make_unique<Epoller>()),
      wakeup_fd_(create_eventfd()),
      wakeup_channel_(std::make_unique<Channel>(this, wakeup_fd_)) {
    wakeup_channel_->set_read_callback([this] { handle_wakeup(); });
    wakeup_channel_->enable_reading();
}

EventLoop::~EventLoop() {
    wakeup_channel_->disable_all();
    wakeup_channel_->remove();
    ::close(wakeup_fd_);
}

void EventLoop::loop() {
    looping_ = true;
    quit_ = false;
    LOG_INFO("EventLoop ", this, " start looping");

    while (!quit_) {
        active_channels_.clear();
        epoller_->poll(/*timeout_ms=*/1000, &active_channels_);
        for (Channel* ch : active_channels_) {
            ch->handle_event();
        }
        do_pending_functors();
    }

    looping_ = false;
    LOG_INFO("EventLoop ", this, " stop looping");
}

void EventLoop::quit() {
    quit_ = true;
    // 若在其他线程调用，需要唤醒以尽快退出
    if (!is_in_loop_thread()) {
        wakeup();
    }
}

void EventLoop::update_channel(Channel* channel) {
    epoller_->update_channel(channel);
}

void EventLoop::remove_channel(Channel* channel) {
    epoller_->remove_channel(channel);
}

void EventLoop::run_in_loop(Functor cb) {
    if (is_in_loop_thread()) {
        cb();
    } else {
        queue_in_loop(std::move(cb));
    }
}

void EventLoop::queue_in_loop(Functor cb) {
    {
        std::lock_guard<std::mutex> lock(pending_mu_);
        pending_functors_.push_back(std::move(cb));
    }
    if (!is_in_loop_thread() || calling_pending_) {
        wakeup();
    }
}

void EventLoop::wakeup() {
    uint64_t one = 1;
    ssize_t n = ::write(wakeup_fd_, &one, sizeof(one));
    (void)n;
}

void EventLoop::handle_wakeup() {
    uint64_t one = 0;
    ssize_t n = ::read(wakeup_fd_, &one, sizeof(one));
    (void)n;
}

void EventLoop::do_pending_functors() {
    std::vector<Functor> functors;
    calling_pending_ = true;
    {
        std::lock_guard<std::mutex> lock(pending_mu_);
        functors.swap(pending_functors_);
    }
    for (auto& f : functors) {
        f();
    }
    calling_pending_ = false;
}

}  // namespace kv
