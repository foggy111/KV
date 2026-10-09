#pragma once

#include "common.hpp"

#include <sys/epoll.h>
#include <vector>

namespace kv {

class Channel;

// epoll 封装：创建 epoll fd、增删改事件、wait 返回活跃 Channel
class Epoller : noncopyable {
public:
    explicit Epoller();
    ~Epoller();

    void update_channel(Channel* channel);
    void remove_channel(Channel* channel);

    // 阻塞等待，返回本次就绪的 Channel 列表（通过 out 参数）
    void poll(int timeout_ms, std::vector<Channel*>* active_channels);

private:
    void ctl(int op, Channel* channel);

    static constexpr int kInitEventListSize = 16;

    int epoll_fd_;
    std::vector<epoll_event> events_;
};

}  // namespace kv
