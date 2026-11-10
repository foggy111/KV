# 设计说明

本文档是当前代码遵循的架构、取舍、错误处理和测试计划。公开接口以 `include/` 为准。类的边界（`EventLoop`、`Channel`、`Epoller`、`Buffer`、`ProtocolParser`、`TcpConnection`、`TcpServer`、`KvStore`）来自仓库里原有的教学骨架；这次在这些边界里面补了能演示的 TTL、协议边角和测试。

和 AI 起草、人工讲解的关系见 [ai_assisted_development.md](ai_assisted_development.md)。

## 目标

做一台本机可运行的键值服务，请求路径和职规赛 PPT 第 10 页一致：

```
TCP 连接
  -> 连接管理（TcpServer / TcpConnection）
  -> 事件循环（EventLoop + epoll，单 Reactor）
  -> Buffer 组装字节流，处理粘包和半包
  -> 协议解析（行协议，以及 RESP 数组子集）
  -> HashMap 存储和 TTL（std::unordered_map + 惰性删除 + 定时抽样扫描）
```

技术点是非阻塞 IO、epoll、单 Reactor。Multi-Reactor 和「把命令丢进线程池」是留好的扩展位置，当前没有可运行实现。

## 模块

| 模块 | 文件 | 职责 |
| --- | --- | --- |
| 监听和接入 | `tcp_server.cpp` | 非阻塞 `listen` / `accept4`，新连接挂到同一个 `EventLoop` |
| 连接 | `tcp_connection.cpp` | 读到 `Buffer`，解析命令，调用 `KvStore`，写回响应 |
| 事件循环 | `event_loop.cpp` `epoller.cpp` `channel.cpp` | `epoll_wait`、就绪回调、`eventfd` 唤醒、周期定时器 |
| 缓冲 | `buffer.cpp` | 可读/可写区间，`readv` 把超出可写区的数据收进来 |
| 协议 | `protocol.cpp` | 半包返回 `NeedMore` 且不消费；粘包由调用方循环解析 |
| 存储 | `kv_store.cpp` | `unordered_map`、互斥锁、TTL |
| 日志 | `async_logger.cpp` | 互斥锁保护的同步 stdout。类名沿用 `AsyncLogger`，实现不是异步落盘 |
| 线程池 | `thread_pool.cpp` | 固定线程队列。服务器进程不创建它 |

`main.cpp` 把这些拼起来：安装 `SIGINT`/`SIGTERM`，每 100ms 调用一次 `KvStore::active_expire(64)`，然后 `loop()`。

## 取舍

**水平触发。** `epoll_wait` 使用默认水平触发。半包留在 `Buffer` 里，连接仍然可读时，下一轮还会进来。边缘触发能少一些唤醒，但漏读一次就会卡住，教学和答辩都不划算。

**命令留在事件循环线程。** `SET`/`GET`/`DEL`/`EXPIRE`/`TTL` 都在 `handle_read` 里同步做完再写响应。`KvStore` 自己有互斥锁，所以别的线程调用也安全，但服务器不这么用。顺序简单：一个连接上的命令按读到的顺序执行，写回也在同一个线程。

**线程池不进请求路径。** `ThreadPool` 能跑任务，也有单测。若把存储丢进池子，响应必须再用 `run_in_loop` 回到 loop 线程去写 socket，还要处理连接已经关闭的情况。当前演示用不上这段复杂度，所以 `main` 不启动工作线程。

**`std::unordered_map` 就是 PPT 里的 HashMap。** 没有再写一个开放寻址法的哈希表。过期时间存在条目里，用 `steady_clock` 的绝对时间，避免墙上时钟回拨把 TTL 打乱。

**TTL 是惰性删除加抽样扫描，不是时间轮。**

