"""
SynapseMatrix — Net 层：TCP 回显客户端（Month 2 环节 1：网络基础起步）
========================================================================

定位：与服务端（echo_server.py）配对的最小客户端。
验证链路：客户端 send -> 服务端 recv -> 服务端 sendall -> 客户端 recv。

今天要建立的核心认知：
    1. connect() 触发的三次握手对应用层不可见
       客户端只需 connect 成功即可写数据；握手状态机由内核完成。
       若对端不可达/端口未监听，connect 抛 ConnectionRefusedError
       （服务端未启动时，正是"端口不可达"的直观体验）。
    2. 主动关闭是四次挥手
       客户端 close() 后，服务端 recv 会返回 b''——
       这是"对端已关闭"的可靠信号（回显服务端靠它退出循环）。
    3. 数据要编码
       socket 只传字节（bytes），文本必须 encode/decode。
       编码不一致（一端 UTF-8、一端 GBK）会乱码——
       同步协议环节必须统一编码（本项目全链路 UTF-8）。

用法：
    python net/echo_client.py "你好，SynapseMatrix"
    python net/echo_client.py "你好" --host 127.0.0.1 --port 9000
"""

from __future__ import annotations

import argparse
import socket
import sys
from pathlib import Path

# 兼容两种运行方式：
#   python net/echo_client.py ...   （直接运行，sys.path[0]=net/，找不到 net 包）
#   python -m net.echo_client ...   （模块运行，推荐）
# 直接运行时把项目根插入 sys.path，使 `from net.echo_server import ...` 可用。
if __package__ is None:
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from net.echo_server import DEFAULT_HOST, DEFAULT_PORT, RECV_SIZE


def echo_once(message: str, host: str, port: int, timeout: float = 5.0) -> str:
    """连接服务端，发送一条消息，接收回显。

    参数：
        message: 待发送的文本（UTF-8 编码）
        host   : 服务端地址
        port   : 服务端端口
        timeout: 连接/接收超时（秒）。设超时是为了避免
                 服务端异常时客户端永久阻塞（挂死）。

    返回：
        服务端回显的文本（解码为 str）

    抛错：
        ConnectionRefusedError：端口未监听（服务端未启动）
        socket.timeout        ：超时（服务端不响应）

    关于"循环收满"（今天的核心知识点）：
        TCP 是字节流，一次 recv(1024) 最多拿 1024 字节——
        长消息会被截断成多次 recv。回显场景有个便利：
        客户端知道自己发了多少字节，可以循环 recv 直到收满
        expected = len(encoded)，再拼接解码。
        这是特例解法；真实场景（不知道对端会发多少）必须靠
        "长度前缀"帧格式（同步协议环节实现，技术债 #19）。
    """
    # 客户端 socket 同样用 AF_INET + SOCK_STREAM（TCP）
    with socket.create_connection((host, port), timeout=timeout) as sock:
        # bytes 编码：协议层只认字节流
        payload = message.encode("utf-8")
        sock.sendall(payload)

        # 循环收满 expected 字节（echo 场景已知长度）
        expected = len(payload)
        received = bytearray()
        while len(received) < expected:
            chunk = sock.recv(RECV_SIZE)
            if not chunk:
                # 服务端提前关闭：对端断开导致收不满
                raise ConnectionError(
                    f"连接在收满前被关闭: 期望 {expected} 字节, 实际 {len(received)}"
                )
            received.extend(chunk)
        return bytes(received).decode("utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description="SynapseMatrix TCP 回显客户端")
    parser.add_argument("message", help="要发送的消息文本")
    parser.add_argument("--host", default=DEFAULT_HOST, help=f"服务端地址（默认 {DEFAULT_HOST}）")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"服务端端口（默认 {DEFAULT_PORT}）")
    args = parser.parse_args()

    try:
        reply = echo_once(args.message, args.host, args.port)
        print(f"收到回显: {reply}")
    except ConnectionRefusedError:
        print(f"[!] 连接被拒绝: {args.host}:{args.port} —— 服务端未启动？")
    except socket.timeout:
        print(f"[!] 超时: {args.host}:{args.port} 无响应")
    except Exception as e:  # 其他网络错误统一报告
        print(f"[!] 连接失败: {e}")


if __name__ == "__main__":
    main()
