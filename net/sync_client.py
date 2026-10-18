"""
SynapseMatrix — Net 层：同步客户端（Month 2 环节 3：同步上 TCP）
=================================================================

定位：与服务端（sync_server.py）配对的主动方——发起连接、
主持对账、应用差集，实现两台设备的记忆同步。

协议时序（与服务端对称，详见 sync_server.py 头部）：
    1. 发 SYNC_IDS（本端全部 identity）
    2. 收 SYNC_IDS（对端全部 identity）
    3. 算差集，逐帧发 SYNC_RECORDS（对端缺的记录）
    4. 收 SYNC_ACK（对端应用统计）
    5. 收 SYNC_RECORDS 逐帧（本端缺的记录），apply
    6. 回 SYNC_ACK（本端应用统计）
    7. 关闭连接

断线重试（V1 简化，今天的核心知识点之一）：
    connect 失败或会话中途断线 -> 整体重试。
    为什么重发是安全的：记忆有跨设备身份（source, source_chunk_id），
    对端 apply 时已存在的记录会被 skipped——幂等保证重发不产生重复。
    这正是"TCP 超时重传"思想在应用层的复刻：
    应用层自己也必须容忍重发并做到幂等。

用法：
    python -m net.sync_client --port 9001 --store data/device_a.json
"""

from __future__ import annotations

import argparse
import socket
import sys
import time
from pathlib import Path

from memory.store.memory_store import MemoryRecord, MemoryStore
from net.protocol import (
    MSG_ACK_STATS,
    MSG_IDS,
    MSG_RECORDS,
    FrameDecoder,
    encode_frame,
)
from net.sync import Identity, MemorySync, SyncRecord

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 9001
DEFAULT_STORE_PATH = Path("data/device_a.json")  # 客户端 = 设备 A
MAX_RETRIES = 3          # 断线重试上限
RETRY_DELAY_SEC = 1.0    # 重试间隔


def _ids_message(records: list[MemoryRecord]) -> dict:
    """把一批记录的 identity 列表打包成 SYNC_IDS 消息。"""
    return {
        "type": MSG_IDS,
        "ids": [list(r.identity) for r in records],
    }


def _recv_one(sock: socket.socket, decoder: FrameDecoder, pending: list, expect_type: str) -> dict:
    """循环直到解出指定类型的一条完整消息（与服务端对称）。

    核心（本环节实测踩坑）：
        1. feed 解出即消费：粘包时一次返回多条，必须全部缓存。
        2. pending 必须是"会话级"队列：若为局部变量，粘包时
           本调用只返回第一条，其余帧随局部变量销毁——对端
           永远等不到 -> 10 秒超时（本环节间歇失败真正根因）。
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
        data = sock.recv(64 * 1024)
        if not data:
            raise ConnectionError("对端提前关闭连接")
        pending.extend(decoder.feed(data))  # extend 保留未消费的帧


class SyncClient:
    """同步客户端：连接服务端并完成双向同步。

    用法：
        client = SyncClient(store_path=..., host=..., port=...)
        report = client.sync_once()          # 单次会话（可重试）
        report = client.sync_with_retry()    # 断线自动重试
    """

    def __init__(
        self,
        store_path: str | Path,
        host: str = DEFAULT_HOST,
        port: int = DEFAULT_PORT,
        max_retries: int = MAX_RETRIES,
        retry_delay: float = RETRY_DELAY_SEC,
    ) -> None:
        self._store_path = Path(store_path)
        self._host = host
        self._port = port
        self._max_retries = max_retries
        self._retry_delay = retry_delay

    def _load_store(self) -> MemoryStore:
        store = MemoryStore(self._store_path)
        if store.exists():
            store.load()
        return store

    def sync_once(self) -> dict:
        """执行一次完整同步会话（不重试）。

        返回：
            会话统计（与服务端 handle_connection 对称）。
        """
        store = self._load_store()
        with socket.create_connection((self._host, self._port), timeout=10) as sock:
            decoder = FrameDecoder()
            pending: list[dict] = []  # 会话级粘包缓冲（见 _recv_one 说明）

            # 1) 发本端 SYNC_IDS
            sock.sendall(encode_frame(_ids_message(store.all_records())))

            # 2) 收对端 SYNC_IDS
            remote = _recv_one(sock, decoder, pending, expect_type=MSG_IDS)
            remote_ids = {(str(i[0]), int(i[1])) for i in remote.get("ids", [])}

            # 3) 算差集并逐帧发送（对端缺的记录）
            to_send = [r for r in store.all_records() if r.identity not in remote_ids]
            for r in to_send:
                sock.sendall(encode_frame({"type": MSG_RECORDS, "record": SyncRecord.from_record(r).__dict__}))
            sock.sendall(encode_frame({"type": MSG_RECORDS, "done": True}))

            # 4) 收对端应用统计
            ack_sent = _recv_one(sock, decoder, pending, expect_type=MSG_ACK_STATS)

            # 5) 收对端发来的记录帧（本端缺的），逐条 apply
            applied = {"added": 0, "updated": 0, "skipped": 0}
            while True:
                rec_msg = _recv_one(sock, decoder, pending, expect_type=MSG_RECORDS)
                if rec_msg.get("done"):
                    break
                rec = SyncRecord(**rec_msg["record"])
                applied[MemorySync.apply_record(store, rec)] += 1
            store.save()

            # 6) 回本端应用统计
            sock.sendall(encode_frame({"type": MSG_ACK_STATS, **applied}))

        return {
            "records_sent": len(to_send),
            "ack_from_peer": ack_sent,
            "records_received": applied,
        }

    def sync_with_retry(self) -> dict:
        """带断线重试的同步：失败自动重连重发（幂等安全）。

        为什么重发安全：对端 apply 已存在的记录会 skipped，
        不会产生重复记忆——这是本协议设计的前提（见模块 docstring）。
        """
        last_err: Exception | None = None
        for attempt in range(1, self._max_retries + 1):
            try:
                return self.sync_once()
            except (ConnectionError, socket.timeout, OSError) as e:
                last_err = e
                print(f"[!] 第 {attempt} 次同步失败: {e}")
                if attempt < self._max_retries:
                    time.sleep(self._retry_delay)
        raise ConnectionError(f"同步失败（已重试 {self._max_retries} 次）: {last_err}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="SynapseMatrix 同步客户端（设备 A）")
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--store", default=str(DEFAULT_STORE_PATH), help="记忆库文件")
    parser.add_argument("--retries", type=int, default=MAX_RETRIES, help="断线重试次数")
    args = parser.parse_args()

    client = SyncClient(args.store, args.host, args.port, max_retries=args.retries)
    try:
        report = client.sync_with_retry()
        print(f"同步完成: {report}")
    except ConnectionError as e:
        print(f"[!] {e}")
        sys.exit(1)
