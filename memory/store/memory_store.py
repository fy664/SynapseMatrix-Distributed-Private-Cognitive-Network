"""
SynapseMatrix — Memory Store 层：JSON 持久化（Month 1 环节 6/6）
===================================================================

定位：对应规格书 11.1「Memory 持久化」的最小实现。

要解决的问题（技术债 #9）：
    VectorStore 的索引是进程内列表，退出即丢失——
    每次使用都要重新 parse + chunk + embed，而 embed 又要重新 fit 词典。
    持久化的目标：导入一次，下次启动直接可检索。

设计决策：
    1. 用 JSON 而不是数据库（V1 最小形态）
        - 向量是 {词: 权重} 的稀疏 dict，JSON 天然可序列化
        - 无需安装依赖、无需服务进程，单文件即记忆库
        - 代价：全量读写，记忆量大后慢——这是 V2 换 SQLite/向量库的动机
    2. 模型与数据分离
        - embedder 模型（词典/IDF）是"向量空间的定义"，存独立文件
        - 记忆记录是"数据"，存记忆库文件
        - 概念对齐：模型定义空间，数据填充空间——这是 408 之外 ML 的常识，
          也是 V2 换 BGE 时只需换模型文件、不动数据文件的接口保证
    3. 幂等（重复导入同一份 PDF 不产生重复记忆）
        - 按 source（文档来源标识）判断：同源已导入则跳过
        - force=True 可全量重建该源（文档更新后重新导入）
    4. chunk_id 全局递增：重启后从文件续号，保证 id 永不冲突

依赖：标准库 json / datetime / pathlib
"""

from __future__ import annotations

import json
from dataclasses import asdict, dataclass, field
from datetime import datetime, timezone
from pathlib import Path

# 记忆库文件 schema 版本；未来升级格式时用于迁移判断
SCHEMA_VERSION = 1


def _now_iso() -> str:
    """UTC 时间 ISO 字符串（排序友好，供 Memory Score 的 recency 用）。"""
    return datetime.now(timezone.utc).isoformat()


@dataclass
class MemoryRecord:
    """一条持久化的记忆记录。

    属性说明：
        chunk_id      : 本地唯一编号（由 MemoryStore 分配，跨文档递增）
        page_no       : 来源页码（回溯原文用）
        text          : chunk 原文
        vector        : TF-IDF 稀疏向量 {词: 权重}（JSON 可直接存）
        created_at    : 创建时间（ISO 字符串）
        source        : 文档来源标识（如文件名），用于幂等判断
        source_chunk_id: 源文档/远端设备内的编号（跨设备身份用）。
                        本地创建时为 None（身份回退到 chunk_id）；
                        同步应用时保留远端编号——两台设备各自的
                        chunk_id 都从 1 开始，不能直接互认。

    身份（跨设备对账的键）：
        identity = (source, source_chunk_id if set else chunk_id)
        两台设备同一条记忆必须算出相同 identity，同步才正确。
    """

    chunk_id: int
    page_no: int
    text: str
    vector: dict[str, float]
    created_at: str
    source: str
    source_chunk_id: int | None = None

    @property
    def identity(self) -> tuple[str, int]:
        """跨设备身份：(源文档, 源内编号)。"""
        cid = self.source_chunk_id if self.source_chunk_id is not None else self.chunk_id
        return (self.source, cid)


