# Reactor KV

教育向的单机键值服务，C++17。请求路径对应职规赛 PPT《让机器人真正跑起来》里 KV Store 那一页：

```
连接管理 → 事件循环 → Buffer / 协议（粘包与半包） → HashMap 存储 + TTL 过期
```

技术点是非阻塞 IO、epoll、单 Reactor。Multi-Reactor 和线程池只留了扩展位置，没有做成可运行模式。

存储用的是 `std::unordered_map`。日志类名叫 `AsyncLogger`，当前实现是互斥锁保护的同步 stdout。这两点和 PPT 上的模块名对应，但不要讲成自研哈希表或已经完成的异步日志。

设计取舍见 [docs/design.md](docs/design.md)。AI 辅助开发的过程和审查记录见 [docs/ai_assisted_development.md](docs/ai_assisted_development.md)、[docs/ai_review_notes.md](docs/ai_review_notes.md)。

## 和 PPT 的对应

| PPT | 代码 | 现在能演示什么 |
| --- | --- | --- |
| 连接管理 | `TcpServer` / `TcpConnection` | 接受连接、读、写、对端关闭。日志里有「接受连接」「连接建立」「连接关闭」 |
| 事件循环 | `EventLoop` + `Epoller` + `Channel` | 单线程 `epoll_wait`。水平触发。`eventfd` 用来跨线程唤醒和处理 `SIGINT` |
| Buffer / 协议，粘包与半包 | `Buffer` + `ProtocolParser` | 行协议和 RESP 数组子集。半包留在缓冲里，一次写入多条命令会逐条响应 |
| HashMap + TTL | `KvStore` | `SET` / `GET` / `DEL`，`SET EX`，`EXPIRE`，`TTL`。到期后 `GET` 为空。另外有定时主动扫描，不访问也会删 |
| 非阻塞 IO、epoll、单 Reactor | `main.cpp` 的启动日志 | 进程起来会写「单 Reactor：命令在事件循环线程内执行」 |
| 向 Multi-Reactor 与线程池演进 | `--io-threads`、`ThreadPool`、`EventLoop::run_every` | 扩展点在，可运行的 Multi-Reactor 没有。`--io-threads` 会直接退出 |
| QPS、P99、CPU、错误率 | `scripts/bench.sh` | 只认 `benchmarks/results/` 里的原始输出。答辩机没跑过就是待测 |

PPT 里写过「功能实现和自动化测试仍待推进」「尚无可信实测」。这个仓库现在把功能、测试和演示补上了。性能数字以原始文件为准，不把构建机的结果说成答辩现场的结果。

## 已实现

- 非阻塞 listen / accept / read / write，`epoll` 水平触发，单 Reactor
- 应用层 `Buffer`（含一次 `readv` 收到超出可写区的数据）
- 行协议，以及 RESP 顶层数组 + bulk string
- `PING`、`SET`、`GET`、`DEL`
- `SET key value EX seconds`、`EXPIRE`、`TTL`
- 惰性过期，加上每 100ms 最多扫描 64 个键的主动过期
- `DBSIZE`：表里的条目数，包含还没扫到的过期键。用来在演示里区分「主动删掉了」和「只是 GET 时才删」
- 请求路径日志，`--quiet` 可关
- `SIGINT` / `SIGTERM` 退出事件循环
- GoogleTest，加上一个真实进程的优雅退出脚本
- `scripts/demo.sh`、`scripts/bench.sh`

默认监听 `6379`。`--port 0` 交给系统分配，实际端口打在「监听端口」那一行。

## 未实现

- Multi-Reactor（`--io-threads` 不是一个隐藏的工作模式）
- 把命令提交到 `ThreadPool` 再写回
- 时间轮、hash / list / zset
- 完整 RESP（嵌套、内联协议以外的所有类型、错误后的重同步）
- `PX` / `NX` / `XX`、带空格的行协议值
- 异步日志和滚动文件
- IPv6
- 在 WSL2 Ubuntu 22.04 答辩机上已经保存的性能原始数据（见下文，没跑就是待测）

## 环境

目标机器是 WSL2 上的 Ubuntu 22.04。代码只依赖 Linux 的 `epoll`、`eventfd`、`accept4`。

本仓库脚本显式使用 `g++`（`-DCMAKE_CXX_COMPILER=g++`）。有的环境里 `/usr/bin/c++` 指向链接不上 `libstdc++` 的 clang，直接 `cmake` 会在编译器检查那一步失败。

依赖：CMake ≥ 3.14，g++ 支持 C++17，Python 3（演示客户端和基准脚本）。测试会从 GitHub 拉取 GoogleTest v1.14.0，配置阶段需要网络。

## 构建和测试

```bash
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=g++
cmake --build build-debug -j
ctest --test-dir build-debug --output-on-failure
```

