#include "tcp_connection.hpp"
#include "async_logger.hpp"
#include "event_loop.hpp"
#include "kv_store.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <optional>
#include <unistd.h>
#include <sys/socket.h>

namespace kv {

namespace {

std::string to_upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string preview_resp(const std::string& s) {
    std::string out;
    out.reserve(std::min<size_t>(s.size(), 48));
    for (char c : s) {
        if (out.size() >= 40) {
            out += "...";
            break;
        }
        if (c == '\r') out += "\\r";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

std::string short_text(const std::string& s) {
    if (s.size() <= 32) return s;
    return s.substr(0, 32) + "...(" + std::to_string(s.size()) + "B)";
}

// 秒数必须是十进制整数。allow_zero 只给 EXPIRE 0（立即删除）使用。
bool parse_seconds(const std::string& s, long long* out, bool allow_zero, std::string* err) {
    if (s.empty()) {
        *err = "expire seconds must be an integer";
        return false;
    }
    for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            *err = "expire seconds must be an integer";
            return false;
        }
    }
    try {
        long long v = std::stoll(s);
        constexpr long long kMax = 100LL * 365 * 24 * 3600;
        if (v < 0 || v > kMax || (v == 0 && !allow_zero)) {
            *err = "expire seconds out of range";
            return false;
        }
        *out = v;
        return true;
    } catch (...) {
        *err = "expire seconds out of range";
        return false;
    }
}

}  // namespace

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
    // 必须在注册读事件之前 tie，保证 handle_event 整个过程连接还活着。
    channel_->tie(shared_from_this());
    channel_->enable_reading();
    LOG_INFO("连接建立 ", name_);
}

void TcpConnection::send(const std::string& msg) {
    loop_->run_in_loop([self = shared_from_this(), msg] { self->send_in_loop(msg); });
}

