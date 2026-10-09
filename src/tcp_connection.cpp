#include "tcp_connection.hpp"
#include "async_logger.hpp"
#include "event_loop.hpp"
#include "kv_store.hpp"

#include <cerrno>
#include <unistd.h>
#include <sys/socket.h>

namespace kv {

TcpConnection::TcpConnection(EventLoop* loop, int fd, std::string name, KvStore* store)
    : loop_(loop), fd_(fd), name_(std::move(name)), store_(store),
      channel_(std::make_unique<Channel>(loop, fd)) {
    channel_->set_read_callback([this] { handle_read(); });
    channel_->set_write_callback([this] { handle_write(); });
    channel_->set_close_callback([this] { handle_close(); });
    channel_->set_error_callback([this] { handle_error(); });
}

TcpConnection::~TcpConnection() {
    ::close(fd_);
}

void TcpConnection::connect_established() {
    channel_->enable_reading();
    LOG_INFO("connection established: ", name_);
}

void TcpConnection::send(const std::string& msg) {
    loop_->run_in_loop([self = shared_from_this(), msg] { self->send_in_loop(msg); });
}

void TcpConnection::send_in_loop(const std::string& msg) {
    ssize_t nwrote = 0;
    size_t remaining = msg.size();
    // 若输出缓冲为空，尝试直接 write
    if (!writing_ && output_.readable_bytes() == 0) {
        nwrote = ::write(fd_, msg.data(), msg.size());
        if (nwrote >= 0) {
            remaining = msg.size() - static_cast<size_t>(nwrote);
            if (remaining == 0) return;
        } else if (errno != EWOULDBLOCK && errno != EAGAIN) {
            LOG_ERROR("write error on ", name_, ": ", std::strerror(errno));
            return;
        } else {
            nwrote = 0;
        }
    }
    if (remaining > 0) {
        output_.append(msg.data() + nwrote, remaining);
        if (!writing_) {
            writing_ = true;
            channel_->enable_writing();
        }
    }
}

void TcpConnection::handle_read() {
    int saved_errno = 0;
    ssize_t n = input_.read_fd(fd_, &saved_errno);
    if (n > 0) {
        process_commands();
    } else if (n == 0) {
        handle_close();
    } else {
        if (saved_errno != EAGAIN && saved_errno != EWOULDBLOCK) {
            LOG_ERROR("read error on ", name_, ": ", std::strerror(saved_errno));
            handle_close();
        }
    }
}

void TcpConnection::process_commands() {
    for (;;) {
        Command cmd;
        std::string err;
        ParseResult r = parser_.try_parse(input_, &cmd, &err);
        if (r == ParseResult::NeedMore) break;
        if (r == ParseResult::Error) {
            send(ProtocolParser::encode_error(err));
            // 协议错误：可选择关闭连接；教学版先继续
            continue;
        }
        send(dispatch(cmd));
    }
}

std::string TcpConnection::dispatch(const Command& cmd) {
    if (cmd.name == "PING") {
        return ProtocolParser::encode_simple("PONG");
    }
    if (cmd.name == "SET") {
        if (cmd.args.size() < 2) {
            return ProtocolParser::encode_error("wrong number of arguments for SET");
        }
        // TODO(学生): 支持 SET key value EX seconds
        store_->set(cmd.args[0], cmd.args[1]);
        return ProtocolParser::encode_simple("OK");
    }
    if (cmd.name == "GET") {
        if (cmd.args.size() != 1) {
            return ProtocolParser::encode_error("wrong number of arguments for GET");
        }
        auto v = store_->get(cmd.args[0]);
        if (!v) return ProtocolParser::encode_null_bulk();
        return ProtocolParser::encode_bulk(*v);
    }
    if (cmd.name == "DEL") {
        if (cmd.args.size() != 1) {
            return ProtocolParser::encode_error("wrong number of arguments for DEL");
        }
        bool ok = store_->del(cmd.args[0]);
        return ProtocolParser::encode_integer(ok ? 1 : 0);
    }
    // TODO(学生): EXPIRE / TTL / EXISTS / KEYS ...
    return ProtocolParser::encode_error("unknown command '" + cmd.name + "'");
}

void TcpConnection::handle_write() {
    if (!writing_) return;
    ssize_t n = ::write(fd_, output_.peek(), output_.readable_bytes());
    if (n > 0) {
        output_.retrieve(static_cast<size_t>(n));
        if (output_.readable_bytes() == 0) {
            channel_->disable_writing();
            writing_ = false;
        }
    } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        LOG_ERROR("handle_write error on ", name_);
        handle_close();
    }
}

void TcpConnection::handle_close() {
    channel_->disable_all();
    channel_->remove();
    Ptr guard = shared_from_this();
    if (close_cb_) close_cb_(guard);
    LOG_INFO("connection closed: ", name_);
}

void TcpConnection::handle_error() {
    LOG_ERROR("connection error: ", name_);
    handle_close();
}

void TcpConnection::shutdown() {
    loop_->run_in_loop([this] {
        ::shutdown(fd_, SHUT_WR);
    });
}

}  // namespace kv