覆盖范围见 [docs/test_plan.md](docs/test_plan.md)：协议解析、Buffer 粘包/半包、存储、TTL 惰性删除和主动扫描、事件循环定时器、线程池本身、真实 socket 上的命令和过期。`graceful_shutdown` 会启动 `kv_server` 并发送 `SIGINT`。

可选 sanitizer：

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=g++ -DKV_SANITIZE=ON
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
```

GitHub Actions（`.github/workflows/ci.yml`）在 `ubuntu-22.04` 上做 Debug 构建和 `ctest`。它不跑基准，也不产生 PPT 上的性能数字。

## 演示

一条命令，适合投屏。脚本会编译 Release、启动服务器、逐步执行命令、最后发 `SIGINT`。

```bash
bash scripts/demo.sh
```

端口被占用时：

```bash
PORT=6380 bash scripts/demo.sh
```

评委会看到：

1. `PING`、`SET hello world`、`GET hello`、`DEL hello`，再 `GET` 得到空批量回复 `$-1`。
2. 一次写入 `PING\r\nGET missing\r\n`，回来两条响应（粘包）。
3. 先发送 `PIN`，停 0.2 秒，再补 `G\r\n`，得到 `+PONG`（半包）。
4. `SET temp soon EX 2`，立刻 `GET` 能读到，`TTL` 是剩余整秒向下取整（刚写入通常显示 `1`）。大约 3 秒后 `GET` 为空，日志里在这次 `GET` 之前就有 `TTL 主动删除 键=temp`。
5. `EXPIRE persist 2` 同样会过期。`TTL` 对没有过期时间的键返回 `-1`，对不存在的键返回 `-2`。
6. 服务器日志按「读取 → 分发 → 存储 → 响应」打印，然后是「事件循环已停止」「服务器已退出」。

不想用脚本时，两个终端分别执行：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++
cmake --build build -j
./build/kv_server
```

```bash
python3 scripts/kv_client.py --port 6379 PING
python3 scripts/kv_client.py --port 6379 SET hello world
python3 scripts/kv_client.py --port 6379 GET hello
python3 scripts/kv_client.py --port 6379 DEL hello
python3 scripts/kv_client.py --port 6379 GET hello
python3 scripts/kv_client.py --port 6379 SET temp soon EX 2
python3 scripts/kv_client.py --port 6379 GET temp
python3 scripts/kv_client.py --port 6379 TTL temp
sleep 3
python3 scripts/kv_client.py --port 6379 GET temp
python3 scripts/kv_client.py --port 6379 DBSIZE
python3 scripts/kv_client.py --port 6379 SET persist yes
python3 scripts/kv_client.py --port 6379 TTL persist
python3 scripts/kv_client.py --port 6379 EXPIRE persist 2
python3 scripts/kv_client.py --port 6379 TTL persist
sleep 3
python3 scripts/kv_client.py --port 6379 DBSIZE
python3 scripts/kv_client.py --port 6379 GET persist
```

然后在服务器那个终端按 `Ctrl+C`。

本机如果另外装了 `redis-cli`，它走 RESP，可以用来手测 `PING`、`SET`、`GET`、`EXPIRE`、`TTL`。演示脚本不依赖它。

## 命令

| 命令 | 成功时的回复 |
| --- | --- |
| `PING` | `+PONG` |
| `PING message` | 批量字符串，内容是 message |
| `SET key value` | `+OK`，并清掉旧 TTL |
| `SET key value EX seconds` | `+OK`。seconds 必须是正整数 |
| `GET key` | 批量字符串；不存在或已过期是 `$-1` |
| `DEL key` | `:1` 删掉了未过期的键，`:0` 键不存在或已过期 |
| `EXPIRE key seconds` | `:1` 设置成功或 `seconds` 为 0 时删掉了活着的键；`:0` 键不存在或已过期 |
| `TTL key` | 剩余整秒，向下取整；`-1` 没有 TTL；`-2` 不存在或已过期 |
| `DBSIZE` | 当前表项个数，含尚未扫描到的过期键 |

未知命令、参数个数不对、不支持的选项，回复 `-ERR ...`，连接保持。RESP 帧本身损坏时回复 `-ERR` 并关闭连接。

主动扫描的周期和每轮个数写在启动日志里：`主动过期扫描 周期毫秒=100 每轮最多键数=64`。

## 性能实验

PPT 第 11 页的四项是 QPS、P99 延迟、CPU 占用、错误率。采集步骤：

```bash
bash scripts/bench.sh
```

可选环境变量：`PORT`（默认 16379）、`DURATION`（默认 5 秒）、`WARMUP`（默认 1 秒）、`CONNECTIONS`（默认 4）、`KEYS`（默认 128）、`RUNS`（默认 3）、`BUILD_DIR`。

脚本做的事：

