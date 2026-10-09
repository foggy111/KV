#include "channel.hpp"
#include "event_loop.hpp"

namespace kv {

Channel::Channel(EventLoop* loop, int fd) : loop_(loop), fd_(fd) {}

Channel::~Channel() {
    // fd 的关闭由持有者（TcpConnection / TcpServer）负责
}

void Channel::handle_event() {
    // EPOLLHUP 且没有可读：对端关闭
    if ((revents_ & EPOLLHUP) && !(revents_ & EPOLLIN)) {
        if (close_cb_) close_cb_();
    }
    if (revents_ & (EPOLLERR)) {
        if (error_cb_) error_cb_();
    }
    if (revents_ & (EPOLLIN | EPOLLPRI | EPOLLRDHUP)) {
        if (read_cb_) read_cb_();
    }
    if (revents_ & EPOLLOUT) {
        if (write_cb_) write_cb_();
    }
}

void Channel::enable_reading() {
    events_ |= EPOLLIN | EPOLLPRI;
    update();
}

void Channel::enable_writing() {
    events_ |= EPOLLOUT;
    update();
}

void Channel::disable_writing() {
    events_ &= ~EPOLLOUT;
    update();
}

void Channel::disable_all() {
    events_ = 0;
    update();
}

void Channel::remove() {
    loop_->remove_channel(this);
}

void Channel::update() {
    loop_->update_channel(this);
}

}  // namespace kv
