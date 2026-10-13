"""
SynapseMatrix — tests/unit/test_graph.py
========================================
MemoryGraph（最小图谱）的单元测试。

覆盖范围：
    - add_node / add_edge / 幂等性 / 非法输入
    - neighbors（一阶）、two_hop（BFS 受限深度）
    - node_count / edge_count / max_degree
    - graph_relevance 归一化与空图边界

运行：cd 项目根目录 && python -m pytest tests/unit -q
"""

import pytest

from memory.graph.graph import MemoryGraph


def _build_demo() -> MemoryGraph:
    """构造考研决策微型图谱：
        Agent项目 - 考研目标 - 南邮
                         |
                        408 - C++
        Agent项目 - 分布式系统
    """
    g = MemoryGraph()
    g.add_edges_from_pairs([
        ("Agent项目", "考研目标"),
        ("考研目标", "南邮"),
        ("考研目标", "408"),
        ("408", "C++"),
        ("Agent项目", "分布式系统"),
    ])
    return g


# ---------------------------------------------------------------------------
# 构建
# ---------------------------------------------------------------------------

class TestBuild:
    def test_add_node_isolated(self) -> None:
        g = MemoryGraph()
        g.add_node("孤立节点")
        assert "孤立节点" in g
        assert g.node_count() == 1
        assert g.neighbors("孤立节点") == set()

    def test_add_edge_undirected(self) -> None:
        """无向：a-b 加边后双向可达。"""
        g = MemoryGraph()
        g.add_edge("a", "b")
        assert "b" in g.neighbors("a")
        assert "a" in g.neighbors("b")

    def test_add_edge_idempotent(self) -> None:
        """重复加同一条边无副作用（边数不变）。"""
        g = MemoryGraph()
        g.add_edge("a", "b")
        g.add_edge("a", "b")
        assert g.edge_count() == 1

    def test_invalid_edge(self) -> None:
        g = MemoryGraph()
        with pytest.raises(ValueError):
            g.add_edge("", "b")
        with pytest.raises(ValueError):
            g.add_edge("a", "a")  # 自环


# ---------------------------------------------------------------------------
# 查询
# ---------------------------------------------------------------------------

class TestQuery:
    def test_neighbors_direct(self) -> None:
        g = _build_demo()
        assert g.neighbors("考研目标") == {"Agent项目", "南邮", "408"}

    def test_neighbors_unknown_node_empty(self) -> None:
        g = _build_demo()
        assert g.neighbors("不存在的节点") == set()

    def test_two_hop_depth_one_equals_neighbors(self) -> None:
        g = _build_demo()
        assert g.two_hop("考研目标", 1) == {"Agent项目", "南邮", "408"}

    def test_two_hop_depth_two(self) -> None:
        g = _build_demo()
        # 从考研目标出发 2 跳：Agent项目、南邮、408 + 它们的邻居中未访问的
        # Agent项目 -> 分布式系统; 408 -> C++
        assert g.two_hop("考研目标", 2) == {"Agent项目", "南邮", "408", "分布式系统", "C++"}

    def test_two_hop_excludes_start(self) -> None:
        """结果不含起点自身。"""
        g = _build_demo()
        assert "考研目标" not in g.two_hop("考研目标", 2)

    def test_two_hop_unknown_node_empty(self) -> None:
        g = _build_demo()
        assert g.two_hop("不存在", 2) == set()

    def test_invalid_depth(self) -> None:
        g = _build_demo()
        with pytest.raises(ValueError):
            g.two_hop("考研目标", 0)


# ---------------------------------------------------------------------------
# 统计与归一化
# ---------------------------------------------------------------------------

class TestStats:
    def test_counts(self) -> None:
        g = _build_demo()
        assert g.node_count() == 6          # Agent项目/考研目标/南邮/408/C++/分布式系统
        assert g.edge_count() == 5

    def test_max_degree(self) -> None:
        g = _build_demo()
        assert g.max_degree() == 3          # 考研目标 有 3 个邻居

    def test_empty_graph_max_degree_zero(self) -> None:
        assert MemoryGraph().max_degree() == 0

    def test_graph_relevance_normalized(self) -> None:
        g = _build_demo()
        # 考研目标度=3 -> 3/3=1.0；南邮度=1 -> 1/3≈0.333
        assert g.graph_relevance("考研目标") == pytest.approx(1.0)
        assert g.graph_relevance("南邮") == pytest.approx(1 / 3)

    def test_graph_relevance_unknown_node_zero(self) -> None:
        g = _build_demo()
        assert g.graph_relevance("不存在") == 0.0

    def test_graph_relevance_single_node(self) -> None:
        """只有孤立节点时：度 0，相关度 0（避免除零）。"""
        g = MemoryGraph()
        g.add_node("孤立")
        assert g.graph_relevance("孤立") == 0.0
