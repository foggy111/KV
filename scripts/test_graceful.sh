#!/usr/bin/env bash
# 启动真实 kv_server，PING 一次，再发 SIGINT，确认事件循环退出。
set -euo pipefail

SERVER=${1:-}
if [[ -z "$SERVER" || ! -x "$SERVER" ]]; then
    echo "用法: scripts/test_graceful.sh /path/to/kv_server" >&2
    exit 2
fi

ROOT=$(cd "$(dirname "$0")/.." && pwd)
LOG=$(mktemp)
PID=""

cleanup() {
    if [[ -n "${PID:-}" ]] && kill -0 "$PID" 2>/dev/null; then
        kill -INT "$PID" 2>/dev/null || true
        wait "$PID" 2>/dev/null || true
    fi
    rm -f "$LOG"
}
trap cleanup EXIT

"$SERVER" --port 0 >"$LOG" 2>&1 &
PID=$!

PORT=""
for _ in $(seq 1 50); do
    if grep -q "监听端口 " "$LOG"; then
        PORT=$(grep -oE "监听端口 [0-9]+" "$LOG" | awk '{print $2}' | tail -n 1)
        break
    fi
    if ! kill -0 "$PID" 2>/dev/null; then
        echo "服务器没有起来" >&2
        cat "$LOG" >&2
        exit 1
    fi
    sleep 0.05
done
if [[ -z "$PORT" ]]; then
    echo "没有解析到监听端口" >&2
    cat "$LOG" >&2
    exit 1
fi

OUT=$(python3 "$ROOT/scripts/kv_client.py" --port "$PORT" PING)
printf '%s\n' "$OUT"
printf '%s\n' "$OUT" | grep -q '+PONG'

kill -INT "$PID"
wait "$PID"
STATUS=$?
PID=""
if [[ "$STATUS" -ne 0 ]]; then
    echo "SIGINT 后退出码是 $STATUS" >&2
    cat "$LOG" >&2
    exit 1
fi

grep -q "事件循环已停止" "$LOG"
grep -q "服务器已退出" "$LOG"
echo "graceful shutdown ok, port=$PORT"
