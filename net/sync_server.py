"""
SynapseMatrix — Net 层：同步服务端（Month 2 环节 3：同步上 TCP）
=================================================================

定位：规格书 5.2「P2P 协同」的第一版线上实现——一台设备作为
服务端等待连接，与客户端设备交换记忆差集。

协议时序（一次会话完成双向同步，详见 docs/research/sync-over-tcp.md）：
    1. 客户端 -> 服务端：SYNC_IDS   （客户端全部 identity 列表）
    2. 服务端 -> 客户端：SYNC_IDS   （服务端全部 identity 列表）
    3. 客户端 -> 服务端：SYNC_RECORDS 逐帧（服务端缺的记录）
    4. 服务端 apply 全部 -> 客户端：SYNC_ACK（added/updated/skipped 统计）
    5. 服务端 -> 客户端：SYNC_RECORDS 逐帧（客户端缺的记录）
    6. 客户端 apply 全部 -> 服务端：SYNC_ACK（统计）
    7. 连接关闭（BYE 由断开代替，V1 简化）

为什么要有第 1、2 步身份对账：
    不交换身份集合，双方都不知道"对方有什么"，只能全量传输。
    先对账再传差集——这就是"增量同步"的线上形态
    （传输量 O(差集)，而不是 O(全部)）。

ACK 的语义（今天的核心知识点）：
    服务端把收到的每条记录 apply 进记忆库后，才回 SYNC_ACK。
    "发出去了"不等于"对方收到了"——收到并落库才算安全。
    这是 TCP 可靠传输思想在应用层的复刻：
    网络层只保证字节到达，应用层必须自己确认"语义被应用"。

断线重试（V1 简化）：
    客户端重连后重新跑完整会话；由于记忆有跨设备身份，
    重复发送幂等（apply 时 skipped），不会产生重复记忆。

本模块提供：
    - SyncServer：等待连接，每连接一个会话线程
    - serve 命令行入口：python -m net.sync_server --port 9001
"""

from __future__ import annotations

import argparse
import socket
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from memory.store.memory_store import MemoryRecord, MemoryStore
from net.protocol import (
    FRAME_HEADER_BYTES,
    MSG_ACK_STATS,
    MSG_IDS,
    MSG_RECORDS,
    FrameDecoder,
    encode_frame,
)
from net.sync import Identity, MemorySync, SyncRecord

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 9001
DEFAULT_STORE_PATH = Path("data/device_b.json")  # 服务端 = 设备 B


def _ids_message(records: list[MemoryRecord]) -> dict:
    """把一批记录的 identity 列表打包成 SYNC_IDS 消息。"""
    return {
        "type": MSG_IDS,
        "ids": [list(r.identity) for r in records],  # [source, chunk_id]
    }