1. 用 g++ 编 Release。
2. 启动 `kv_server --quiet`。请求路径日志关掉，否则测到的主要是写 stdout。
3. 同一个服务器进程上连续跑 `kv_bench`。负载是本机闭环：每条连接最多一个未完成请求，`SET` 和 `GET` 交替，值 16 字节，键在一个固定池里轮转。`GET` 未命中（`$-1`）算成功，`-ERR` 和连接失败算错误。
4. P99 用测量窗口内各次往返时间的 nearest-rank（`ceil(0.99 * n) - 1`），不做插值。预热请求和窗口结束时还没回来的请求不计入。
5. CPU 读服务器进程 `/proc/<pid>/stat` 的 `utime+stime`，除以 `CLK_TCK` 和采样窗口。100 表示一个逻辑 CPU 被占满。客户端进程的 CPU 一并写入，用来判断是不是客户端先饱和。
6. 退出码为 0 才把原始文本写到 `benchmarks/results/bench-<UTC时间>.txt`。文件头里有机器、系统、是不是 WSL、编译器、编译命令、负载和采样方法。

怎么读结果：

- 答辩环境（WSL2 Ubuntu 22.04）上如果还没有这样一份文件，四项都写 **待测**。在那台机器上重跑 `bash scripts/bench.sh`，用新文件里的数字。
- 构建机上的文件只代表文件头里的那台机器、那次负载。不要抄进 PPT 当成答辩机成绩，也不要把它说成服务器的极限吞吐。闭环、没有 pipeline、日志关闭，都是这个数字的条件。
- 脑机竞赛的识别率不是本服务的测试结果。本仓库没有真实机器人，也没有真机实验。

结果目录的说明在 [benchmarks/results/README.md](benchmarks/results/README.md)。若下面出现「构建机摘录」，它必须和对应的 `bench-*.txt` 一致；不一致时以 txt 为准。

### 构建机摘录

文件：`benchmarks/results/bench-20261110T063712Z.txt`。

这是一次 Ubuntu 24.04.5 的 KVM 虚拟机测量，`wsl=no`，主机名 `cursor`，`cpu_model=Intel(R) Xeon(R) Processor`，在线 4 核，g++ 13.3.0，编译命令含 `-O3 -DNDEBUG`。它不是 WSL2 Ubuntu 22.04。答辩环境的四项仍然是待测。

`git_rev=681668c90993a6df568621433b71c01250c2b6d7`。文件头 `git_status=1 dirty_lines` 是因为当时脚本先创建了这个 txt，`git status` 把该未跟踪文件自己数了进去。开跑前源码工作区是干净的。采集脚本后来把 `git status` 挪到创建文件之前，以免下次再把结果文件算进去；下面的数字来自挪动之前的那次运行。

同一次 `kv_server --quiet --port 16379`，4 条连接，闭环且每条连接窗口为 1，`SET`/`GET` 交替，128 个键，值 16 字节，预热 1 秒，测量 5 秒，连续 3 次：

| 次序 | QPS | P99（微秒） | 服务器 CPU | 客户端 CPU | 错误率 |
| --- | --- | --- | --- | --- | --- |
| 1 | 205096.4721 | 35 | 98.60 | 99.40 | 0.000000 |
| 2 | 219118.3928 | 24 | 99.60 | 99.80 | 0.000000 |
| 3 | 216474.5675 | 30 | 99.80 | 99.60 | 0.000000 |

CPU 列是「一个逻辑 CPU 为 100」。服务器和 `kv_bench` 都接近打满一个核。这是上述负载下的测量，不是多核扩展成绩，也不是打开请求路径日志时的成绩。日志全开时的 QPS / P99 / CPU / 错误率仍是待测。min、p50、max 和样本数只写在 txt 里。

## AI 辅助开发

人先定的是教学骨架里的模块边界，以及 [docs/design.md](docs/design.md) 里的取舍：单 Reactor、水平触发、命令留在事件循环线程、TTL 的两种删除、哪些失败要关连接。

AI（Cursor 编码代理）写了 CMake / CI / 演示和基准脚本的样板，起草了 GoogleTest，读了原来的解析和连接代码，并根据 ASan 修了关闭连接时的生命周期。审查记录在 [docs/ai_review_notes.md](docs/ai_review_notes.md)。

答辩时人要能讲清事件循环、粘包半包、锁，以及每一个报出去的性能数字。没有原始输出就说待测。这里不估计「AI 写了百分之多少」。

## 参考

epoll 水平触发、带前置空间的应用层缓冲、`readv` 额外缓冲、用 `shared_ptr` 在一次事件处理里保住连接，都是常见 Reactor 教学里的做法，实现时参考过 muduo 一类项目的结构，也参考了 Redis 的 `EXPIRE` / `TTL` 返回值习惯。代码是按这个仓库的接口写的，不是把某个项目整树搬过来。使用别人的代码时遵守对方许可证，并在报告里注明。

## License

MIT（教学用途）。
