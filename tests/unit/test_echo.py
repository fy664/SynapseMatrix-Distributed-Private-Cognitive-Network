"""
SynapseMatrix — tests/unit/test_echo.py
=======================================
TCP 回显的单元测试（Month 2 环节 1）。

覆盖范围：
    - 单客户端回显往返
    - 多客户端并发互不串扰（每个连接独立）
    - 中文消息往返（UTF-8 编码一致性）
    - 服务端未启动时连接拒绝
    - 服务端支持多轮会话（同一连接多次收发）

测试策略：
    每个测试自己启动服务端（随机端口），测完关闭——
    避免共享状态、端口冲突，保证可重复运行。
    随机端口用 port=0 让内核分配（bind 后 sock.getsockname() 拿实际端口）。

运行：cd 项目根目录 && python -m pytest tests/unit -q
"""

import socket
import threading

import pytest

from net.echo_server import serve, handle_client


def _start_server() -> tuple[socket.socket, int]:
    """在随机端口启动回显服务端（独立线程），返回 (服务端 socket, 端口)。"""
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))  # port=0 -> 内核分配空闲端口
    srv.listen(5)
    port = srv.getsockname()[1]

    # 服务端 accept 循环跑在后台线程
    def _run() -> None:
        while True:
            try:
                conn, addr = srv.accept()
            except OSError:
                break  # 测试结束关闭了 srv
            threading.Thread(target=handle_client, args=(conn, addr), daemon=True).start()

    threading.Thread(target=_run, daemon=True).start()
    return srv, port


def _echo(message: str, port: int) -> str:
    """同步发送一条消息并收回显（客户端一次会话）。

    循环收满 expected 字节：服务端按 1024 分块回显，
    单次 recv 只能拿第一块——这正是"字节流无消息边界"
    的直接体现（详见 echo_client.echo_once 的注释）。
    """
    payload = message.encode("utf-8")
    with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
        sock.sendall(payload)
        received = bytearray()
        while len(received) < len(payload):
            chunk = sock.recv(4096)
            if not chunk:
                raise ConnectionError("连接在收满前被关闭")
            received.extend(chunk)
        return bytes(received).decode("utf-8")


class TestEcho:
    @pytest.fixture(autouse=True)
    def _server(self):
        srv, port = _start_server()
        self.port = port
        yield
        srv.close()

    def test_single_message_round_trip(self) -> None:
        reply = _echo("hello", self.port)
        assert reply == "hello"

    def test_chinese_message_round_trip(self) -> None:
        """中文往返不乱码（全链路 UTF-8）。"""
        reply = _echo("记忆同步需要可靠传输", self.port)
        assert reply == "记忆同步需要可靠传输"

    def test_multiple_clients_isolated(self) -> None:
        """并发客户端互不串扰：每个连接收自己的回显。"""
        results: list[str] = []
        errors: list[Exception] = []

        def _worker(tag: str) -> None:
            try:
                results.append(_echo(tag, self.port))
            except Exception as e:  # noqa: BLE001
                errors.append(e)

        threads = [
            threading.Thread(target=_worker, args=(f"client-{i}",))
            for i in range(6)
        ]
        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=5)

        assert not errors
        assert sorted(results) == sorted(f"client-{i}" for i in range(6))

    def test_multi_turn_same_connection(self) -> None:
        """同一连接可多次收发（TCP 是全双工会话，不是一收一断）。"""
        with socket.create_connection(("127.0.0.1", self.port), timeout=5) as sock:
            for i in range(3):
                msg = f"第{i}轮消息"
                sock.sendall(msg.encode("utf-8"))
                reply = sock.recv(4096).decode("utf-8")
                assert reply == msg

    def test_long_message(self) -> None:
        """超过单次 recv 缓冲的消息（验证字节流分块接收，回显仍完整）。"""
        msg = "长消息" * 400  # 1200 字符，超过 RECV_SIZE=1024 字节
        # 客户端一次 send，服务端可能分多次 recv 再 sendall——
        # 回显完整性依赖 sendall 语义；这里验证往返无损
        reply = _echo(msg, self.port)
        assert reply == msg


class TestConnectionRefused:
    def test_unreachable_port_fails_fast(self) -> None:
        """端口不可达时，客户端应快速失败（不挂死）。

        可靠性说明：不用"bind 后 close"的端口（释放瞬间可能被系统
        重新分配，产生竞态）。用"bind 但 listen"的 socket——该端口
        处于不可连接状态。Windows 沙箱里 connect 结果可能是
        ConnectionRefusedError（Linux 典型）或 TimeoutError（环境差异），
        两者都是 OSError 子类；测试断言"快速失败且抛 OSError"，
        验证的核心是客户端不挂死、能快速报错。
        """
        probe = socket.socket()
        probe.bind(("127.0.0.1", 0))  # 只 bind，不 listen
        port = probe.getsockname()[1]

        try:
            with pytest.raises(OSError):
                with socket.create_connection(("127.0.0.1", port), timeout=2) as sock:
                    sock.sendall(b"ping")
        finally:
            probe.close()