class SyncServer:
    """同步服务端：等待客户端连接并完成双向同步。

    用法：
        server = SyncServer(store_path=...)
        server.listen(port)          # 阻塞服务，Ctrl+C 退出
    或编程方式（测试用）：
        server = SyncServer(store_path=...)
        handle = server.handle_connection(conn)   # 单连接会话
    """

    def __init__(self, store_path: str | Path) -> None:
        self._store_path = Path(store_path)
        self._store: MemoryStore | None = None

    def _load_store(self) -> MemoryStore:
        """加载记忆库（首次自动创建）。"""
        if self._store is None:
            store = MemoryStore(self._store_path)
            if store.exists():
                store.load()
            self._store = store
        return self._store

    def handle_connection(self, conn: socket.socket) -> dict:
        """处理一个同步会话（阻塞直到会话结束）。

        参数：
            conn: 已建立的 TCP 连接

        返回：
            会话统计：
                {
                  "ids_received": n,      # 收到的客户端身份数
                  "records_applied": {...},  # 应用到本端的统计
                  "records_sent": n,       # 发给客户端的记录数
                  "ack_received": {...},   # 客户端应用后的确认统计
                }
        """
        store = self._load_store()
        decoder = FrameDecoder()
        pending: list[dict] = []  # 会话级粘包缓冲（见 _recv_one 说明）
        stats: dict = {"ids_received": 0, "records_applied": {}, "records_sent": 0, "ack_received": {}}

        # 1) 收客户端的 SYNC_IDS（对账第一步）
        client_ids: set[Identity] = set()
        msg = self._recv_one(conn, decoder, pending, expect_type=MSG_IDS)
        client_ids = {(str(i[0]), int(i[1])) for i in msg.get("ids", [])}
        stats["ids_received"] = len(client_ids)

        # 2) 回本端 SYNC_IDS（对账第二步）
        conn.sendall(encode_frame(_ids_message(store.all_records())))

        # 3) 收客户端发来的记录帧（服务端缺的），逐条 apply
        applied = {"added": 0, "updated": 0, "skipped": 0}
        while True:
            rec_msg = self._recv_one(conn, decoder, pending, expect_type=MSG_RECORDS)
            if rec_msg.get("done"):
                break  # 记录批次结束标记
            rec = SyncRecord(**rec_msg["record"])
            applied[MemorySync.apply_record(store, rec)] += 1
        store.save()
        stats["records_applied"] = applied

        # 4) 回 ACK（确认已落库）
        ack = {"type": MSG_ACK_STATS, **applied}
        conn.sendall(encode_frame(ack))

        # 5) 发本端独有的记录（客户端缺的）——按收到的客户端身份集合算差集
        #    注意：用步骤 1 收到的 client_ids 判断（客户端未变的身份）
        to_send = [r for r in store.all_records() if r.identity not in client_ids]
        stats["records_sent"] = len(to_send)
        for r in to_send:
            conn.sendall(encode_frame({"type": MSG_RECORDS, "record": SyncRecord.from_record(r).__dict__}))
        conn.sendall(encode_frame({"type": MSG_RECORDS, "done": True}))

        # 6) 收客户端应用后的 ACK
        stats["ack_received"] = self._recv_one(conn, decoder, pending, expect_type=MSG_ACK_STATS)
        return stats

    @staticmethod
    def _recv_one(conn: socket.socket, decoder: FrameDecoder, pending: list, expect_type: str) -> dict:
        """循环直到解出指定类型的一条完整消息。

        核心（本环节实测踩坑）：
            1. feed 解出即消费：粘包时一次返回多条，必须全部缓存。
            2. pending 必须是"会话级"队列（本函数外的持久对象）：
               若把它做成局部变量，粘包时本调用只返回第一条，
               其余帧随局部变量销毁——对端永远等不到 -> 10 秒超时。
               这是本环节间歇性失败（约 10% 概率）的真正根因。
            3. 先消费 pending、不足才 recv。
        """
        while True:
            # 先消费手头已解出的消息（粘包时有多条）
            while pending:
                msg = pending.pop(0)
                if msg.get("type") != expect_type:
                    raise ValueError(
                        f"协议错乱: 期望 {expect_type}, 收到 {msg.get('type')}"
                    )
                return msg
            # 手头没有完整消息，才 recv 补数据
            data = conn.recv(64 * 1024)
            if not data:
                raise ConnectionError("对端提前关闭连接")
            pending.extend(decoder.feed(data))  # extend 保留未消费的帧

    def listen(self, host: str = DEFAULT_HOST, port: int = DEFAULT_PORT) -> None:
        """阻塞监听并处理连接（每连接一线程）。"""
        server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind((host, port))
        server.listen(5)
        print(f"[*] 同步服务端就绪: {host}:{port}（Ctrl+C 退出）")

        executor = ThreadPoolExecutor(max_workers=4)
        try:
            while True:
                conn, addr = server.accept()
                print(f"[+] 客户端连接: {addr[0]}:{addr[1]}")
                executor.submit(self.handle_connection, conn)
        except KeyboardInterrupt:
            print("\n[*] 退出")
        finally:
            executor.shutdown(wait=False, cancel_futures=True)
            server.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="SynapseMatrix 同步服务端（设备 B）")
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--store", default=str(DEFAULT_STORE_PATH), help="记忆库文件")
    args = parser.parse_args()
    SyncServer(args.store).listen(args.host, args.port)
