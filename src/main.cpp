#include "async_logger.hpp"
#include "event_loop.hpp"
#include "kv_store.hpp"
#include "tcp_server.hpp"
#include "thread_pool.hpp"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {
std::atomic<kv::EventLoop*> g_loop{nullptr};

void on_signal(int) {
    kv::EventLoop* loop = g_loop.load();
    if (loop) loop->quit();
}
}  // namespace

int main(int argc, char* argv[]) {
    uint16_t port = 6379;
    if (argc >= 2) {
        port = static_cast<uint16_t>(std::atoi(argv[1]));
        if (port == 0) port = 6379;
    }

    // 若 6379 被占用，可: ./kv_server 6380
    LOG_INFO("Reactor KV educational starter starting on port ", port);

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    kv::EventLoop loop;
    g_loop.store(&loop);

    kv::KvStore store;
    // 线程池示例：当前业务在 IO 线程同步执行；
    // TODO(学生): 把慢操作丢进 pool，结果通过 run_in_loop 回写
    kv::ThreadPool pool(2);
    (void)pool;

    kv::TcpServer server(&loop, port, &store);
    server.start();

    LOG_INFO("Press Ctrl+C for graceful shutdown");
    loop.loop();

    g_loop.store(nullptr);
    LOG_INFO("bye");
    return 0;
}
