"""
SynapseMatrix — Memory Graph 层：最小图谱（Month 1 环节 5/6 最小实现）
=======================================================================

定位：对应规格书第 10 章「图谱设计」，是 L4 Memory Fabric 的关系维度。

为什么记忆需要图谱：
    纯向量检索只回答"哪些 chunk 词面/语义相似"，回答不了"实体之间什么关系"。
    例如检索"为什么决定学 Agent"：
        - 向量层：找到所有提到 Agent 的 chunk
        - 图层面：发现"Agent 项目 <DERIVED_FROM> 考研目标 <MENTIONS> 南邮"，
          把"考研"和"Agent"两个看似无关的记忆通过关系连起来——
          这就是规格书场景 B 的"图关系"维度。

V1 范围（先手写最小实现，不引 Neo4j）：
    - 邻接表存储：node -> set(neighbor)，O(1) 查相邻
    - 支持两种关系查询：
        a) 直接相关（neighbors）：与查询节点有边相连的节点
        b) 二跳相关（2-hop）：与查询节点隔一个中间节点的节点
          （"间接相关"的最小形态，对应图检索的 BFS 深度 2）
    - 图相关度归一化：邻居数 / 全图最大度，落在 [0,1] 供 Memory Score 使用

V1 简化（技术债，记入 TECH-DEBT）：
    - 无关系类型区分（规格书 10.2 的 RELATED_TO / MENTIONS / PART_OF 等
      统一按"有边"处理；区分关系类型是 V2）
    - 无节点属性（实体类型 Person/Project/Document 等，V2 加）
    - 无带权边（权重 = 共现次数等，V2 加）

依赖：标准库 collections（无第三方依赖）
"""

from __future__ import annotations

from collections import defaultdict


class MemoryGraph:
    """极简无向图（邻接表），用于记忆间的实体关系。

    用法：
        g = MemoryGraph()
        g.add_edge("Agent项目", "考研目标")   # 建立关系（无向）
        g.add_edge("考研目标", "南邮")
        g.neighbors("考研目标")               # -> {"Agent项目", "南邮"}
        g.graph_relevance("考研目标")          # 归一化相关度
    """

    def __init__(self) -> None:
        # 邻接表：node -> set(neighbor)。无向图：加边时两端互加。
        self._adj: dict[str, set[str]] = defaultdict(set)

    def add_node(self, node: str) -> None:
        """添加孤立节点（无边的节点也能出现在图中）。"""
        if not node:
            raise ValueError("节点名不能为空")
        self._adj.setdefault(node, set())

    def add_edge(self, a: str, b: str) -> None:
        """在 a 与 b 之间建立无向边（幂等：重复加边无副作用）。

        参数校验：两端非空且不相同（自环在图谱里无意义，V1 拒绝）。
        """
        if not a or not b:
            raise ValueError("边两端节点名不能为空")
        if a == b:
            raise ValueError(f"自环不允许: {a} == {b}")
        self._adj[a].add(b)
        self._adj[b].add(a)

    def add_edges_from_pairs(self, pairs: list[tuple[str, str]]) -> None:
        """批量加边（供 pipeline 一次性灌入实体关系）。"""
        for a, b in pairs:
            self.add_edge(a, b)

    def __contains__(self, node: str) -> bool:
        return node in self._adj

    def __len__(self) -> int:
        return len(self._adj)

    def node_count(self) -> int:
        """节点数。"""
        return len(self._adj)

    def edge_count(self) -> int:
        """边数（无向边计 1 条：每条边在邻接表里出现两次，故除 2）。"""
        return sum(len(ns) for ns in self._adj.values()) // 2

    def neighbors(self, node: str) -> set[str]:
        """返回与 node 直接相连的节点集合（一阶邻居）。

        不存在的节点返回空集合（V1 简化：不报错，便于检索时容错）。
        """
        return set(self._adj.get(node, set()))

    def two_hop(self, node: str, max_depth: int = 2) -> set[str]:
        """返回从 node 出发不超过 max_depth 跳可达的节点集合（不含 node 自身）。

        实现：BFS 受限深度遍历。
        - max_depth=1：等价于 neighbors
        - max_depth=2：直接邻居 + 邻居的邻居（间接相关的"最小形态"）

        参数：
            node     : 起点
            max_depth: 最大跳数（>= 1）

        返回：
            可达节点集合；不存在的节点返回空集合
        """
        if max_depth < 1:
            raise ValueError(f"max_depth 必须 >= 1，收到 {max_depth}")
        if node not in self._adj:
            return set()

        visited: set[str] = {node}
        frontier: set[str] = {node}
        for _ in range(max_depth):
            nxt: set[str] = set()
            for current in frontier:
                nxt.update(self._adj.get(current, set()))
            nxt -= visited  # 不回头：已访问的节点不再展开
            visited |= nxt
            frontier = nxt
            if not frontier:
                break
        visited.discard(node)
        return visited

    def max_degree(self) -> int:
        """全图最大度（用于图相关度归一化）。空图返回 0。"""
        if not self._adj:
            return 0
        return max(len(ns) for ns in self._adj.values())

    def graph_relevance(self, node: str) -> float:
        """节点的图相关度（归一化到 [0,1]），供 Memory Score 使用。

        定义（V1 简化）：degree(node) / max_degree
        - 度 = 邻居数，代表节点在图中的"连通性/活跃度"
        - 除以最大度归一化，避免图规模不同时分数不可比

        说明：
            严格说"图相关度"应针对查询实体计算（与查询有多近），
            V1 用"节点自身连通性"近似——技术债，V2 改为
            node 到 query 实体的最短路径距离（BFS 深度倒数）。
        """
        if node not in self._adj:
            return 0.0
        max_deg = self.max_degree()
        if max_deg == 0:
            return 0.0
        return len(self._adj[node]) / max_deg


if __name__ == "__main__":
    # 演示：考研决策的微型知识图谱
    g = MemoryGraph()
    g.add_edges_from_pairs([
        ("Agent项目", "考研目标"),
        ("考研目标", "南邮"),
        ("考研目标", "408"),
        ("408", "C++"),
        ("Agent项目", "分布式系统"),
    ])
    print(f"节点 {g.node_count()} 个, 边 {g.edge_count()} 条")
    print(f"'考研目标' 的直接邻居: {sorted(g.neighbors('考研目标'))}")
    print(f"'Agent项目' 的二跳可达: {sorted(g.two_hop('Agent项目', 2))}")
    for n in sorted(g._adj):
        print(f"  图相关度({n}) = {g.graph_relevance(n):.3f}")