class MemoryStore:
    """JSON 记忆库：增查删 + 落盘/加载。

    用法：
        store = MemoryStore("data/memory.json")
        store.add_record(record)
        store.save()                       # 写盘
        store2 = MemoryStore("data/memory.json")
        store2.load()                      # 读回
        store2.all_records()               # 全部记忆

    文件格式（data/memory.json）：
        {
          "schema_version": 1,
          "next_chunk_id": 57,             # 下次分配的 id
          "records": [ {MemoryRecord 字段} ]
        }
    """

    def __init__(self, path: str | Path | None = None) -> None:
        """初始化空记忆库。

        参数：
            path: 记忆库文件路径；为 None 时仅内存操作（不落盘）
        """
        self._path: Path | None = Path(path) if path else None
        self._records: list[MemoryRecord] = []
        self._next_chunk_id: int = 1

    # ------------------------------------------------------------------
    # 写入
    # ------------------------------------------------------------------

    def add_record(
        self,
        page_no: int,
        text: str,
        vector: dict[str, float],
        source: str,
    ) -> MemoryRecord:
        """分配 id 并追加一条记录。

        返回：
            创建好的 MemoryRecord（chunk_id 已分配）

        说明：
            - 分配后立即占用 id，保证全局递增唯一
            - 调用方需自行在合适的时机 save() 落盘
        """
        record = MemoryRecord(
            chunk_id=self._next_chunk_id,
            page_no=page_no,
            text=text,
            vector=vector,
            created_at=_now_iso(),
            source=source,
        )
        self._records.append(record)
        self._next_chunk_id += 1
        return record

    def add_remote_record(
        self,
        source: str,
        source_chunk_id: int,
        page_no: int,
        text: str,
        vector: dict[str, float],
        created_at: str,
    ) -> MemoryRecord:
        """同步专用：插入一条远端记录，保留远端身份。

        与 add_record 的区别：
            - 本地 chunk_id 仍自动分配（本地唯一性不破坏）
            - source_chunk_id 存远端编号，身份 = (source, source_chunk_id)
              与远端设备对同一记忆算出的身份一致

        参数：
            source        : 远端来源文档标识
            source_chunk_id: 远端设备内的编号
            page_no/text/vector/created_at: 记录内容与远端时间戳

        返回：
            新插入的 MemoryRecord
        """
        record = MemoryRecord(
            chunk_id=self._next_chunk_id,
            page_no=page_no,
            text=text,
            vector=vector,
            created_at=created_at,
            source=source,
            source_chunk_id=source_chunk_id,
        )
        self._records.append(record)
        self._next_chunk_id += 1
        return record

    def has_source(self, source: str) -> bool:
        """该来源是否已导入过（幂等判断）。"""
        return any(r.source == source for r in self._records)

    def delete_by_source(self, source: str) -> int:
        """删除某来源的全部记录（文档更新后重建用）。

        返回：
            删除的记录数
        """
        kept = [r for r in self._records if r.source != source]
        removed = len(self._records) - len(kept)
        self._records = kept
        return removed

    def update_record(
        self,
        source: str,
        source_chunk_id: int,
        text: str,
        vector: dict[str, float],
        created_at: str | None = None,
    ) -> bool:
        """按跨设备身份 (source, source_chunk_id) 更新单条记录的内容。

        用途：同步冲突处理（后写者胜）——远端版本更新时，
        只替换这一条，不动同源其他记录（delete_by_source 会误删）。

        参数：
            source        : 源文档标识
            source_chunk_id: 源内编号（身份键，可能非本地 chunk_id）
            text          : 新文本
            vector        : 新向量
            created_at    : 新创建时间（可选；None 时保留原时间戳）

        返回：
            True  = 找到并更新；False = 无此身份（调用方应先判断）
        """
        for r in self._records:
            if r.identity == (source, source_chunk_id):
                r.text = text
                r.vector = vector
                if created_at is not None:
                    r.created_at = created_at
                return True
        return False

    # ------------------------------------------------------------------
    # 读取
    # ------------------------------------------------------------------

    def all_records(self) -> list[MemoryRecord]:
        """全部记录（拷贝返回，防外部修改内部状态）。"""
        return list(self._records)

    def __len__(self) -> int:
        return len(self._records)

    def __iter__(self):
        return iter(self._records)

    # ------------------------------------------------------------------
    # 持久化
    # ------------------------------------------------------------------

    def save(self) -> None:
        """把记忆库写入 JSON 文件（覆盖写）。

        抛错：
            FileNotFoundError：path 为 None 时（纯内存模式不可落盘）
        """
        if self._path is None:
            raise FileNotFoundError("MemoryStore 未绑定文件路径，无法 save()")

        # 显式创建父目录：首次运行时 data/ 可能不存在
        self._path.parent.mkdir(parents=True, exist_ok=True)
        payload = {
            "schema_version": SCHEMA_VERSION,
            "next_chunk_id": self._next_chunk_id,
            "records": [asdict(r) for r in self._records],
        }
        # ensure_ascii=False：中文以 UTF-8 明文存储，文件可读可 diff
        # indent=2：文本量小，牺牲体积换可读性（V1 取舍）
        self._path.write_text(
            json.dumps(payload, ensure_ascii=False, indent=2),
            encoding="utf-8",
        )

    def load(self) -> "MemoryStore":
        """从 JSON 文件加载记忆库（自返回，支持链式）。

        抛错：
            FileNotFoundError：文件不存在（调用方应先判断）
            ValueError：schema_version 不兼容
        """
        if self._path is None:
            raise FileNotFoundError("MemoryStore 未绑定文件路径，无法 load()")
        if not self._path.exists():
            raise FileNotFoundError(f"记忆库不存在: {self._path}")

        payload = json.loads(self._path.read_text(encoding="utf-8"))
        if payload.get("schema_version") != SCHEMA_VERSION:
            raise ValueError(
                f"schema 版本不兼容: 文件 {payload.get('schema_version')} "
                f"vs 当前 {SCHEMA_VERSION}（需迁移）"
            )

        self._next_chunk_id = int(payload["next_chunk_id"])
        self._records = [MemoryRecord(**r) for r in payload["records"]]
        return self

    def exists(self) -> bool:
        """记忆库文件是否已存在（首次导入 vs 后续加载的判断）。"""
        return self._path is not None and self._path.exists()


if __name__ == "__main__":
    # 演示：写入两条记忆 -> 落盘 -> 重新加载读回
    import tempfile

    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "memory.json"
        store = MemoryStore(path)
        r1 = store.add_record(1, "规格书定义端侧优先架构", {"端侧": 1.0, "架构": 0.8}, "规格书.pdf")
        r2 = store.add_record(2, "记忆需要持久化，重启不丢", {"记忆": 1.0, "持久化": 0.9}, "规格书.pdf")
        store.save()
        print(f"已写入 {len(store)} 条 -> {path}")

        loaded = MemoryStore(path).load()
        print(f"加载回 {len(loaded)} 条, 下个 id = {loaded._next_chunk_id}")
        for r in loaded:
            print(f"  #{r.chunk_id} [{r.source} p{r.page_no}] {r.text[:18]}...")
        print(f"幂等: has_source('规格书.pdf') = {loaded.has_source('规格书.pdf')}")
