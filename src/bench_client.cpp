// 闭环压测客户端：每条连接最多一个未完成请求。
// 指标定义印在输出里。失败时退出码为 2，不打印伪造的 0 QPS 当成功。

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct CpuSample {
    long ticks{-1};
};

CpuSample read_proc_ticks(int pid) {
    std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
    if (!in) return {};
    std::string text;
    std::getline(in, text);
    auto rparen = text.rfind(')');
    if (rparen == std::string::npos) return {};
    std::istringstream iss(text.substr(rparen + 2));
    std::string skip;
    // /proc/pid/stat 在 comm 之后：state … cmajflt 共 11 个字段，然后是 utime、stime。
    for (int i = 0; i < 11; ++i) iss >> skip;
    long utime = 0;
    long stime = 0;
    iss >> utime >> stime;
    if (!iss) return {};
    CpuSample sample;
    sample.ticks = utime + stime;
    return sample;
}

std::string bulk(const std::string& s) {
    return "$" + std::to_string(s.size()) + "\r\n" + s + "\r\n";
}

std::string cmd_set(const std::string& key, const std::string& value) {
    return "*3\r\n" + bulk("SET") + bulk(key) + bulk(value);
}

std::string cmd_get(const std::string& key) {
    return "*2\r\n" + bulk("GET") + bulk(key);
}

enum class Pop { Need, Ok, Bad };

Pop pop_resp(std::string* buf, bool* is_err) {
    if (buf->empty()) return Pop::Need;
    auto crlf = buf->find("\r\n");
    if (crlf == std::string::npos) return Pop::Need;
    char type = (*buf)[0];
    if (type == '+' || type == '-' || type == ':') {
        *is_err = (type == '-');
        buf->erase(0, crlf + 2);
        return Pop::Ok;
    }
    if (type != '$') return Pop::Bad;
    long len = 0;
    try {
        len = std::stol(buf->substr(1, crlf));
    } catch (...) {
        return Pop::Bad;
    }
    if (len < 0) {
        *is_err = false;
        buf->erase(0, crlf + 2);
        return Pop::Ok;
    }
    size_t need = crlf + 2 + static_cast<size_t>(len) + 2;
    if (buf->size() < need) return Pop::Need;
    if (buf->compare(need - 2, 2, "\r\n") != 0) return Pop::Bad;
    *is_err = false;
    buf->erase(0, need);
    return Pop::Ok;
}

uint64_t percentile_us(std::vector<uint64_t> samples, double p) {
    if (samples.empty()) return 0;
    std::sort(samples.begin(), samples.end());
    // nearest rank：ceil(p * n) - 1。p=0.99、n=100 时下标为 98。
    double rank = p * static_cast<double>(samples.size());
    size_t ceil_rank = static_cast<size_t>(rank);
    if (static_cast<double>(ceil_rank) < rank) ++ceil_rank;
    if (ceil_rank == 0) ceil_rank = 1;
    size_t index = ceil_rank - 1;
    if (index >= samples.size()) index = samples.size() - 1;
    return samples[index];
}

struct Conn {
    int fd{-1};
    bool alive{true};
    bool waiting{false};
    uint64_t seq{0};
    std::string to_write;
    std::string inbuf;
    std::chrono::steady_clock::time_point started{};
};

int connect_one(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

bool flush_conn(Conn* conn) {
    while (!conn->to_write.empty()) {
        ssize_t n = ::send(conn->fd, conn->to_write.data(), conn->to_write.size(), MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
            return false;
        }
        if (n == 0) return false;
        conn->to_write.erase(0, static_cast<size_t>(n));
    }
    return true;
}

void usage() {
    std::cerr
        << "用法: kv_bench --port PORT [--connections N] [--duration SEC]\n"
        << "              [--warmup SEC] [--keys N] [--server-pid PID]\n"
        << "闭环、每连接窗口为 1。SET 与 GET 交替。GET 未命中算成功，-ERR 算失败。\n";
}

}  // namespace

