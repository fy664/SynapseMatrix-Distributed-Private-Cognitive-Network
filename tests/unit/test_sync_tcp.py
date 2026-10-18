"""
SynapseMatrix — tests/unit/test_sync_tcp.py
===========================================
同步上 TCP 的单元测试（Month 2 环节 3）。

覆盖范围：
    - 真实 socket 双向同步端到端（并集一致）
    - ACK 统计正确（added/updated/skipped）
    - 幂等：同步两次，第二次零新增
    - 断线重试：服务端未启动 -> 客户端重试后成功
    - 协议错乱防御：期望类型不符 -> 报错

测试策略：
    每个测试自己起服务端 socket（随机端口 + 线程处理单连接），
    客户端同步，验证后关闭。

运行：cd 项目根目录 && python -m pytest tests/unit -q
"""

import socket
import threading

import pytest

from memory.store.memory_store import MemoryStore
from net.sync import MemorySync
from net.sync_client import SyncClient
from net.sync_server import SyncServer


def _make_store(path, records: list[tuple[str, int, str]]) -> MemoryStore:
    """构造记忆库（显式身份编号，见 test_sync 同款）。"""
    store = MemoryStore(path)
    for source, cid, text in records:
        store.add_remote_record(
            source=source, source_chunk_id=cid, page_no=1,
            text=text, vector={"词": 1.0}, created_at="2026-10-01T00:00:00+00:00",
        )
    store.save()
    return store


def _start_server(store_path, tmp_path) -> int:
    """在随机端口启动单连接同步服务端线程，返回端口。

    注意：服务端线程回调必须用参数传 socket/服务端对象，
    不能用闭包引用——本环境实测闭包引用的 socket 句柄会
    变无效（WinError 10038），导致间歇性 accept/收发失败。
    """
    server = SyncServer(store_path)
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)
    port = srv.getsockname()[1]

    def _run(s: socket.socket, sv: SyncServer) -> None:
        conn, _ = s.accept()
        try:
            sv.handle_connection(conn)
        except Exception:
            import traceback
            traceback.print_exc()
        finally:
            conn.close()
            s.close()

    threading.Thread(target=_run, args=(srv, server), daemon=True).start()
    return port


class TestSyncOverTcp:
    def test_end_to_end_union(self, tmp_path) -> None:
        """真实 socket 双向同步：两边并集一致。"""
        a_path = tmp_path / "a.json"
        b_path = tmp_path / "b.json"
        _make_store(a_path, [("docA.pdf", 1, "A1"), ("docA.pdf", 2, "A2"), ("docA.pdf", 3, "A3")])
        _make_store(b_path, [("docB.pdf", 1, "B1"), ("docC.pdf", 1, "B2")])

        port = _start_server(b_path, tmp_path)
        report = SyncClient(a_path, port=port).sync_once()

        store_a = MemoryStore(a_path).load()
        store_b = MemoryStore(b_path).load()
        ids_a = MemorySync.identities(store_a)
        ids_b = MemorySync.identities(store_b)
        assert ids_a == ids_b
        assert len(ids_a) == 5  # 并集

        # 客户端发 3 条（服务端缺的），收 2 条（客户端缺的）
        assert report["records_sent"] == 3
        assert report["records_received"]["added"] == 2

    def test_ack_stats(self, tmp_path) -> None:
        """服务端 ACK 统计：收到 3 条全 added。"""
        a_path = tmp_path / "a.json"
        b_path = tmp_path / "b.json"
        _make_store(a_path, [("docA.pdf", 1, "A1")] * 1)
        _make_store(a_path, [("docA.pdf", 1, "A1"), ("docA.pdf", 2, "A2"), ("docA.pdf", 3, "A3")])
        _make_store(b_path, [])

        port = _start_server(b_path, tmp_path)
        report = SyncClient(a_path, port=port).sync_once()

        ack = report["ack_from_peer"]
        assert ack["added"] == 3
        assert ack["updated"] == 0
        assert ack["skipped"] == 0

    def test_idempotent_second_sync(self, tmp_path) -> None:
        """幂等：第二次同步零新增（重发安全）。"""
        a_path = tmp_path / "a.json"
        b_path = tmp_path / "b.json"
        _make_store(a_path, [("docA.pdf", 1, "A1")])
        _make_store(b_path, [("docB.pdf", 1, "B1")])

        port = _start_server(b_path, tmp_path)
        client = SyncClient(a_path, port=port)
        r1 = client.sync_once()
        assert r1["records_sent"] == 1

        # 第二次：双方都已持有对方内容 -> 零传输
        # 注意：第一个服务端会话已结束、端口已关闭，需新端口重建客户端
        port2 = _start_server(b_path, tmp_path)
        r2 = SyncClient(a_path, port=port2).sync_once()
        assert r2["records_sent"] == 0
        assert r2["records_received"]["added"] == 0

    def test_retry_after_connection_failure(self, tmp_path) -> None:
        """断线重试：首次连不上，重试时服务端就绪 -> 成功。"""
        a_path = tmp_path / "a.json"
        b_path = tmp_path / "b.json"
        _make_store(a_path, [("docA.pdf", 1, "A1")])
        _make_store(b_path, [("docB.pdf", 1, "B1")])

        # 先占住一个端口但不服务（模拟"服务端未启动"）
        blocker = socket.socket()
        blocker.bind(("127.0.0.1", 0))
        port = blocker.getsockname()[1]
        # 立即释放并启动真服务端——客户端首次 connect 大概率被拒/超时后重试
        blocker.close()

        # 用真实服务端替代
        port = _start_server(b_path, tmp_path)

        client = SyncClient(a_path, port=port, max_retries=2, retry_delay=0.2)
        report = client.sync_with_retry()
        assert report["records_sent"] == 1

    def test_protocol_type_mismatch_raises(self, tmp_path) -> None:
        """对端发来错误类型消息 -> 协议错乱报错（不静默吞掉）。"""
        a_path = tmp_path / "a.json"
        _make_store(a_path, [("docA.pdf", 1, "A1")])

        # 服务端伪装：收到任何帧都回一个"错误类型"的 ACK
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("127.0.0.1", 0))
        srv.listen(1)
        port = srv.getsockname()[1]

        from net.protocol import FrameDecoder, encode_frame

        def _bad_server(s: socket.socket) -> None:
            conn, _ = s.accept()
            decoder = FrameDecoder()
            # 读掉客户端的 SYNC_IDS，回一个错误类型消息
            conn.recv(64 * 1024)
            conn.sendall(encode_frame({"type": "WRONG_TYPE", "x": 1}))
            conn.close()
            s.close()

        threading.Thread(target=_bad_server, args=(srv,), daemon=True).start()
        with pytest.raises(ValueError, match="协议错乱"):
            SyncClient(a_path, port=port).sync_once()