void TcpConnection::send_in_loop(const std::string& msg) {
    if (closed_) return;
    ssize_t nwrote = 0;
    size_t remaining = msg.size();
    // 若输出缓冲为空，尝试直接 write
    if (!writing_ && output_.readable_bytes() == 0) {
        nwrote = ::write(fd_, msg.data(), msg.size());
        if (nwrote >= 0) {
            remaining = msg.size() - static_cast<size_t>(nwrote);
            if (remaining == 0) return;
        } else if (errno != EWOULDBLOCK && errno != EAGAIN) {
            LOG_ERROR("写失败 连接=", name_, " 原因=", std::strerror(errno));
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
    auto guard = shared_from_this();
    if (closed_) return;
    int saved_errno = 0;
    ssize_t n = input_.read_fd(fd_, &saved_errno);
    if (n > 0) {
        LOG_REQ("路径 连接=", name_, " 阶段=读取 字节=", n,
                " 缓冲=", input_.readable_bytes());
        process_commands();
    } else if (n == 0) {
        handle_close();
    } else {
        if (saved_errno != EAGAIN && saved_errno != EWOULDBLOCK) {
            LOG_ERROR("读失败 连接=", name_, " 原因=", std::strerror(saved_errno));
            handle_close();
        }
    }
}

void TcpConnection::process_commands() {
    for (;;) {
        if (closed_) return;
        Command cmd;
        std::string err;
        ParseResult r = parser_.try_parse(input_, &cmd, &err);
        if (r == ParseResult::NeedMore) return;
        if (r == ParseResult::Error) {
            // 解析失败时字节还在缓冲里。先回错误再关连接，避免死循环重试同一段数据。
            LOG_WARN("协议错误 连接=", name_, " 原因=", err, "，关闭连接");
            send(ProtocolParser::encode_error(err.empty() ? "protocol error" : err));
            handle_close();
            return;
        }
        LOG_REQ("路径 连接=", name_, " 阶段=分发 命令=", cmd.name,
                " 参数个数=", cmd.args.size());
        std::string resp = dispatch(cmd);
        LOG_REQ("路径 连接=", name_, " 阶段=响应 命令=", cmd.name,
                " 预览=", preview_resp(resp));
        send(resp);
    }
}

std::string TcpConnection::dispatch(const Command& cmd) {
    if (cmd.name == "PING") {
        if (cmd.args.empty()) return ProtocolParser::encode_simple("PONG");
        if (cmd.args.size() == 1) return ProtocolParser::encode_bulk(cmd.args[0]);
        return ProtocolParser::encode_error("wrong number of arguments for PING");
    }
    if (cmd.name == "SET") {
        if (cmd.args.size() != 2 && cmd.args.size() != 4) {
            return ProtocolParser::encode_error("wrong number of arguments for SET");
        }
        std::optional<std::chrono::milliseconds> ttl;
        if (cmd.args.size() == 4) {
            if (to_upper(cmd.args[2]) != "EX") {
                return ProtocolParser::encode_error("unsupported SET option");
            }
            long long sec = 0;
            std::string err;
            if (!parse_seconds(cmd.args[3], &sec, /*allow_zero=*/false, &err)) {
                return ProtocolParser::encode_error(err);
            }
            ttl = std::chrono::milliseconds(sec * 1000);
        }
        store_->set(cmd.args[0], cmd.args[1], ttl);
        LOG_REQ("路径 连接=", name_, " 阶段=存储 操作=SET 键=", short_text(cmd.args[0]),
                " 值=", short_text(cmd.args[1]),
                " ttl_ms=", ttl ? std::to_string(ttl->count()) : "none");
        return ProtocolParser::encode_simple("OK");
    }
    if (cmd.name == "GET") {
        if (cmd.args.size() != 1) {
            return ProtocolParser::encode_error("wrong number of arguments for GET");
        }
        auto v = store_->get(cmd.args[0]);
        LOG_REQ("路径 连接=", name_, " 阶段=存储 操作=GET 键=", short_text(cmd.args[0]),
                " 命中=", v ? "yes" : "no",
                " 值字节=", v ? std::to_string(v->size()) : "0");
        if (!v) return ProtocolParser::encode_null_bulk();
        return ProtocolParser::encode_bulk(*v);
    }
    if (cmd.name == "DEL") {
        if (cmd.args.size() != 1) {
            return ProtocolParser::encode_error("wrong number of arguments for DEL");
        }
        bool ok = store_->del(cmd.args[0]);
        LOG_REQ("路径 连接=", name_, " 阶段=存储 操作=DEL 键=", short_text(cmd.args[0]),
                " 删除=", ok ? "yes" : "no");
        return ProtocolParser::encode_integer(ok ? 1 : 0);
    }
    if (cmd.name == "EXPIRE") {
        if (cmd.args.size() != 2) {
            return ProtocolParser::encode_error("wrong number of arguments for EXPIRE");
        }
        long long sec = 0;
        std::string err;
        if (!parse_seconds(cmd.args[1], &sec, /*allow_zero=*/true, &err)) {
            return ProtocolParser::encode_error(err);
        }
        bool ok = store_->expire(cmd.args[0], std::chrono::milliseconds(sec * 1000));
        LOG_REQ("路径 连接=", name_, " 阶段=存储 操作=EXPIRE 键=", short_text(cmd.args[0]),
                " 秒=", sec, " 结果=", ok ? 1 : 0);
        return ProtocolParser::encode_integer(ok ? 1 : 0);
    }
    if (cmd.name == "TTL") {
        if (cmd.args.size() != 1) {
            return ProtocolParser::encode_error("wrong number of arguments for TTL");
        }
        long long sec = store_->ttl_seconds(cmd.args[0]);
        LOG_REQ("路径 连接=", name_, " 阶段=存储 操作=TTL 键=", short_text(cmd.args[0]),
                " 秒=", sec);
        return ProtocolParser::encode_integer(sec);
    }
    if (cmd.name == "DBSIZE") {
        if (!cmd.args.empty()) {
            return ProtocolParser::encode_error("wrong number of arguments for DBSIZE");
        }
        // 含尚未扫描掉的过期键。演示主动删除时，要在 GET 之前看它。
        return ProtocolParser::encode_integer(static_cast<long long>(store_->size()));
    }
    return ProtocolParser::encode_error("unknown command '" + cmd.name + "'");
}

void TcpConnection::handle_write() {
    auto guard = shared_from_this();
    if (!writing_ || closed_) return;
    ssize_t n = ::write(fd_, output_.peek(), output_.readable_bytes());
    if (n > 0) {
        output_.retrieve(static_cast<size_t>(n));
        if (output_.readable_bytes() == 0) {
            channel_->disable_writing();
            writing_ = false;
        }
    } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        LOG_ERROR("写回调失败 连接=", name_);
        handle_close();
    }
}

void TcpConnection::handle_close() {
    if (closed_) return;
    closed_ = true;
    if (output_.readable_bytes() > 0) {
        // 短错误响应尽量先写出去。写不完就放弃，教学服务器不在关闭路径上重试。
        ssize_t n = ::write(fd_, output_.peek(), output_.readable_bytes());
        if (n > 0) output_.retrieve(static_cast<size_t>(n));
    }
    channel_->disable_all();
    channel_->remove();
    Ptr guard = shared_from_this();
    if (close_cb_) close_cb_(guard);
    LOG_INFO("连接关闭 ", name_);
}

void TcpConnection::handle_error() {
    LOG_ERROR("连接错误 ", name_);
    handle_close();
}

void TcpConnection::shutdown() {
    loop_->run_in_loop([self = shared_from_this()] {
        if (!self->closed_) ::shutdown(self->fd_, SHUT_WR);
    });
}

}  // namespace kv
