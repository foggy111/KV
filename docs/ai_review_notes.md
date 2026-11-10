# AI 审查记录

范围：在原教学骨架上补 TTL、协议边角、请求路径日志、测试、演示和基准脚本。审查方式是读代码、跑 GoogleTest，以及用 AddressSanitizer / UndefinedBehaviorSanitizer 再跑一遍。下面写的是这次实际处理过的问题，不是一份通用清单。

构建机是 Ubuntu 24.04 的 KVM 虚拟机，编译器是 g++ 13.3.0。这不是 WSL2，也不能代替答辩机。

## 发现并改掉的问题

1. **协议错误会忙循环。** `parse_resp` 在 `*x` 这种坏头上返回 `Error` 且不消费字节。`process_commands` 原来收到错误后继续循环，于是同一段数据会被无限解析。现在的约定写在 `protocol.hpp`：`Error` 不消费字节，调用方必须停。连接层回 `-ERR` 后关闭连接。测试：`Protocol.RespBadArgcDoesNotConsume`、`Integration.ProtocolErrorCloses`。

2. **空行把后面的命令卡住。** 行解析遇到空行时返回 `NeedMore`，但字节已经消费掉了。`PING` 前面如果有 `\r\n`，循环会停住，缓冲里的 `PING` 要等下一次读才会被看见。空行现在直接跳过。测试：`Protocol.EmptyLineDoesNotStallFollowingCommand`。

3. **过期键被 `DEL` 当成存在。** 原来 `DEL` 不看 TTL，过期键也会返回成功。现在先按过期处理，过期则删除并返回失败。测试：`KvStore.DelExpiredIsMiss`。

4. **只有惰性删除。** `GET` 会清掉过期键，但没人访问时键一直留在表里。PPT 要求的过期不能只靠这一点。补了 `KvStore::active_expire`，由 `EventLoop::run_every` 每 100ms 抽样最多 64 个键。集成测试在 `GET` 之前看 `DBSIZE`，避免惰性删除把主动扫描的效果掩盖掉。测试：`KvStore.ActiveExpireRemovesWithoutGet`、`Integration.SetExThenGetMissAndDbsizeDrops`。

5. **bulk 结尾不检查 CRLF。** 长度字段对了、后面两个字节不是 `\r\n` 时，解析器会把后面的字节当成下一条命令的开头。现在返回 `bulk missing CRLF` 且不消费。测试：`Protocol.RespBulkMissingCrlf`。

6. **`localtime` 使用静态缓冲区。** 日志改成 `localtime_r`。当前日志有自己的互斥锁，但调用约定应该不依赖那把锁。

7. **`SIGPIPE` 能把进程打死。** 客户端先断开、服务器还在 `write` 时，默认动作是终止进程。`main` 里忽略 `SIGPIPE`，错误留给读写回调。

8. **关闭连接时 `Channel` 被提前析构。** 普通测试没报错。ASan 在 `Integration.PingSetGetDel` 里报了 `heap-use-after-free`：`handle_read` 里关连接，引用计数归零，`Channel` 析构，回到 `Channel::handle_event` 再读 `revents_`。`Channel::tie` 在整个 `handle_event` 期间保住 `TcpConnection`。加上这一步之后，ASan/UBSan 配置下 `ctest` 通过，包括 `scripts/test_graceful.sh`。优雅退出脚本在 ASan 失败时，是因为服务器被 sanitizer 中止，退出码不是 0，不是 SIGINT 本身没接到。

9. **`TcpConnection::shutdown` 捕获裸 `this`。** 改成 `shared_from_this`。这条路径现在演示不用，但原来的写法在连接已销毁时会悬空。

10. **默认构造两个空闲工作线程。** 原来的 `main` 创建 `ThreadPool(2)` 然后 `(void)pool`。日志会打出线程池已启动，看起来像请求已经并发执行。服务器现在不创建这个池。类和单测保留，作为以后 offload 的扩展点。

11. **对端复位后多打一条读失败。** `EPOLLERR` 已经关过连接，同一次事件里的读回调又去 `read`，日志顺序变成「连接错误 / 连接关闭 / 读失败」。`handle_read` 在 `closed_` 之后直接返回。

12. **CPU 字段下标用一次空转核对过。** `kv_bench` 读 `/proc/<pid>/stat` 在 `comm` 之后的第 12、13 个字段（utime、stime）。用本进程空转约 0.4 秒，clk_tck=100，增量约 40 个 tick，换算大约 100% 单核。正式基准的 CPU 仍以 `benchmarks/results/` 里那次运行的原始输出为准，不用这次核对代替成绩。

## 明确拒绝的做法

- 不做一个看起来能跑、但没有对照测试的 `--io-threads`。传入这个参数就退出，并写明未实现。
- 不把 `SET`/`GET` 丢进线程池。响应必须回到 loop 线程写 socket，演示路径会更难讲，也更容易在连接关闭时写错对象。
- 不把 `AsyncLogger` 改成「已经是异步日志」。名字保留，注释和 README 写明它是同步 stdout。
- 不替换 `std::unordered_map` 来伪装自研哈希表，也不实现时间轮然后说成和 Redis 等价。
- 不写 QPS、P99、CPU、错误率的估计值。冒烟跑过的 1 秒数字不收进结果目录。
- 不把脑机竞赛的识别率写进本仓库，不写真实机器人或真机验证。
- 不把构建机（Ubuntu 24.04 虚拟机）的结果写成 WSL2 Ubuntu 22.04 的答辩成绩。

## 仍然要在答辩里讲清楚的限制

- 主动过期是抽样，不是时间轮。键很多、又一直没有读的时候，删除会晚于到期时刻。读路径仍然不会返回过期值。
- `DBSIZE` 包含还没扫到的过期键。
- 行协议不能表达带空格的值。
- RESP 不是完整协议，协议错误直接关连接。
- 请求日志开着时会占事件循环。基准测试关掉它，所以那组数字不包含「日志全开」的开销。日志全开的性能是待测。
- `kv_bench` 是闭环、每连接一个未完成请求，没有 pipeline。QPS 是这个负载下的结果，不是把服务器打到极限的声明。客户端进程自己的 CPU 也记在原始输出里；如果它也接近一个核，就不能把 QPS 说成「服务器还有余量」。
- 水平触发、单线程执行命令、IPv4、`TCP_NODELAY` 都是当前行为的一部分。
- 优雅退出会停止循环并关掉连接，不保证每个客户端都先收到完整的收尾响应。演示用的短响应在 `SIGINT` 之前已经写完。
