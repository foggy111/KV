#include "epoller.hpp"
#include "channel.hpp"
#include "async_logger.hpp"

#include <unistd.h>

namespace kv {

Epoller::Epoller() : epoll_fd_(::epoll_create1(EPOLL_CLOEXEC)), events_(kInitEventListSize) {
    if (epoll_fd_ < 0) {
        throw_sys_error("epoll_create1");
    }
}

Epoller::~Epoller() {
    ::close(epoll_fd_);
}

void Epoller::ctl(int op, Channel* channel) {
    epoll_event ev{};
    ev.events = channel->events();
    ev.data.ptr = channel;
    if (::epoll_ctl(epoll_fd_, op, channel->fd(), &ev) < 0) {
        throw_sys_error("epoll_ctl");
    }
}

void Epoller::update_channel(Channel* channel) {
    // 首次注册用 ADD，之后用 MOD。用 Channel 内部标记简化。
    if (channel->index() < 0) {
        channel->set_index(1);  // 标记已注册
        ctl(EPOLL_CTL_ADD, channel);
    } else {
        ctl(EPOLL_CTL_MOD, channel);
    }
}

void Epoller::remove_channel(Channel* channel) {
    if (channel->index() >= 0) {
        ctl(EPOLL_CTL_DEL, channel);
        channel->set_index(-1);
    }
}

void Epoller::poll(int timeout_ms, std::vector<Channel*>* active_channels) {
    int n = ::epoll_wait(epoll_fd_, events_.data(), static_cast<int>(events_.size()), timeout_ms);
    if (n < 0) {
        if (errno != EINTR) {
            LOG_ERROR("epoll_wait failed: ", std::strerror(errno));
        }
        return;
    }

    for (int i = 0; i < n; ++i) {
        auto* ch = static_cast<Channel*>(events_[i].data.ptr);
        ch->set_revents(events_[i].events);
        active_channels->push_back(ch);
    }

    // 事件数组不够大时扩容
    if (static_cast<size_t>(n) == events_.size()) {
        events_.resize(events_.size() * 2);
    }
}

}  // namespace kv
