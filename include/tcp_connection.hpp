#pragma once

#include "buffer.hpp"
#include "channel.hpp"
#include "common.hpp"
#include "protocol.hpp"

#include <functional>
#include <memory>
#include <string>

namespace kv {

class EventLoop;
class KvStore;

// 一条 TCP 连接：读缓冲 + 写缓冲 + 协议解析 + 业务分发
class TcpConnection : public std::enable_shared_from_this<TcpConnection>, noncopyable {
public:
    using Ptr = std::shared_ptr<TcpConnection>;
    using CloseCallback = std::function<void(const Ptr&)>;

    TcpConnection(EventLoop* loop, int fd, std::string name, KvStore* store);
    ~TcpConnection();

    void set_close_callback(CloseCallback cb) { close_cb_ = std::move(cb); }

    // 在 loop 线程中调用
    void connect_established();
    void send(const std::string& msg);
    void shutdown();

    const std::string& name() const { return name_; }
    int fd() const { return fd_; }

private:
    void handle_read();
    void handle_write();
    void handle_close();
    void handle_error();

    void process_commands();
    std::string dispatch(const Command& cmd);

    void send_in_loop(const std::string& msg);

    EventLoop* loop_;
    const int fd_;
    std::string name_;
    KvStore* store_;

    std::unique_ptr<Channel> channel_;
    Buffer input_;
    Buffer output_;
    ProtocolParser parser_;
    CloseCallback close_cb_;
    bool writing_{false};
};

}  // namespace kv
