#!/usr/bin/env bash
# 按 PPT 的四项指标采集一次原始结果：QPS、P99、进程 CPU、错误率。
# 不填估计值。失败就不写结果文件。
# 用法：bash scripts/bench.sh
# 环境变量：PORT DURATION WARMUP CONNECTIONS KEYS RUNS BUILD_DIR
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
PORT=${PORT:-16379}
DURATION=${DURATION:-5}
WARMUP=${WARMUP:-1}
CONNECTIONS=${CONNECTIONS:-4}
KEYS=${KEYS:-128}
RUNS=${RUNS:-3}
BUILD_DIR=${BUILD_DIR:-"$ROOT/build-bench"}
RESULT_DIR="$ROOT/benchmarks/results"
mkdir -p "$BUILD_DIR" "$RESULT_DIR"

echo "===== 构建 Release ====="
cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER="${CXX:-g++}"
cmake --build "$BUILD_DIR" -j

SERVER="$BUILD_DIR/kv_server"
BENCH="$BUILD_DIR/kv_bench"

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
    echo "端口 $PORT 已被占用" >&2
    exit 1
fi

LOG=$(mktemp)
"$SERVER" --quiet --port "$PORT" >"$LOG" 2>&1 &
PID=$!
cleanup() {
    if [[ -n "${PID:-}" ]] && kill -0 "$PID" 2>/dev/null; then
        kill -INT "$PID" 2>/dev/null || true
        wait "$PID" 2>/dev/null || true
    fi
    rm -f "$LOG"
}
trap cleanup EXIT

for _ in $(seq 1 50); do
    if grep -q "监听端口 ${PORT}" "$LOG"; then
        break
    fi
    if ! kill -0 "$PID" 2>/dev/null; then
        echo "服务器启动失败" >&2
        cat "$LOG" >&2
        exit 1
    fi
    sleep 0.05
done
grep -q "监听端口 ${PORT}" "$LOG"

STAMP=$(date -u +%Y%m%dT%H%M%SZ)
OUT="$RESULT_DIR/bench-${STAMP}.txt"
{
    echo "# reactor-kv raw benchmark"
    echo "# This file is produced by scripts/bench.sh. Do not edit the numbers by hand."
    echo "collected_at=$(date -Is)"
    echo "git_rev=$(git rev-parse HEAD)"
    echo "git_status=$(git status --porcelain | wc -l) dirty_lines"
    echo "hostname=$(hostname)"
    echo "uname=$(uname -a)"
    os_pretty=$(. /etc/os-release && printf '%s' "$PRETTY_NAME")
    echo "os_pretty=${os_pretty}"
    echo "virt=$(systemd-detect-virt 2>/dev/null || true)"
    if grep -qi microsoft /proc/version 2>/dev/null || [[ -f /proc/sys/fs/binfmt_misc/WSLInterop ]]; then
        echo "wsl=yes"
    else
        echo "wsl=no"
    fi
    echo "proc_version=$(cat /proc/version)"
    echo "cpu_model=$(grep -m1 'model name' /proc/cpuinfo | sed 's/.*: //')"
    echo "cpu_cores_online=$(nproc)"
    echo "compiler=$(${CXX:-g++} --version | head -n 1)"
    echo "cmake_build_type=Release"
    echo "compile_command=$(python3 - "$BUILD_DIR/compile_commands.json" <<'PY'
import json, sys
path = sys.argv[1]
try:
    commands = json.load(open(path))
except FileNotFoundError:
    print("NA")
    raise SystemExit
for item in commands:
    if item.get("file", "").endswith("src/main.cpp"):
        print(item.get("command", "NA"))
        break
else:
    print("NA")
PY
)"
    echo "server_bin=$SERVER"
    echo "bench_bin=$BENCH"
    echo "server_flags=--quiet --port $PORT"
    echo "request_log=off"
    echo "load=localhost closed-loop; connections=$CONNECTIONS; pipeline_depth=1; keys=$KEYS; value_bytes=16; mix=alternating SET/GET; warmup_s=$WARMUP; duration_s=$DURATION; runs=$RUNS on one server process"
    echo "sampling=kv_bench prints nearest-rank P99 of per-request RTT and /proc stat CPU of the server pid; see each run block"
    echo "not_a_claim=WSL2 Ubuntu 22.04 presentation machine is 待测 unless this file's uname/os/wsl lines say that is where it ran"
    echo
} >"$OUT"

fail=0
for run in $(seq 1 "$RUNS"); do
    echo "===== run $run / $RUNS ====="
    {
        echo "----- run $run -----"
        if ! "$BENCH" --port "$PORT" --connections "$CONNECTIONS" --duration "$DURATION" \
            --warmup "$WARMUP" --keys "$KEYS" --server-pid "$PID"; then
            echo "run_${run}_exit=failed"
            fail=1
        fi
        echo
    } | tee -a "$OUT"
done

if [[ "$fail" != "0" ]]; then
    echo "有运行失败。原始输出仍留在 $OUT，不要把失败运行摘成成绩。" >&2
    exit 1
fi

echo "原始结果已写入 $OUT"
echo "答辩机若不是文件头里的那台机器，PPT 上仍写待测，并在那台机器重跑本脚本。"