- 惰性：`GET` / `DEL` / `EXPIRE` / `TTL` 碰到已过期的键会立刻删掉，读路径不会返回过期值。
- 主动：事件循环每 100ms 从游标起最多检查 64 个键，删掉其中已过期的。键的数量小于等于 64 时，过期后大约一个周期内会被扫掉。键很多时，一个无人访问的过期键最坏大约要等 `(键数 / 64) * 100ms` 才被主动删掉。
- `DBSIZE` 返回的是表里的条目数，包含还没被扫到的过期键。演示主动删除时，要在 `GET` 之前看它。

**协议是子集。**

- 行协议按空白分词：`SET key value`、`SET key value EX seconds`。值里面有空格时要用 RESP。
- RESP 只接受顶层 `*N` 加上 bulk string。不接受嵌套数组，也不在协议错误之后尝试重同步。
- `SET` 只支持可选的一个 `EX`。`PX`、`NX`、`XX` 会返回错误。
- `EX` 的秒数必须是正整数。`EXPIRE` 允许 `0`，表示键还在就立即删掉。
- `TTL` 返回剩余整秒，向下取整：不存在是 `-2`，没有过期时间是 `-1`。刚执行 `SET key value EX 2` 时，通常会看到 `1`，因为已经过去了几毫秒。

**日志打在事件循环线程上。** 演示默认打开「路径」日志，方便投屏解释。`scripts/bench.sh` 使用 `--quiet`，避免日志 I/O 变成被测的主要开销。连接建立、主动过期和退出日志仍然保留。

**接受连接时打开 `TCP_NODELAY`。** 响应很短，不希望被 Nagle 攒住。这是负载的一部分，写进了基准测试说明。

**只监听 IPv4 `0.0.0.0`。** 演示和测试都走 `127.0.0.1`。

## 错误处理

| 情况 | 行为 |
| --- | --- |
| 半包 | `NeedMore`，字节留在 `Buffer` |
| 空行 | 跳过，继续解析后面的命令 |
| 行协议超过 1MB 仍没有换行 | 协议错误 |
| RESP 头损坏、bulk 长度非法、bulk 结尾不是 CRLF | 协议错误，且不消费字节 |
| 协议错误 | 回 `-ERR ...`，然后关闭连接。不能原地重试，否则会在同一段坏数据上忙循环 |
| 未知命令、参数个数不对、不支持的 `SET` 选项 | `-ERR`，连接保持 |
| `write` 遇到 `EAGAIN` | 剩下的字节进输出 `Buffer`，关注 `EPOLLOUT` |
| 对端关闭 | 从 epoll 摘掉，释放连接 |
| `SIGPIPE` | 忽略。对端已关时 `write` 返回错误，由读写回调处理，不把进程打死 |
| `SIGINT` / `SIGTERM` | `EventLoop::quit()`。信号处理函数只碰原子变量和 `eventfd` 的 `write` |
| 监听端口被占用 | `bind` 失败，进程以异常信息退出 |

`Channel::tie` 在 `handle_event` 期间保住 `TcpConnection`。关闭连接的回调可能丢掉 map 里的最后一个外部引用；如果这时 `Channel` 已经被析构，`handle_event` 后半段会踩悬空指针。tie 把生命周期延长到这次事件处理结束。

## 扩展点（未实现）

`kv_server --io-threads` 直接退出，并说明 Multi-Reactor 还没有可运行版本。以后如果要做，需要：

1. 一个接受连接的主循环，把新连接分配到独立的 `EventLoop` 线程。
2. 决定 `KvStore` 的锁是否够用，以及同一个键上的命令顺序。
3. 连接很多时的背压：写缓冲上限，慢客户端不能拖垮循环。
4. 和单 Reactor 的对照测试，而不是只加一个开关。

`SO_REUSEPORT` 也留在监听 socket 旁边，单 Reactor 不打开它。

## 测试计划

见 [test_plan.md](test_plan.md)。性能采集方法见仓库根目录 README 的「性能实验」和 `scripts/bench.sh`。没有原始输出的指标写待测。
