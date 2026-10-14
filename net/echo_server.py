"""
SynapseMatrix — Net 层：TCP 回显服务端（Month 2 环节 1：网络基础起步）
========================================================================

定位：规格书第 5 章「P2P 协同」的通信底座第一步。
本文件只做一件事：验证"跨进程传数据"的最小路径——TCP 回显服务。
两台设备同步记忆库（规格书 5.2）是后续环节，今天先把管道打通。

今天要建立的核心认知：
    1. TCP 是字节流，不是报文
       recv() 返回的字节数不代表"一条消息"——对端一次 send 的数据
       可能被拆成多次 recv，也可能两次 send 合并成一次 recv（粘包）。
       本环节是回显，不做消息边界解析；但代码里明确注释这一点，
       下一个环节（同步协议）必须引入帧格式（长度前缀）解决粘包。
    2. 三次握手是 connect() 内部完成的
       客户端 socket.connect() 成功返回 = 握手完成，数据可写。
       为什么是三次：第二次握手后服务端其实已经可以收数据，
       第三次是让服务端确认"客户端的确认已到达"，防止过期 SYN
       让服务端建立无效连接（408 高频考点，详见 docs/research/tcp-echo.md）。
    3. 每个客户端独立连接，服务端需要并发处理
       单线程 accept 循环会阻塞——一个客户端不发送，其他客户端全卡住。
       V1 用线程池（ThreadPoolExecutor）处理每个连接，
       V2 换 selectors/异步 I/O 或 C++ epoll（规格书 L4 端侧 C++ 目标）。

用法：
    python net/echo_server.py            # 启动服务端（默认 127.0.0.1:9000）
    python net/echo_client.py <消息>      # 另一个终端，发消息并收回显
"""

from __future__ import annotations

import argparse
import socket
from concurrent.futures import ThreadPoolExecutor

# 默认监听地址：127.0.0.1 仅本机可连（隐私优先——规格书核心原则，
# 端侧服务默认不对外网暴露；V2 开 P2P 时再引入加密与鉴权）
DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 9000

# 单次 recv 上限：TCP 是流，一次 recv 只拿"当前可达的字节"，
# 不代表一条完整消息（粘包问题的根源）。回显场景无需解帧，
# 同步协议环节将改为"长度前缀"帧格式。
RECV_SIZE = 1024


def handle_client(conn: socket.socket, addr: tuple[str, int]) -> None:
    """处理单个客户端连接：循环读 -> 原样写回（回显）。

    为什么回显而不是直接 close：回显让客户端能验证
    "发出去的数据确实到达了服务端"——这是同步协议里
    "确认送达"（ACK）的最小原型。

    参数：
        conn: 已建立的 TCP 连接（accept 返回）
        addr: 客户端地址 (ip, port)，仅用于日志
    """
    print(f"[+] 新连接: {addr[0]}:{addr[1]}")
    try:
        while True:
            data = conn.recv(RECV_SIZE)
            if not data:
                # recv 返回 b'' = 对端正常关闭（四次挥手完成），
                # 退出循环、关闭连接
                print(f"[-] 连接关闭: {addr[0]}:{addr[1]}")
                break
            # 回显：原样写回。注意 send 不一定一次写完——
            # 这里数据量小（<1KB）通常一次完成；大消息需循环 send
            # （技术债 #20：send_all 封装留待同步协议环节）
            conn.sendall(data)
    except (ConnectionResetError, BrokenPipeError) as e:
        # 对端异常断开（如崩溃/RST），非致命错误，记录后继续服务
        print(f"[!] 连接异常断开: {addr[0]}:{addr[1]} ({e})")
    finally:
        conn.close()


def serve(host: str = DEFAULT_HOST, port: int = DEFAULT_PORT) -> None:
    """启动 TCP 回显服务端（阻塞，Ctrl+C 优雅退出）。

    流程：
        1. 创建 socket（AF_INET=IPv4, SOCK_STREAM=TCP）
        2. setsockopt(SO_REUSEADDR)：允许端口快速复用——
           调试时频繁重启服务端，否则 TIME_WAIT 会让端口暂时不可绑
        3. bind：绑定地址与端口
        4. listen(5)：进入监听状态，内核维护连接队列（backlog=5）
        5. 循环 accept：接受新连接，交给线程池处理
    """
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((host, port))
    server.listen(5)
    print(f"[*] 回显服务端已启动: {host}:{port} (Ctrl+C 退出)")

    # 线程池：每个连接一个工作线程。
    # max_workers=8：V1 足够（个人端侧场景并发客户端 <8）；
    # 线程数过多反而因 GIL/上下文切换劣化（见研究记录）。
    executor = ThreadPoolExecutor(max_workers=8)

    try:
        while True:
            conn, addr = server.accept()  # 阻塞等待新连接
            executor.submit(handle_client, conn, addr)
    except KeyboardInterrupt:
        print("\n[*] 收到退出信号，关闭服务端...")
    finally:
        executor.shutdown(wait=False, cancel_futures=True)
        server.close()
        print("[*] 服务端已关闭")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="SynapseMatrix TCP 回显服务端")
    parser.add_argument("--host", default=DEFAULT_HOST, help=f"监听地址（默认 {DEFAULT_HOST}）")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"监听端口（默认 {DEFAULT_PORT}）")
    args = parser.parse_args()
    serve(args.host, args.port)
