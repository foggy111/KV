# 测试计划

自动化测试用 GoogleTest（CMake `FetchContent` 固定 v1.14.0），外加一个 shell 用例覆盖真实进程的 `SIGINT`。

```bash
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=g++
cmake --build build-debug -j
ctest --test-dir build-debug --output-on-failure
```

可选的 AddressSanitizer / UndefinedBehaviorSanitizer：

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=g++ -DKV_SANITIZE=ON
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
```

## 协议 `tests/test_protocol.cpp`

| 用例 | 要证明的事 |
| --- | --- |
| `InlineSet` / `InlineSetEx` | 行协议能解析 `SET`，以及 `EX` 作为参数留下来 |
| `HalfPacket` | 半行、只有 `\r` 都返回 `NeedMore`，补上 `\n` 后得到完整命令 |
| `StickyTwoCommands` | 一次缓冲里的两条行协议都能解析完 |
| `EmptyLineDoesNotStallFollowingCommand` | 空行不会把后面的 `PING` 卡住 |
| `RespSet` / `RespHalfPacket` / `RespStickyTwoCommands` | RESP 数组子集，含半包和粘包 |
| `RespBadArgcDoesNotConsume` | 坏的 `*x` 返回错误且不消费字节 |
| `RespBulkMissingCrlf` | bulk 结尾不是 CRLF 算协议错误 |
| `InlineTooLong` | 超过 1MB 仍无换行算协议错误 |
| `Encode` | `+`、`-ERR`、`$`、`$-1`、`:` 的编码 |

## Buffer `tests/test_buffer.cpp`

| 用例 | 要证明的事 |
| --- | --- |
| `AppendAndRetrieve` | 基本读写 |
| `CrlfAppearsAfterSecondAppend` | `\r` 和 `\n` 分两次到达，随后能解析出两条命令 |
| `CompactKeepsUnreadBytes` | 腾挪前置空间时不丢未读字节 |
| `ResizeKeepsBytes` | 扩容时不丢字节 |
| `ReadFdHalfThenRest` | `socketpair` 上先读半段再读后半段，并解析粘在一起的两条命令 |
| `ReadFdUsesExtraBufferWhenWritableIsSmall` | 可写区小于本次到达的数据时，`readv` 的额外缓冲仍然收全 |

## 存储 `tests/test_kv_store.cpp`

| 用例 | 要证明的事 |
| --- | --- |
| `SetGetDel` | 基本读写删 |
| `SetWithoutTtlClearsOldTtl` | 不带 TTL 的 `SET` 清掉旧过期时间 |
| `LazyExpireOnGet` | 到期后 `GET` 为空，并且把键删掉 |
| `DelExpiredIsMiss` | 已过期的键 `DEL` 返回失败 |
| `TtlSecondsFloor` | `-2` / `-1` / 向下取整；不足 1 秒时 `TTL` 为 0 但 `GET` 仍能读到 |
| `ExpireZeroDeletesLiveKey` | `expire(0)` 删除活着的键 |
| `ExpireMissingOrAlreadyExpired` | 不存在或已过期返回失败 |
| `ExpireOverridesTtl` | `EXPIRE` 覆盖原来的 TTL |
| `ActiveExpireRemovesWithoutGet` | 不调用 `GET`，主动扫描也能删掉过期键 |
| `ActiveExpireSamplesAndContinues` | 每轮有上限，下一轮从游标继续 |
| `ActiveExpireSkipsLiveKeys` | 未过期的键留着 |
| `ZeroTtlArgumentDoesNotExpire` | 存储 API 里 `ttl <= 0` 表示不过期（协议层的 `EX 0` 另有拒绝） |

## 事件循环和线程池

| 用例 | 要证明的事 |
| --- | --- |
| `EventLoop.RunEveryFiresUntilQuit` | 周期定时器会反复触发，`quit()` 能停掉循环 |
| `ThreadPool.RunsQueuedTasks` | 队列里的任务会执行完 |
| `ThreadPool.EnqueueAfterStop` | 停止后拒绝新任务 |

线程池测试只说明这个类能用。服务器默认不把它放进请求路径。

## 集成 `tests/test_integration.cpp`

在后台线程里起真实的 `TcpServer`（端口 0），用 RESP 客户端访问。

| 用例 | 要证明的事 |
| --- | --- |
| `PingSetGetDel` | 线上 `PING`/`SET`/`GET`/`DEL`/`TTL`/`DBSIZE` |
| `StickyWriteAndByteAtATime` | 一次写入多条命令；再逐字节写入 |
| `HalfPacketThenComplete` | 先写 `PIN`，停 80ms，再写完，得到 `+PONG` |
| `SetExThenGetMissAndDbsizeDrops` | `SET EX 1` 到期后，先看 `DBSIZE` 再 `GET`。主动扫描没跑的话，`DBSIZE` 不会先降下来 |
| `ExpireCommand` | `EXPIRE` 到期后同样先看 `DBSIZE` |
| `RejectBadExpire` | `EX 0`、`PX`、负数秒被拒绝，原键还在 |
| `UnknownAndArity` | 未知命令和参数个数 |
| `ProtocolErrorCloses` | `*x\r\n` 得到 `-ERR`，随后连接关闭 |
| `QuitStopsAccepting` | `EventLoop::quit()` 之后新连接失败 |

## 进程级

`scripts/test_graceful.sh`：启动 `kv_server --port 0`，`PING` 成功，发 `SIGINT`，退出码为 0，日志里有「事件循环已停止」和「服务器已退出」。

## 不在这套测试里的事

- Multi-Reactor、`--io-threads`
- 时间轮的时间精度
- 异步日志落盘
- 基准测试的性能回归阈值。`scripts/bench.sh` 只记录原始数，不断言 QPS 必须大于某个值
