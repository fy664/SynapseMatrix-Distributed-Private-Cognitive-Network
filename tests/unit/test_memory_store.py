"""
SynapseMatrix — tests/unit/test_memory_store.py
================================================
JSON 记忆库持久化的单元测试。

覆盖范围：
    - add_record 分配 id、字段写入
    - 幂等判断 has_source / delete_by_source
    - save -> load 往返一致性（重启不丢）
    - schema 版本校验
    - 纯内存模式（无路径）save 报错

运行：cd 项目根目录 && python -m pytest tests/unit -q
"""

import json

import pytest

from memory.store.memory_store import MemoryRecord, MemoryStore, SCHEMA_VERSION


class TestAddRecord:
    def test_assigns_increasing_ids(self) -> None:
        store = MemoryStore()
        r1 = store.add_record(1, "文本A", {"词": 1.0}, "src.pdf")
        r2 = store.add_record(2, "文本B", {"词": 0.5}, "src.pdf")
        assert r1.chunk_id == 1
        assert r2.chunk_id == 2
        assert len(store) == 2

    def test_created_at_is_iso(self) -> None:
        store = MemoryStore()
        r = store.add_record(1, "文本", {"词": 1.0}, "src.pdf")
        # ISO 格式：2026-10-14T12:00:00.123456+00:00
        assert "T" in r.created_at and ":" in r.created_at


class TestSourceIdempotency:
    def test_has_source(self) -> None:
        store = MemoryStore()
        store.add_record(1, "文本A", {"词": 1.0}, "规格书.pdf")
        assert store.has_source("规格书.pdf")
        assert not store.has_source("其他.pdf")

    def test_delete_by_source(self) -> None:
        store = MemoryStore()
        store.add_record(1, "文本A", {"词": 1.0}, "规格书.pdf")
        store.add_record(2, "文本B", {"词": 0.5}, "规格书.pdf")
        store.add_record(3, "文本C", {"词": 0.2}, "其他.pdf")
        removed = store.delete_by_source("规格书.pdf")
        assert removed == 2
        assert len(store) == 1
        assert not store.has_source("规格书.pdf")


class TestPersistenceRoundTrip:
    def test_save_load_round_trip(self, tmp_path) -> None:
        path = tmp_path / "memory.json"

        # 第一次生命周期：写入并保存
        store = MemoryStore(path)
        store.add_record(1, "端侧优先架构", {"端侧": 1.0}, "规格书.pdf")
        store.add_record(2, "记忆持久化", {"持久化": 0.9}, "规格书.pdf")
        store.save()

        # 模拟"进程重启"：全新对象，从磁盘加载
        loaded = MemoryStore(path).load()
        assert len(loaded) == 2
        records = loaded.all_records()
        assert records[0].chunk_id == 1
        assert records[0].text == "端侧优先架构"
        assert records[0].vector == {"端侧": 1.0}
        assert records[0].source == "规格书.pdf"

    def test_next_chunk_id_survives_restart(self, tmp_path) -> None:
        """重启后续号，不重复分配 id。"""
        path = tmp_path / "memory.json"
        store = MemoryStore(path)
        store.add_record(1, "文本A", {"词": 1.0}, "s.pdf")
        store.save()

        loaded = MemoryStore(path).load()
        r = loaded.add_record(1, "文本B", {"词": 0.5}, "s.pdf")
        assert r.chunk_id == 2  # 不覆盖 1

    def test_file_contains_schema_version(self, tmp_path) -> None:
        path = tmp_path / "memory.json"
        store = MemoryStore(path)
        store.add_record(1, "文本", {"词": 1.0}, "s.pdf")
        store.save()

        payload = json.loads(path.read_text(encoding="utf-8"))
        assert payload["schema_version"] == SCHEMA_VERSION

    def test_schema_version_mismatch_raises(self, tmp_path) -> None:
        path = tmp_path / "memory.json"
        path.write_text(
            json.dumps({"schema_version": 99, "next_chunk_id": 1, "records": []}),
            encoding="utf-8",
        )
        with pytest.raises(ValueError, match="schema"):
            MemoryStore(path).load()

    def test_load_missing_file_raises(self, tmp_path) -> None:
        with pytest.raises(FileNotFoundError):
            MemoryStore(tmp_path / "nope.json").load()


class TestMemoryOnlyMode:
    def test_save_without_path_raises(self) -> None:
        store = MemoryStore()  # 无路径
        store.add_record(1, "文本", {"词": 1.0}, "s.pdf")
        with pytest.raises(FileNotFoundError):
            store.save()

    def test_exists_without_path(self) -> None:
        assert not MemoryStore().exists()
