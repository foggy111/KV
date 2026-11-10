#!/usr/bin/env bash
# 答辩投屏用。启动服务器，逐步演示 PING / SET / GET / DEL / TTL，最后 Ctrl+C 等价的 SIGINT。
# 用法：bash scripts/demo.sh
# 换端口：PORT=6380 bash scripts/demo.sh
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
PORT=${PORT:-6379}
BUILD_DIR=${BUILD_DIR:-"$ROOT/build"}
LOG=${LOG:-"$BUILD_DIR/demo-server.log"}

mkdir -p "$BUILD_DIR"
if [[ "${SKIP_BUILD:-0}" != "1" ]]; then
    echo "===== 构建 Release（已编过会增量编译）====="
    cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER="${CXX:-g++}"
    cmake --build "$BUILD_DIR" -j
fi

if [[ ! -x "$BUILD_DIR/kv_server" ]]; then
    echo "找不到 $BUILD_DIR/kv_server"
    exit 1
fi

if python3 - "$PORT" <<'PY'
import socket, sys
port = int(sys.argv[1])
s = socket.socket()
s.settimeout(0.2)
busy = s.connect_ex(("127.0.0.1", port)) == 0
s.close()
sys.exit(0 if busy else 1)
PY
then
    echo "端口 $PORT 已被占用。换一个端口再跑：PORT=6380 bash scripts/demo.sh"
    exit 1
fi

echo "===== 启动 kv_server 端口 $PORT ====="
"$BUILD_DIR/kv_server" --port "$PORT" >"$LOG" 2>&1 &
PID=$!

cleanup() {
    if [[ -n "${PID:-}" ]] && kill -0 "$PID" 2>/dev/null; then
        kill -INT "$PID" 2>/dev/null || true
        wait "$PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

ready=0
for _ in $(seq 1 50); do
    if grep -q "监听端口 ${PORT}" "$LOG" 2>/dev/null; then
        ready=1
        break
    fi
    if ! kill -0 "$PID" 2>/dev/null; then
        echo "服务器启动失败，日志如下："
        cat "$LOG"
        PID=""
        exit 1
    fi
    sleep 0.05
done
if [[ "$ready" != "1" ]]; then
    echo "等待监听超时，日志如下："
    cat "$LOG"
    exit 1
fi

run() {
    echo
    echo "----- $* -----"
    python3 "$ROOT/scripts/kv_client.py" --port "$PORT" "$@"
}

echo
echo "请求路径：连接管理 → 事件循环 → Buffer/协议 → HashMap + TTL"
echo "下面每条命令都会新开一个短连接，方便在日志里看到接受连接和分发。"

run PING
run SET hello world
run GET hello
run DEL hello
run GET hello

echo
echo "===== 粘包：一次 write 里放两条命令 ====="
python3 - "$PORT" <<'PY'
import socket, sys
port = int(sys.argv[1])
payload = b"PING\r\nGET missing\r\n"
s = socket.create_connection(("127.0.0.1", port), timeout=2)
s.settimeout(2)
s.sendall(payload)
buf = b""
while buf.count(b"\n") < 2:
    chunk = s.recv(128)
    if not chunk:
        break
    buf += chunk
s.close()
shown = buf.decode().replace("\r", "\\r").replace("\n", "\\n\n")
print(">> PING\\r\\nGET missing\\r\\n")
print("<<")
print(shown, end="" if shown.endswith("\n") else "\n")
PY

echo
echo "===== 半包：先送到 PIN，停一下，再补上 G\\r\\n ====="
python3 - "$PORT" <<'PY'
import socket, sys, time
port = int(sys.argv[1])
s = socket.create_connection(("127.0.0.1", port), timeout=2)
s.settimeout(2)
s.sendall(b"PIN")
time.sleep(0.2)
s.sendall(b"G\r\n")
buf = b""
while b"\n" not in buf:
    chunk = s.recv(128)
    if not chunk:
        break
    buf += chunk
s.close()
print(">> PIN   (停 0.2 秒)   G\\r\\n")
print("<< " + buf.decode().replace("\r", "\\r").replace("\n", "\\n"))
PY

echo
echo "===== TTL：SET key value EX 秒 ====="
run SET temp soon EX 2
run GET temp
run TTL temp
echo
echo "等待 3 秒。EX 2 到期后，GET 应为空；主动扫描周期是 100ms，DBSIZE 不应再把 temp 算进去。"
for sec in 3 2 1; do
    echo "  ... ${sec}"
    sleep 1
done
run GET temp
run TTL temp
run DBSIZE

echo
echo "===== EXPIRE / TTL ====="
echo "TTL 是剩余秒数向下取整。EXPIRE 2 刚执行完，通常显示 1 而不是 2。"
run SET persist yes
run TTL persist
run EXPIRE persist 2
run TTL persist
echo "等待 3 秒让 persist 过期"
sleep 3
run DBSIZE
run GET persist

echo
echo "===== 发送 SIGINT，等价于演示时按 Ctrl+C ====="
kill -INT "$PID"
wait "$PID" || true
PID=""
trap - EXIT

echo
echo "===== 服务器日志（请求路径 / TTL / 退出）====="
grep -E "路径 |TTL |监听端口|接受连接|连接建立|连接关闭|事件循环|服务器已退出|单 Reactor|Multi-Reactor|主动过期|Ctrl\\+C" "$LOG" || true

echo
echo "演示结束。日志文件：$LOG"
echo "若要看性能数字，请另跑 bash scripts/bench.sh，不要把这次功能演示当成压测。"
