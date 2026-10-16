"""
SynapseMatrix — tests/unit/test_sync.py
=======================================
同步会话 v1 的单元测试（Month 2 环节 2）。

覆盖范围：
    - 差集计算（missing_in）
    - 双向同步后并集一致
    - 增量：只传差集，不传全量（sent 计数）
    - 幂等：重复同步结果不变
    - 冲突：后写者胜（updated），本地更新保留（skipped）
    - 内容一致跳过（skipped）
    - MemoryStore.update_record 单条更新不动同源其他记录

运行：cd 项目根目录 && python -m pytest tests/unit -q
"""

from datetime import datetime, timedelta, timezone

import pytest

from memory.store.memory_store import MemoryRecord, MemoryStore
from net.sync import MemorySync, SyncRecord


def _make_store(records: list[tuple[str, int, str]]) -> MemoryStore:
    """构造记忆库：records = [(source, source_chunk_id, text)]。

    用 add_remote_record 显式指定身份编号（source_chunk_id），
    使测试能精确控制跨设备身份——本地递增 id 无法表达测试意图。
    """
    store = MemoryStore()
    for source, cid, text in records:
        store.add_remote_record(
            source=source,
            source_chunk_id=cid,
            page_no=1,
            text=text,
            vector={"词": 1.0},
            created_at=_iso(1),
        )
    return store


def _iso(days_ago: int) -> str:
    """构造 n 天前的 ISO 时间戳（冲突测试用）。"""
    dt = datetime.now(timezone.utc) - timedelta(days=days_ago)
    return dt.isoformat()


class TestDiff:
    def test_missing_in_basic(self) -> None:
        a = _make_store([("docA.pdf", 1, "A1"), ("docA.pdf", 2, "A2"), ("docB.pdf", 1, "B1")])
        b = _make_store([("docB.pdf", 1, "B1")])  # b 只有 docB.pdf 一条

        missing = MemorySync.missing_in(a, b)
        # A 的 docA.pdf 两条是 B 缺的；docB.pdf (1) 与 B 已有身份相同
        assert len(missing) == 2
        assert {r.text for r in missing} == {"A1", "A2"}

    def test_identities_keyed_by_source_and_chunk(self) -> None:
        """身份 = (source, chunk_id)：不同源的相同本地编号不是同一身份。"""
        a = _make_store([("docA.pdf", 1, "A"), ("docB.pdf", 1, "B")])
        ids = MemorySync.identities(a)
        assert ("docA.pdf", 1) in ids
        assert ("docB.pdf", 1) in ids
        assert len(ids) == 2  # 两个不同身份，即使本地 chunk_id 都是 1

    def test_newer_version_in_diff(self) -> None:
        """差集包含更新版本：同身份、内容不同、本方较新 -> 发送。"""
        a = MemoryStore()
        b = MemoryStore()
        a.add_record(1, "版本1", {"词": 1.0}, "docA.pdf")
        b.add_record(1, "版本1", {"词": 1.0}, "docA.pdf")
        # A 更新为版本2（时间戳更新）
        a.update_record("docA.pdf", 1, "版本2", {"词": 2.0}, created_at=_iso(0))

        missing = MemorySync.missing_in(a, b)
        assert len(missing) == 1
        assert missing[0].text == "版本2"

    def test_older_version_not_in_diff(self) -> None:
        """差集排除旧版本：同身份、本方较旧 -> 不发送（避免回退）。"""
        a = MemoryStore()
        b = MemoryStore()
        a.add_record(1, "版本2", {"词": 2.0}, "docA.pdf")
        b.add_record(1, "版本2", {"词": 2.0}, "docA.pdf")
        # A 被回退到旧版本（时间戳更旧）
        a.update_record("docA.pdf", 1, "版本1", {"词": 1.0}, created_at=_iso(30))

        assert MemorySync.missing_in(a, b) == []


