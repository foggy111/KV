# Reactor KV（教育向单机 KV 服务）

寒假实习向：**基于 Reactor 的单机 Key-Value 服务** C++17 教学骨架。

本仓库提供「能编译、能监听、能跑通 SET/GET」的最小可运行路径，并在关键位置留下 `TODO(学生)`，供你自己补全 TTL 主动过期、完整 RESP、异步日志、Benchmark 等。

> **重要**：可参考 muduo / Redis / 开源 Reactor 项目的设计思路，**请勿整仓抄袭后当作自己的实习项目提交**。面试官能看出来。

## 环境

- Linux（依赖 `epoll` / `eventfd` / `accept4`）
- CMake ≥ 3.14
- 支持 C++17 的 GCC / Clang

## 构建与运行

```bash
cmake -B build
cmake --build build -j

# 默认监听 6379；被占用时可换端口
./build/kv_server
./build/kv_server 6380

# 协议单测（独立 assert，无需 GoogleTest）
./build/test_protocol
# 或
ctest --test-dir build --output-on-failure
```

### 手工试玩

```bash
# 行协议
printf 'PING\r\nSET hello world\r\nGET hello\r\n' | nc 127.0.0.1 6379

# 也可用 redis-cli（RESP）
redis-cli -p 6379 PING
redis-cli -p 6379 SET foo bar
redis-cli -p 6379 GET foo
```

优雅退出：`Ctrl+C`（SIGINT）→ `EventLoop::quit()`。

## 目录结构

```
include/          头文件（Reactor / Buffer / Protocol / KV / Pool / Logger）
src/              对应实现 + main.cpp
tests/            协议解析单测
CMakeLists.txt
```

## 学习路线图（建议顺序）

| 阶段 | 主题 | 本仓库对应 |
|------|------|------------|
| 1 | epoll 基础 | `Epoller` |
| 2 | 非阻塞 socket | `TcpServer::create_nonblocking_listen` / `accept4` |
| 3 | Reactor 模型 | `EventLoop` + `Channel` |
| 4 | Buffer 粘包/半包 | `Buffer` + `ProtocolParser` |
| 5 | TCP 连接管理 | `TcpConnection` / `TcpServer` |
| 6 | 协议解析 | 行协议 + RESP-lite |
| 7 | Hash 存储 | `KvStore`（`unordered_map`） |
| 8 | TTL | 字段已预留；主动过期 / 时间轮为 TODO |
| 9 | ThreadPool | 最小可用线程池；业务 offload 为 TODO |
| 10 | AsyncLogger | 当前为带锁 stdout；异步落盘为 TODO |
| 11 | Graceful Shutdown | SIGINT → quit loop |
| 12 | Benchmark | 自行用 redis-benchmark / 自写压测 |
| 13 | GoogleTest | 当前为 assert 测试；可自行引入 gtest |

## 已实现 vs TODO

**已实现（hello path）：**

- epoll Reactor 单线程事件循环 + eventfd 唤醒
- 非阻塞 listen / accept / read / write
- 应用层 Buffer（含 readv 技巧）
- 行协议 + 最小 RESP 数组解析（处理粘包/半包）
- SET / GET / DEL / PING
- `unordered_map` 存储 + TTL 字段与惰性删除骨架
- 最小线程池、带锁日志、SIGINT 退出

**留给学生：**

- 完整 RESP（嵌套、pipeline 边界、错误恢复策略）
- SET EX / EXPIRE / TTL 与**定期**过期清理
- 慢查询丢进 ThreadPool，结果 `run_in_loop` 回写
- 异步日志队列 + 文件滚动
- one loop per thread 多 Reactor
- Benchmark 与性能调优
- GoogleTest / sanitizer / CI

## 下一步建议

1. 画一张「连接从 accept 到 GET 响应」的时序图。
2. 给 `SET key value EX seconds` 补协议与存储。
3. 写一个 1000 连接的短压测，观察 `epoll_wait` 与写缓冲积压。
4. 阅读 muduo 的 `TcpConnection` / Redis 的 `ae` 事件循环，对照差异，**用自己的话重写**。

## License

MIT（教学用途）。参考他人代码时请遵守其许可证，并在报告中标注参考来源。
