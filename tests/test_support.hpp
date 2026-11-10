#pragma once

// 集成测试用的循环线程服务器和阻塞 RESP 客户端。只给测试编译。

#include "event_loop.hpp"
#include "kv_store.hpp"
#include "tcp_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace kvtest {

class TestServer {
public:
    TestServer() {
        std::promise<uint16_t> port_promise;
        auto port_future = port_promise.get_future();
        thread_ = std::thread([this, promise = std::move(port_promise)]() mutable {
            kv::EventLoop loop;
            loop_.store(&loop);
            kv::TcpServer server(&loop, 0, &store_);
            server.start();
            loop.run_every(std::chrono::milliseconds(50), [this] { store_.active_expire(128); });
            promise.set_value(server.listen_port());
            loop.loop();
            loop_.store(nullptr);
        });
        port_ = port_future.get();
    }

    ~TestServer() { stop(); }

    void stop() {
        if (!thread_.joinable()) return;
        if (kv::EventLoop* loop = loop_.load()) loop->quit();
        thread_.join();
    }

    uint16_t port() const { return port_; }

private:
    kv::KvStore store_;
    std::thread thread_;
    std::atomic<kv::EventLoop*> loop_{nullptr};
    uint16_t port_{0};
};

class Client {
public:
    explicit Client(uint16_t port) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        int saved = 0;
        for (int attempt = 0; attempt < 50; ++attempt) {
            fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
            if (fd_ < 0) throw std::runtime_error("socket");
            timeval tv{};
            tv.tv_sec = 2;
            ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) return;
            saved = errno;
            ::close(fd_);
            fd_ = -1;
            if (saved != ECONNREFUSED && saved != EINTR) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        throw std::runtime_error(std::string("connect: ") + std::strerror(saved));
    }

    ~Client() {
        if (fd_ >= 0) ::close(fd_);
    }

    std::string command(std::vector<std::string> args) {
        write_raw(encode(args));
        return read_one();
    }

    void write_raw(const std::string& data) {
        size_t off = 0;
        while (off < data.size()) {
            ssize_t n = ::write(fd_, data.data() + off, data.size() - off);
            if (n < 0) {
                if (errno == EINTR) continue;
                throw std::runtime_error(std::string("write: ") + std::strerror(errno));
            }
            off += static_cast<size_t>(n);
        }
    }

    std::string read_one() {
        std::string out;
        for (;;) {
            if (try_pop(&out)) return out;
            char tmp[512];
            ssize_t n = ::read(fd_, tmp, sizeof(tmp));
            if (n < 0) {
                if (errno == EINTR) continue;
                throw std::runtime_error(std::string("read: ") + std::strerror(errno));
            }
            if (n == 0) throw std::runtime_error("eof before a full response");
            buf_.append(tmp, static_cast<size_t>(n));
        }
    }

    // 读到对端关闭返回 true。若还有数据返回 false。
    bool read_eof() {
        char tmp[32];
        for (;;) {
            ssize_t n = ::read(fd_, tmp, sizeof(tmp));
            if (n < 0) {
                if (errno == EINTR) continue;
                throw std::runtime_error(std::string("read: ") + std::strerror(errno));
            }
            return n == 0;
        }
    }

    int fd() const { return fd_; }

private:
    static std::string encode(const std::vector<std::string>& args) {
        std::string out = "*" + std::to_string(args.size()) + "\r\n";
        for (const auto& arg : args) {
            out += "$" + std::to_string(arg.size()) + "\r\n" + arg + "\r\n";
        }
        return out;
    }

    bool try_pop(std::string* out) {
        if (buf_.empty()) return false;
        auto crlf = buf_.find("\r\n");
        if (crlf == std::string::npos) return false;
        char type = buf_[0];
        if (type == '+' || type == '-' || type == ':') {
            *out = buf_.substr(0, crlf + 2);
            buf_.erase(0, crlf + 2);
            return true;
        }
        if (type != '$') throw std::runtime_error("unexpected response type");
        long len = std::stol(buf_.substr(1, crlf));
        if (len < 0) {
            *out = buf_.substr(0, crlf + 2);
            buf_.erase(0, crlf + 2);
            return true;
        }
        size_t need = crlf + 2 + static_cast<size_t>(len) + 2;
        if (buf_.size() < need) return false;
        *out = buf_.substr(0, need);
        buf_.erase(0, need);
        return true;
    }

    int fd_{-1};
    std::string buf_;
};

}  // namespace kvtest