int main(int argc, char** argv) {
    uint16_t port = 0;
    int connections = 4;
    double duration = 5.0;
    double warmup = 1.0;
    int keys = 128;
    int server_pid = 0;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "缺少参数 " << name << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--help" || arg == "-h") {
            usage();
            return 0;
        } else if (arg == "--port") {
            int p = std::atoi(need("--port"));
            if (p <= 0 || p > 65535) {
                std::cerr << "非法端口\n";
                return 2;
            }
            port = static_cast<uint16_t>(p);
        } else if (arg == "--connections") {
            connections = std::atoi(need("--connections"));
        } else if (arg == "--duration") {
            duration = std::atof(need("--duration"));
        } else if (arg == "--warmup") {
            warmup = std::atof(need("--warmup"));
        } else if (arg == "--keys") {
            keys = std::atoi(need("--keys"));
        } else if (arg == "--server-pid") {
            server_pid = std::atoi(need("--server-pid"));
        } else {
            usage();
            return 2;
        }
    }
    if (port == 0 || connections <= 0 || connections > 256 || duration <= 0 || warmup < 0 ||
        keys <= 0) {
        usage();
        return 2;
    }

    std::signal(SIGPIPE, SIG_IGN);

    const std::string value(16, 'v');
    std::vector<Conn> conns(static_cast<size_t>(connections));
    for (int i = 0; i < connections; ++i) {
        conns[static_cast<size_t>(i)].fd = connect_one(port);
        if (conns[static_cast<size_t>(i)].fd < 0) {
            std::cerr << "连接失败\n";
            return 2;
        }
    }

    using Clock = std::chrono::steady_clock;
    auto t0 = Clock::now();
    auto measure_start = t0 + std::chrono::duration_cast<Clock::duration>(
                                  std::chrono::duration<double>(warmup));
    auto measure_end = measure_start + std::chrono::duration_cast<Clock::duration>(
                                            std::chrono::duration<double>(duration));

    uint64_t ok = 0;
    uint64_t errors = 0;
    std::vector<uint64_t> samples;
    samples.reserve(100000);
    bool cpu_started = false;
    CpuSample server_cpu0;
    CpuSample self_cpu0;
    Clock::time_point cpu_t0{};
    int self_pid = static_cast<int>(::getpid());

    auto note_sample = [&](const Conn& conn, bool is_err) {
        if (conn.started < measure_start || conn.started >= measure_end) return;
        if (is_err) {
            ++errors;
            return;
        }
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - conn.started)
                      .count();
        if (us < 0) us = 0;
        samples.push_back(static_cast<uint64_t>(us));
        ++ok;
    };

    auto issue = [&](Conn* conn) {
        std::string key = "k" + std::to_string(conn->seq % static_cast<uint64_t>(keys));
        if (conn->seq % 2 == 0) conn->to_write = cmd_set(key, value);
        else conn->to_write = cmd_get(key);
        ++conn->seq;
        conn->waiting = true;
        conn->started = Clock::now();
        if (!flush_conn(conn)) {
            note_sample(*conn, true);
            conn->alive = false;
        }
    };

    for (auto& conn : conns) issue(&conn);

    while (Clock::now() < measure_end) {
        auto now = Clock::now();
        if (!cpu_started && now >= measure_start) {
            if (server_pid > 0) server_cpu0 = read_proc_ticks(server_pid);
            self_cpu0 = read_proc_ticks(self_pid);
            cpu_t0 = Clock::now();
            cpu_started = true;
        }

        std::vector<pollfd> pfds(conns.size());
        bool any = false;
        for (size_t i = 0; i < conns.size(); ++i) {
            pfds[i].fd = conns[i].alive ? conns[i].fd : -1;
            pfds[i].events = POLLIN;
            if (conns[i].alive && !conns[i].to_write.empty()) pfds[i].events |= POLLOUT;
            pfds[i].revents = 0;
            if (conns[i].alive) any = true;
        }
        if (!any) break;
        int pr = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), 50);
        if (pr < 0) {
            if (errno == EINTR) continue;
            std::cerr << "poll 失败\n";
            return 2;
        }

        for (size_t i = 0; i < conns.size(); ++i) {
            Conn& conn = conns[i];
            if (!conn.alive) continue;
            if (pfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                if (conn.waiting) note_sample(conn, true);
                conn.alive = false;
                continue;
            }
            if (pfds[i].revents & POLLOUT) {
                if (!flush_conn(&conn)) {
                    if (conn.waiting) note_sample(conn, true);
                    conn.alive = false;
                    continue;
                }
            }
            if (pfds[i].revents & POLLIN) {
                char tmp[4096];
                ssize_t n = ::recv(conn.fd, tmp, sizeof(tmp), 0);
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    if (conn.waiting) note_sample(conn, true);
                    conn.alive = false;
                    continue;
                }
                if (n == 0) {
                    if (conn.waiting) note_sample(conn, true);
                    conn.alive = false;
                    continue;
                }
                conn.inbuf.append(tmp, static_cast<size_t>(n));
                for (;;) {
                    bool is_err = false;
                    Pop pop = pop_resp(&conn.inbuf, &is_err);
                    if (pop == Pop::Need) break;
                    if (pop == Pop::Bad) {
                        note_sample(conn, true);
                        conn.alive = false;
                        break;
                    }
                    note_sample(conn, is_err);
                    conn.waiting = false;
                    if (Clock::now() < measure_end) issue(&conn);
                }
            }
        }
    }

    auto ended = Clock::now();
    CpuSample server_cpu1;
    CpuSample self_cpu1;
    Clock::time_point cpu_t1 = ended;
    if (cpu_started) {
        if (server_pid > 0) server_cpu1 = read_proc_ticks(server_pid);
        self_cpu1 = read_proc_ticks(self_pid);
        cpu_t1 = Clock::now();
    }
    double cpu_window = cpu_started ? std::chrono::duration<double>(cpu_t1 - cpu_t0).count() : 0;

    for (auto& conn : conns) {
        if (conn.fd >= 0) ::close(conn.fd);
    }

    double elapsed = std::chrono::duration<double>(ended - measure_start).count();
    if (elapsed < 0) elapsed = 0;
    long clk = ::sysconf(_SC_CLK_TCK);

    auto cpu_percent = [&](const CpuSample& a, const CpuSample& b) -> std::string {
        if (a.ticks < 0 || b.ticks < 0 || clk <= 0 || cpu_window <= 0) return "NA";
        double seconds = static_cast<double>(b.ticks - a.ticks) / static_cast<double>(clk);
        std::ostringstream oss;
        oss.setf(std::ios::fixed);
        oss.precision(2);
        oss << (seconds / cpu_window * 100.0);
        return oss.str();
    };
    auto cpu_seconds = [&](const CpuSample& a, const CpuSample& b) -> std::string {
        if (a.ticks < 0 || b.ticks < 0 || clk <= 0) return "NA";
        std::ostringstream oss;
        oss.setf(std::ios::fixed);
        oss.precision(4);
        oss << (static_cast<double>(b.ticks - a.ticks) / static_cast<double>(clk));
        return oss.str();
    };

    std::cout.setf(std::ios::fixed);
    std::cout.precision(4);
    std::cout << "tool=kv_bench\n";
    std::cout << "connections=" << connections << "\n";
    std::cout << "pipeline_depth=1\n";
    std::cout << "command_mix=alternating SET and GET\n";
    std::cout << "value_bytes=" << value.size() << "\n";
    std::cout << "keys=" << keys << "\n";
    std::cout << "warmup_s=" << warmup << "\n";
    std::cout << "requested_duration_s=" << duration << "\n";
    std::cout << "elapsed_s=" << elapsed << "\n";
    std::cout << "ok=" << ok << "\n";
    std::cout << "errors=" << errors << "\n";
    std::cout << "samples=" << samples.size() << "\n";
    if (ok + errors == 0 || elapsed <= 0) {
        std::cout << "qps=NA\n";
        std::cout << "error_rate=NA\n";
    } else {
        std::cout << "qps=" << (static_cast<double>(ok) / elapsed) << "\n";
        std::cout.precision(6);
        std::cout << "error_rate="
                  << (static_cast<double>(errors) / static_cast<double>(ok + errors)) << "\n";
        std::cout.precision(4);
    }
    if (samples.empty()) {
        std::cout << "rtt_us_min=NA\n";
        std::cout << "rtt_us_p50=NA\n";
        std::cout << "rtt_us_p99=NA\n";
        std::cout << "rtt_us_max=NA\n";
    } else {
        auto min_us = *std::min_element(samples.begin(), samples.end());
        auto max_us = *std::max_element(samples.begin(), samples.end());
        std::cout << "rtt_us_min=" << min_us << "\n";
        std::cout << "rtt_us_p50=" << percentile_us(samples, 0.50) << "\n";
        std::cout << "rtt_us_p99=" << percentile_us(samples, 0.99) << "\n";
        std::cout << "rtt_us_max=" << max_us << "\n";
    }
    std::cout << "cpu_window_s=" << cpu_window << "\n";
    std::cout << "server_cpu_percent_one_core=" << cpu_percent(server_cpu0, server_cpu1) << "\n";
    std::cout << "server_cpu_seconds=" << cpu_seconds(server_cpu0, server_cpu1) << "\n";
    std::cout << "client_cpu_percent_one_core=" << cpu_percent(self_cpu0, self_cpu1) << "\n";
    std::cout << "client_cpu_seconds=" << cpu_seconds(self_cpu0, self_cpu1) << "\n";
    std::cout << "rtt_definition=steady_clock from request issue (first send attempt) until one full response is parsed\n";
    std::cout << "p99_method=nearest_rank ceil(p*n)-1 ; not interpolated\n";
    std::cout << "qps_method=ok / elapsed_s ; elapsed is measured window, warmup excluded\n";
    std::cout << "excluded=warmup responses and requests still in flight at window end\n";
    std::cout << "error_rate_method=errors/(ok+errors); GET miss ($-1) is success; -ERR and transport failure are errors\n";
    std::cout << "cpu_method=delta(/proc/<pid>/stat utime+stime)/CLK_TCK/cpu_window_s*100 ; 100 means one logical CPU saturated ; window is the sample interval inside the measure window, not an external profiler\n";
    std::cout << "server_pid=" << server_pid << "\n";
    std::cout << "clk_tck=" << clk << "\n";

    if (ok == 0) return 2;
    return 0;
}
