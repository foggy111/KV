#include "async_logger.hpp"
#include "event_loop.hpp"
#include "kv_store.hpp"
#include "tcp_server.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {
std::atomic<kv::EventLoop*> g_loop{nullptr};

// 主动过期：每 100ms 从游标起最多扫 64 个键。
// 键很多时，一个过期键最坏要等约 (键数 / 64) 个周期才会被主动删掉。
// GET / TTL / DEL / EXPIRE 仍会惰性删除，读路径不会返回过期值。
constexpr int kActiveExpirePeriodMs = 100;
constexpr size_t kActiveExpireSample = 64;

void on_signal(int) {
    kv::EventLoop* loop = g_loop.load();
    if (loop) loop->quit();
}

void usage() {
    std::cerr
        << "用法: kv_server [--port PORT] [--quiet]\n"
        << "      kv_server [PORT]\n"
        << "默认端口 6379。--port 0 由系统分配端口，并在日志里打印实际端口。\n"
        << "--quiet 关闭每条请求的路径日志（压测用）。连接和 TTL 删除日志仍保留。\n"
        << "--io-threads 未实现：Multi-Reactor 还没有可运行版本。\n";
}
}  // namespace

int main(int argc, char* argv[]) {
    uint16_t port = 6379;
    bool quiet = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            usage();
            return 0;
        }
        if (arg == "--quiet") {
            quiet = true;
            continue;
        }
        if (arg == "--io-threads") {
            std::cerr << "Multi-Reactor (--io-threads) 尚未实现，当前只有单 Reactor。\n";
            return 2;
        }
        if (arg == "--port") {
            if (i + 1 >= argc) {
                usage();
                return 2;
            }
            arg = argv[++i];
        } else if (!arg.empty() && arg[0] == '-') {
            usage();
            return 2;
        }
        int parsed = std::atoi(arg.c_str());
        if (parsed < 0 || parsed > 65535) {
            std::cerr << "非法端口\n";
            return 2;
        }
        port = static_cast<uint16_t>(parsed);
    }

    // 客户端提前断开时，write 不应把进程打死。错误在读写回调里处理。
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    if (quiet) kv::AsyncLogger::instance().set_request_log(false);

    LOG_INFO("单 Reactor：命令在事件循环线程内执行");
    LOG_INFO("Multi-Reactor 未实现");
    LOG_INFO("主动过期扫描 周期毫秒=", kActiveExpirePeriodMs,
             " 每轮最多键数=", kActiveExpireSample);
    if (quiet) LOG_INFO("请求路径日志已关闭");

    kv::EventLoop loop;
    g_loop.store(&loop);

    kv::KvStore store;
    loop.run_every(std::chrono::milliseconds(kActiveExpirePeriodMs), [&store] {
        store.active_expire(kActiveExpireSample);
    });

    kv::TcpServer server(&loop, port, &store);
    server.start();

    LOG_INFO("Ctrl+C 触发优雅退出");
    loop.loop();

    g_loop.store(nullptr);
    LOG_INFO("服务器已退出");
    return 0;
}
