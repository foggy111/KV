#pragma once

#include "common.hpp"
#include "tcp_connection.hpp"

#include <atomic>
#include <memory>
#include <unordered_map>

namespace kv {

class EventLoop;
class Channel;
class KvStore;

// 监听 socket + accept，把新连接挂到 EventLoop
class TcpServer : noncopyable {
public:
    TcpServer(EventLoop* loop, uint16_t port, KvStore* store);
    ~TcpServer();

    void start();

private:
    void handle_accept();
    void remove_connection(const TcpConnection::Ptr& conn);
    static int create_nonblocking_listen(uint16_t port);

    EventLoop* loop_;
    const uint16_t port_;
    KvStore* store_;
    int listen_fd_;
    std::unique_ptr<Channel> accept_channel_;
    std::unordered_map<std::string, TcpConnection::Ptr> connections_;
    std::atomic<int> next_conn_id_{1};
    bool started_{false};
};

}  // namespace kv
