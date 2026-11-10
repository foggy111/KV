#include "event_loop.hpp"
#include "channel.hpp"
#include "async_logger.hpp"

#include <algorithm>
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
    LOG_INFO("事件循环开始");

    while (!quit_) {
        active_channels_.clear();
        // 顺序：就绪 IO → 到期定时器（TTL 主动扫描）→ 跨线程任务
        epoller_->poll(next_timeout_ms(), &active_channels_);
        for (Channel* ch : active_channels_) {
            ch->handle_event();
        }
        run_due_timers();
        do_pending_functors();
    }

    looping_ = false;
    LOG_INFO("事件循环已停止");
}

void EventLoop::run_every(std::chrono::milliseconds interval, Functor cb) {
    if (interval.count() <= 0) interval = std::chrono::milliseconds(1);
    run_in_loop([this, interval, cb = std::move(cb)]() mutable {
        Timer t;
        t.when = std::chrono::steady_clock::now() + interval;
        t.interval = interval;
        t.repeat = true;
        t.alive = true;
        t.cb = std::move(cb);
        timers_.push_back(std::move(t));
    });
}

int EventLoop::next_timeout_ms() const {
    constexpr int kMaxWaitMs = 1000;
    auto now = std::chrono::steady_clock::now();
    bool any = false;
    auto next = now;
    for (const Timer& t : timers_) {
        if (!t.alive) continue;
        if (!any || t.when < next) {
            next = t.when;
            any = true;
        }
    }
    if (!any) return kMaxWaitMs;
    if (next <= now) return 0;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(next - now).count();
    if (ms > kMaxWaitMs) ms = kMaxWaitMs;
    if (ms < 0) ms = 0;
    return static_cast<int>(ms);
}

void EventLoop::run_due_timers() {
    auto now = std::chrono::steady_clock::now();
    std::vector<Functor> due;
    due.reserve(timers_.size());
    for (Timer& t : timers_) {
        if (!t.alive || t.when > now) continue;
        due.push_back(t.cb);
        if (t.repeat) {
            t.when = now + t.interval;
        } else {
            t.alive = false;
        }
    }
    timers_.erase(std::remove_if(timers_.begin(), timers_.end(),
                                 [](const Timer& t) { return !t.alive; }),
                  timers_.end());
    for (auto& cb : due) {
        cb();
    }
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
