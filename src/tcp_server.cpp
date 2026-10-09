#include "tcp_server.hpp"
#include "async_logger.hpp"
#include "channel.hpp"
#include "event_loop.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace kv {

int TcpServer::create_nonblocking_listen(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) throw_sys_error("socket");

    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    // TODO(学生): 按需打开 SO_REUSEPORT

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        throw_sys_error("bind");
    }
    if (::listen(fd, SOMAXCONN) < 0) {
        ::close(fd);
        throw_sys_error("listen");
    }
    return fd;
}

TcpServer::TcpServer(EventLoop* loop, uint16_t port, KvStore* store)
    : loop_(loop),
      port_(port),
      store_(store),
      listen_fd_(create_nonblocking_listen(port)),
      accept_channel_(std::make_unique<Channel>(loop, listen_fd_)) {}

TcpServer::~TcpServer() {
    accept_channel_->disable_all();
    accept_channel_->remove();
    ::close(listen_fd_);
}

void TcpServer::start() {
    if (started_) return;
    started_ = true;
    accept_channel_->set_read_callback([this] { handle_accept(); });
    accept_channel_->enable_reading();
    LOG_INFO("TcpServer listening on port ", port_);
}

void TcpServer::handle_accept() {
    for (;;) {
        sockaddr_in peer{};
        socklen_t len = sizeof(peer);
        int connfd = ::accept4(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &len,
                               SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (connfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            LOG_ERROR("accept4: ", std::strerror(errno));
            break;
        }

        char ip[INET_ADDRSTRLEN];
        ::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        std::string name = std::string(ip) + ":" + std::to_string(ntohs(peer.sin_port)) +
                           "#" + std::to_string(next_conn_id_++);

        auto conn = std::make_shared<TcpConnection>(loop_, connfd, name, store_);
        connections_[name] = conn;
        conn->set_close_callback([this](const TcpConnection::Ptr& c) {
            remove_connection(c);
        });
        conn->connect_established();
    }
}

void TcpServer::remove_connection(const TcpConnection::Ptr& conn) {
    loop_->run_in_loop([this, conn] {
        connections_.erase(conn->name());
    });
}

}  // namespace kv