class TestSyncAll:
    def test_union_after_sync(self) -> None:
        """双向同步后：两边身份集合一致且为并集。"""
        a = _make_store([("docA.pdf", 1, "A1"), ("docA.pdf", 2, "A2"), ("docA.pdf", 3, "A3")])
        b = _make_store([("docB.pdf", 1, "B1"), ("docC.pdf", 1, "B2")])

        report = MemorySync().sync_all(a, b)

        assert report["a_to_b"]["sent"] == 3
        assert report["a_to_b"]["added"] == 3
        assert report["b_to_a"]["sent"] == 2
        assert report["b_to_a"]["added"] == 2

        ids_a = MemorySync.identities(a)
        ids_b = MemorySync.identities(b)
        assert ids_a == ids_b
        assert len(ids_a) == 5  # 并集：3 + 2

    def test_incremental_only_delta(self) -> None:
        """增量：A 有 10 条、B 有 9 条（缺 1 条）-> 只传 1 条。"""
        a = _make_store([("docA.pdf", i, f"A{i}") for i in range(10)])
        b = _make_store([("docA.pdf", i, f"A{i}") for i in range(9)])

        report = MemorySync().sync_all(a, b)
        assert report["a_to_b"]["sent"] == 1
        assert report["b_to_a"]["sent"] == 0  # B 没有 A 缺的

    def test_idempotent_second_sync_noop(self) -> None:
        """幂等：同步完成后再次同步，无任何传输。"""
        a = _make_store([("docA.pdf", 1, "A1")])
        b = _make_store([("docB.pdf", 1, "B1")])

        MemorySync().sync_all(a, b)
        report2 = MemorySync().sync_all(a, b)
        assert report2["a_to_b"]["sent"] == 0
        assert report2["b_to_a"]["sent"] == 0

    def test_identical_content_skipped(self) -> None:
        """同身份同内容：skipped 而非 added/updated。"""
        a = MemoryStore()
        b = MemoryStore()
        a.add_record(1, "相同内容", {"词": 1.0}, "docA.pdf")
        b.add_record(1, "相同内容", {"词": 1.0}, "docA.pdf")

        report = MemorySync().sync_all(a, b)
        assert report["a_to_b"]["skipped"] == 1
        assert report["a_to_b"]["added"] == 0


class TestConflict:
    def test_newer_wins(self) -> None:
        """冲突：远端版本较新 -> updated（后写者胜）。"""
        a = MemoryStore()
        b = MemoryStore()
        a.add_record(1, "旧版本内容", {"词": 1.0}, "docA.pdf")
        b.add_record(1, "旧版本内容", {"词": 1.0}, "docA.pdf")

        # B 端更新内容，且时间戳更新
        b.update_record("docA.pdf", 1, "B端新版本", {"词": 2.0}, created_at=_iso(0))
        # A 端时间戳更旧（模拟早先同步的版本）
        a.update_record("docA.pdf", 1, "旧版本内容", {"词": 1.0}, created_at=_iso(30))

        report = MemorySync().sync_all(a, b)
        assert report["b_to_a"]["updated"] == 1

        # A 端已被 B 的版本覆盖
        updated = [r for r in a if r.source == "docA.pdf"][0]
        assert updated.text == "B端新版本"

    def test_local_newer_kept(self) -> None:
        """冲突：本地版本较新 -> 保留本地（远端被丢弃）。"""
        a = MemoryStore()
        b = MemoryStore()
        a.add_record(1, "A端较新版本", {"词": 2.0}, "docA.pdf")
        b.add_record(1, "B端较旧版本", {"词": 1.0}, "docA.pdf")
        a.update_record("docA.pdf", 1, "A端较新版本", {"词": 2.0}, created_at=_iso(0))
        b.update_record("docA.pdf", 1, "B端较旧版本", {"词": 1.0}, created_at=_iso(30))

        report = MemorySync().sync_all(a, b)
        assert report["b_to_a"]["skipped"] == 1  # 远端被丢弃 -> 无动作

        kept = [r for r in a if r.source == "docA.pdf"][0]
        assert kept.text == "A端较新版本"

    def test_update_record_preserves_siblings(self) -> None:
        """update_record 只改目标记录，同源其他记录不受影响。"""
        store = MemoryStore()
        store.add_record(1, "记录1", {"词": 1.0}, "docA.pdf")
        store.add_record(1, "记录2", {"词": 2.0}, "docA.pdf")

        ok = store.update_record("docA.pdf", 1, "记录1改", {"词": 9.0})
        assert ok is True
        texts = {r.text for r in store}
        assert texts == {"记录1改", "记录2"}  # 记录2 未被误删

    def test_update_record_missing_identity(self) -> None:
        store = MemoryStore()
        store.add_record(1, "记录", {"词": 1.0}, "docA.pdf")
        assert store.update_record("不存在的源", 1, "x", {}) is False
