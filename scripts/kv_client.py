#!/usr/bin/env python3
"""最小 RESP 客户端。演示脚本用，不参与压测指标。"""

import socket
import sys


def encode(args):
    out = f"*{len(args)}\r\n".encode()
    for arg in args:
        raw = arg.encode()
        out += f"${len(raw)}\r\n".encode() + raw + b"\r\n"
    return out


def read_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise EOFError("连接在响应读完前关闭")
        buf += chunk
    return buf


def read_line(sock):
    buf = b""
    while b"\r\n" not in buf:
        chunk = sock.recv(1)
        if not chunk:
            raise EOFError("连接在读行时关闭")
        buf += chunk
    return buf


def read_resp(sock):
    line = read_line(sock)
    kind = line[:1]
    if kind in (b"+", b"-", b":"):
        return line.decode()
    if kind != b"$":
        raise ValueError("无法识别的响应: " + line.decode(errors="replace"))
    length = int(line[1:-2])
    if length < 0:
        return line.decode()
    body = read_exact(sock, length + 2)
    return (line + body).decode()


def show(text):
    return text.replace("\r", "\\r").replace("\n", "\\n")


def main():
    if len(sys.argv) < 3 or sys.argv[1] != "--port":
        print("用法: kv_client.py --port PORT COMMAND [ARG ...]", file=sys.stderr)
        return 2
    port = int(sys.argv[2])
    args = sys.argv[3:]
    if not args:
        print("缺少命令", file=sys.stderr)
        return 2
    sock = socket.create_connection(("127.0.0.1", port), timeout=2)
    try:
        sock.sendall(encode(args))
        resp = read_resp(sock)
    finally:
        sock.close()
    print(">> " + " ".join(args))
    print("<< " + show(resp))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, EOFError, ValueError) as exc:
        print("客户端失败: " + str(exc), file=sys.stderr)
        sys.exit(1)
