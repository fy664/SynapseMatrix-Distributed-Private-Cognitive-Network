"""
SynapseMatrix — Net 层：同步会话 v1（Month 2 环节 2）
=======================================================

定位：规格书 5.2「记忆同步」的语义层——两台设备交换记忆差集。

前置：protocol.py 已解决"怎么传字节"（长度前缀帧），
本模块解决"传什么、以谁为准"。

核心设计：
    1. 增量同步（只传差集）
       两边对账时，只发"对方缺的"记录，不发全量。
       差集 = 本设备全部记录 - 对方已有记录的标识集合。
       对账成本 O(n)（标识集合比较），传输量 O(差集)——这就是增量。

    2. 全局身份 vs 本地编号
       两台设备各自的 chunk_id 都从 1 开始——A 的 #1 和 B 的 #1
       不是同一条记忆。同步必须用全局身份：
       identity = (source, chunk_id)  # 源文档 + 源内编号
       本设备内部仍用本地 chunk_id 索引，跨设备身份另算。
       （V2 可升级为 UUID，见技术债 #24）

    3. 冲突处理（V1 简化）：后写者胜（last-write-wins）
       同一 identity 两边都存在且内容不同 -> 保留 created_at 较新的。
       为什么 V1 可以这么简单：本系统记忆只增不改（无单条更新），
       真实冲突概率极低；复杂的向量合并策略留 V2（规格书 5.2）。
       created_at 为 ISO 字符串，字典序即时间序，可直接比较。

同步流程（双向，一次会话完成）：
    sync(a, b):
        方向 A->B：B 缺的记录，A 发给 B，B 应用后回 ACK
        方向 B->A：A 缺的记录，B 发给 A，A 应用后回 ACK
    结束后：两边记忆完全一致（并集）。

本模块提供：
    - SyncRecord：跨设备传输的记录载体（含全局身份字段）
    - MemorySync：同步协调器（对账、应用、冲突处理）
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Iterable

from memory.store.memory_store import MemoryRecord, MemoryStore

# 全局身份 = (源文档, 源内编号)；见模块 docstring 设计 2
Identity = tuple[str, int]


@dataclass(frozen=True)
class SyncRecord:
    """跨设备传输的记忆记录（协议消息的 record 字段）。

    字段说明：
        source   : 源文档标识（与 MemoryRecord.source 一致）
        chunk_id : 源内编号（与 MemoryRecord.chunk_id 一致）
        page_no  : 页码
        text     : 文本
        vector   : 稀疏向量
        created_at: 创建时间（ISO），冲突时用于后写者胜判断

    注意：chunk_id 不是全局唯一——(source, chunk_id) 才是。
    """

    source: str
    chunk_id: int
    page_no: int
    text: str
    vector: dict[str, float]
    created_at: str

    @property
    def identity(self) -> Identity:
        """全局身份：跨设备对账的键。"""
        return (self.source, self.chunk_id)

    @staticmethod
    def from_record(r: MemoryRecord) -> "SyncRecord":
        """从本地 MemoryRecord 构造（同步时打包）。

        关键：chunk_id 传身份编号（source_chunk_id or chunk_id），
        而非本地唯一 id——本地 id 跨设备无意义。
        """
        return SyncRecord(
            source=r.source,
            chunk_id=r.identity[1],  # 身份编号（源内编号）
            page_no=r.page_no,
            text=r.text,
            vector=r.vector,
            created_at=r.created_at,
        )


class MemorySync:
    """同步协调器：对账 -> 传差集 -> 应用 -> 冲突处理。

    用法：
        sync = MemorySync()
        report = sync.sync_all(store_a, store_b)
        # 之后 store_a 与 store_b 内容一致（并集）
    """

    # ------------------------------------------------------------------
    # 对账（只读）
    # ------------------------------------------------------------------

    @staticmethod
    def identities(records: Iterable[MemoryRecord]) -> set[Identity]:
        """收集一批记录的全局身份集合（用于差集判断）。"""
        return {r.identity for r in records}

    @staticmethod
    def missing_in(
        source_records: Iterable[MemoryRecord],
        target_records: Iterable[MemoryRecord],
    ) -> list[MemoryRecord]:
        """差集：source 需要发给 target 的记录。

        判定规则（增量同步的完整语义）：
            对每个 source 记录 r，查 target 中同 identity 的记录 t：
            1. t 不存在         -> 发送（target 缺，added）
            2. t 存在且文本相同 -> 不发送（幂等，skipped）
            3. t 存在但文本不同 -> 仅当 r 更新（created_at 更晚）才发送，
                                  否则 target 版本更新，不覆盖

        参数：
            source_records: 本设备的全部记录
            target_records: 对端的全部记录

        返回：
            需要发送的记录列表（保持原顺序）

        说明：
            仅按身份集合判断会漏掉"同身份不同内容"的更新——
            冲突更新必须通过差集到达对端，否则后写者胜永不生效。
        """
        target_by_id: dict[Identity, MemoryRecord] = {
            r.identity: r for r in target_records
        }
        to_send: list[MemoryRecord] = []
        for r in source_records:
            t = target_by_id.get(r.identity)
            if t is None:
                to_send.append(r)          # target 缺这条
            elif t.text != r.text and r.created_at > t.created_at:
                to_send.append(r)          # 本方更新，需覆盖对端旧版
            # 其余（相同 / 对端更新）不发送
        return to_send

    # ------------------------------------------------------------------
    # 应用（写入 + 冲突）
    # ------------------------------------------------------------------

    @staticmethod
    def apply_record(store: MemoryStore, rec: SyncRecord) -> str:
        """把一条远端记录应用到本地记忆库。

        三种结果（返回值）：
            "added"   ：本地原本没有 -> 新增（保留远端身份）
            "updated" ：本地有同 identity 但文本不同 -> 冲突，
                        保留 created_at 较新的（后写者胜）
            "skipped" ：本地已有且内容相同 -> 跳过（幂等）

        实现说明：
            - "added" 走 add_remote_record：本地 id 自动分配，
              远端身份存 source_chunk_id——与远端对同一记忆算出的
              identity 保持一致
            - "updated" 走 MemoryStore.update_record：按身份只替换
              冲突那一条，不动同源其他记录
        """
        # 查找本地同 identity 的记录
        local = [r for r in store if r.identity == rec.identity]

        if not local:
            store.add_remote_record(
                source=rec.source,
                source_chunk_id=rec.chunk_id,
                page_no=rec.page_no,
                text=rec.text,
                vector=rec.vector,
                created_at=rec.created_at,
            )
            return "added"

        existing = local[0]
        if existing.text == rec.text:
            return "skipped"  # 内容一致，无需动作

        # 冲突：保留 created_at 较新的（ISO 字符串字典序 = 时间序）
        if rec.created_at > existing.created_at:
            # 远端更新：按全局身份替换内容，保留本地记录的 id 与页号
            store.update_record(
                source=rec.source,
                source_chunk_id=rec.chunk_id,
                text=rec.text,
                vector=rec.vector,
                created_at=rec.created_at,
            )
            return "updated"
        # 本地较新：保留本地（远端版本被丢弃——后写者胜的另一半）
        return "skipped"

    # ------------------------------------------------------------------
    # 完整同步（双向差集）
    # ------------------------------------------------------------------

    def sync_all(self, store_a: MemoryStore, store_b: MemoryStore) -> dict:
        """双向增量同步：结束后两边记忆为并集。

        参数：
            store_a: 设备 A 的记忆库
            store_b: 设备 B 的记忆库

        返回：
            统计报告：
                {
                  "a_to_b": {"sent": n, "added": n, "updated": n, "skipped": n},
                  "b_to_a": {"sent": n, "added": n, "updated": n, "skipped": n},
                }

        说明：
            V1 在同一进程内完成对账（双方记录直接可见），
            传输层（真实 socket + 帧）是下一环节的接入点：
            把"missing_in 结果"编码成 SYNC 帧发出去、对端 apply 后回 ACK，
            语义与本函数完全一致——先验证语义，再接传输。
        """
        # 方向 A -> B
        a_to_b = self._sync_direction(store_a, store_b)

        # 方向 B -> A：基于 B 当前状态（可能已含 A 刚发来的记录）
        # 对 A 做对账——A->B 之后 B 拥有 A 的全部，B->A 只发 B 独有的
        b_to_a = self._sync_direction(store_b, store_a)

        return {"a_to_b": a_to_b, "b_to_a": b_to_a}

    def _sync_direction(self, source: MemoryStore, target: MemoryStore) -> dict:
        """单方向同步：把 source 的差集应用到 target。

        返回统计：
            sent   : 实际发送（target 缺 / source 更新）的记录数
            added  : 应用结果——target 新增
            updated: 应用结果——target 被覆盖（冲突后写者胜）
            skipped: 对账发现无需发送（内容相同 / target 版本更新）
        """
        stats = {"sent": 0, "added": 0, "updated": 0, "skipped": 0}
        target_by_id: dict[Identity, MemoryRecord] = {
            r.identity: r for r in target
        }
        for r in source:
            t = target_by_id.get(r.identity)
            if t is None:
                stats["sent"] += 1
                stats[self.apply_record(target, SyncRecord.from_record(r))] += 1
            elif t.text != r.text and r.created_at > t.created_at:
                stats["sent"] += 1
                stats[self.apply_record(target, SyncRecord.from_record(r))] += 1
            else:
                stats["skipped"] += 1  # 相同，或 target 版本更新（不回退）
        return stats


if __name__ == "__main__":
    # 演示：A 有 docA 的 3 条记忆、B 有 docB/docC 的 2 条记忆
    # 同步后：两边都应持有全部 5 条（并集）
    from pathlib import Path
    from tempfile import TemporaryDirectory

    with TemporaryDirectory() as tmp:
        d = Path(tmp)
        sa = MemoryStore(d / "a.json")
        sb = MemoryStore(d / "b.json")

        # A：docA.pdf 3 条
        for i in range(1, 4):
            sa.add_record(1, f"A的记忆{i}", {"词": float(i)}, "docA.pdf")
        # B：docB.pdf / docC.pdf 各 1 条
        sb.add_record(1, "B的记忆1", {"词": 1.0}, "docB.pdf")
        sb.add_record(1, "B的记忆2", {"词": 2.0}, "docC.pdf")

        print(f"同步前: A={len(sa)} 条, B={len(sb)} 条")
        report = MemorySync().sync_all(sa, sb)
        print(f"同步报告: {report}")
        print(f"同步后: A={len(sa)} 条, B={len(sb)} 条")
        ids_a = {r.identity for r in sa}
        ids_b = {r.identity for r in sb}
        print(f"两边身份集合一致: {ids_a == ids_b}（{len(ids_a)} 个身份）")
