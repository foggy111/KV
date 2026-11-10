# AI 辅助开发说明

这个仓库会拿到职规赛就业赛道上讲。评委应该能看到开发过程，而不是只听到「用了 AI」。这里不写「百分之多少是 AI 写的」这种无法核对的比例。

## 人先定边界，AI 填样板和草稿

仓库里原来的教学骨架已经定了公开接口：`EventLoop`、`Channel`、`Epoller`、`Buffer`、`ProtocolParser`、`TcpConnection`、`TcpServer`、`KvStore`、`ThreadPool`、`AsyncLogger`。那些头文件里的类和「单 Reactor、非阻塞、epoll」的方向是人写下来的。

`docs/design.md` 把这次要守住的取舍写清楚：水平触发、命令留在事件循环线程、TTL 用惰性删除加抽样扫描、协议只做子集、Multi-Reactor 不做假开关。实现、测试和演示都对着这份边界，而不是先生成一套另一风格的服务器再回头解释。

## AI 适合做的事

这次实际用到的，是 Cursor 里的编码代理：

- CMake、GitHub Actions、`scripts/demo.sh`、`scripts/bench.sh`、`scripts/kv_client.py` 这类样板
- 按测试计划生成 GoogleTest 草稿，再改到能编译、能针对真实行为断言
- 读原来的解析器和连接代码，指出协议错误不消费字节、空行会卡住后续命令、`DEL` 把过期键当成删除成功
- 看 AddressSanitizer 的栈，修「关闭连接时 `Channel::handle_event` 还在用已经被析构的对象」
- README、注释、提交说明的第一稿

审查里记下了什么、拒绝了什么，见 [ai_review_notes.md](ai_review_notes.md)。

## AI 不能代劳的事

答辩时人要能自己讲，不能靠现场再问一遍模型：

1. 为什么是单 Reactor、水平触发，而不是一开始就做 Multi-Reactor。
2. `epoll_wait` 返回之后，读、定时器、跨线程任务各在哪一步。
3. 半包为什么要留在 `Buffer` 里并且不消费；粘包为什么要在一个循环里解析多条。
4. `KvStore` 的锁保护什么；主动扫描和惰性删除各保证什么，`DBSIZE` 为什么可能还数着没扫到的过期键。
5. 每一个 QPS、P99、CPU、错误率从哪一个原始文件来，负载是什么，这台机器是不是答辩用的 WSL2。没有原始输出就说待测。

AI 不得编造这些数字。`scripts/bench.sh` 只有在进程真正跑完且退出码为 0 时才留下 `benchmarks/results/bench-*.txt`。

## 每个模块可以怎么循环

1. 在设计说明里写清这个模块的接口、失败时怎么办、要测什么。
2. 让 AI 按测试计划起草测试。人要看断言是不是在测行为，而不是在测 AI 自己刚写的实现细节。
3. 把实现改到测试通过。这次提交里，这一步由编码代理做完，并用本机 `ctest` 和 ASan 复核。
4. 把审查结论写进 `docs/ai_review_notes.md`：抓住了什么，拒绝了什么，还剩什么限制。

人如果讲不清第 3 步的核心路径，这个循环就没有完成。测试通过只说明当前用例是绿的，不代替对 epoll、Buffer 和锁的理解。
