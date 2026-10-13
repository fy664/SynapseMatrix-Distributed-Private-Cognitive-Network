"""
SynapseMatrix — tests/unit/test_memory_activation.py
=====================================================
衰减函数 + Memory Score 的单元测试。

覆盖范围：
    - exponential_decay：边界、半衰期性质、单调递减、非法参数
    - half_life：换算正确性、lambda<=0 报错
    - reinforce：封顶 1.0、非法输入
    - MemoryScore：加权求和、权重校验、分量越界、rank 排序

运行：cd 项目根目录 && python -m pytest tests/unit -q
"""

import math

import pytest

from memory.activation.decay import (
    exponential_decay,
    half_life,
    reinforce,
)
from memory.activation.memory_score import (
    MemoryScoreInput,
    MemoryScoreWeights,
    ScoredMemory,
    compute,
    rank,
)


# ---------------------------------------------------------------------------
# exponential_decay
# ---------------------------------------------------------------------------

class TestExponentialDecay:
    def test_zero_dt_returns_initial(self) -> None:
        """dt=0 时激活度不变。"""
        assert exponential_decay(0.8, 0.1, 0) == pytest.approx(0.8)

    def test_zero_lambda_no_decay(self) -> None:
        """lambda=0 永不衰减。"""
        assert exponential_decay(0.9, 0.0, 1e9) == pytest.approx(0.9)

    def test_monotonic_decreasing(self) -> None:
        """随时间增大，激活度单调递减（不增）。"""
        a1 = exponential_decay(1.0, 0.01, 10)
        a2 = exponential_decay(1.0, 0.01, 100)
        a3 = exponential_decay(1.0, 0.01, 1000)
        assert a1 > a2 > a3

    def test_never_negative(self) -> None:
        """极端大的 dt 也不会衰减为负。"""
        assert exponential_decay(1.0, 1.0, 1e10) >= 0

    def test_half_life_consistency(self) -> None:
        """半衰期换算一致：经过 half_life 秒后激活度恰好减半。"""
        lam = 0.1
        t_half = half_life(lam)
        after = exponential_decay(1.0, lam, t_half)
        assert after == pytest.approx(0.5, abs=1e-9)

    def test_invalid_activation_range(self) -> None:
        with pytest.raises(ValueError):
            exponential_decay(1.5, 0.1, 10)  # 超出 [0,1]
        with pytest.raises(ValueError):
            exponential_decay(-0.1, 0.1, 10)

    def test_invalid_lambda(self) -> None:
        with pytest.raises(ValueError):
            exponential_decay(1.0, -0.1, 10)

    def test_invalid_dt(self) -> None:
        with pytest.raises(ValueError):
            exponential_decay(1.0, 0.1, -1)


# ---------------------------------------------------------------------------
# half_life
# ---------------------------------------------------------------------------

class TestHalfLife:
    def test_known_value(self) -> None:
        """ln(2)/0.693147 ≈ 1。"""
        assert half_life(0.6931471805599453) == pytest.approx(1.0, rel=1e-6)

    def test_larger_lambda_shorter_half_life(self) -> None:
        """lambda 越大，半衰期越短。"""
        assert half_life(1.0) < half_life(0.1)

    def test_nonpositive_lambda_raises(self) -> None:
        with pytest.raises(ValueError):
            half_life(0.0)
        with pytest.raises(ValueError):
            half_life(-1.0)


# ---------------------------------------------------------------------------
# reinforce
# ---------------------------------------------------------------------------

class TestReinforce:
    def test_basic_reinforce(self) -> None:
        assert reinforce(0.5, 0.2) == pytest.approx(0.7)

    def test_capped_at_one(self) -> None:
        """封顶 1.0。"""
        assert reinforce(0.95, 0.1) == pytest.approx(1.0)
        assert reinforce(1.0, 0.5) == pytest.approx(1.0)

    def test_default_reinforcement(self) -> None:
        assert reinforce(0.5) == pytest.approx(0.6)  # 默认 0.1

    def test_invalid_input(self) -> None:
        with pytest.raises(ValueError):
            reinforce(1.5, 0.1)
        with pytest.raises(ValueError):
            reinforce(0.5, -0.1)


# ---------------------------------------------------------------------------
# MemoryScore
# ---------------------------------------------------------------------------

class TestMemoryScore:
    def test_default_weights_sum(self) -> None:
        """默认权重和为 1（凸组合）。"""
        w = MemoryScoreWeights()
        assert pytest.approx(
            w.semantic + w.recency + w.activation + w.importance + w.graph
        ) == 1.0

    def test_pure_semantic_score(self) -> None:
        """只有语义分量非零时，score = w_semantic * semantic。"""
        m = compute("m1", MemoryScoreInput(semantic=0.8))
        assert m.score == pytest.approx(0.5 * 0.8)  # 默认语义权重 0.5

    def test_recency_raises_score(self) -> None:
        """语义相同、新近度更高 -> 总分更高。"""
        old = compute("old", MemoryScoreInput(semantic=0.6, recency=0.1, activation=0.05))
        new = compute("new", MemoryScoreInput(semantic=0.6, recency=0.9, activation=0.8))
        assert new.score > old.score

    def test_score_in_unit_range(self) -> None:
        """全分量满值 -> 分数为权重和 = 1.0。"""
        m = compute("m", MemoryScoreInput(1.0, 1.0, 1.0, 1.0, 1.0))
        assert m.score == pytest.approx(1.0)

    def test_zero_input_zero_score(self) -> None:
        """全零分量 -> 分数 0。"""
        m = compute("m", MemoryScoreInput(0.0, 0.0, 0.0, 0.0, 0.0))
        assert m.score == pytest.approx(0.0)

    def test_components_recorded(self) -> None:
        """components 保留各分量，供可解释性。"""
        m = compute("m", MemoryScoreInput(semantic=0.5, recency=0.3))
        assert m.components["semantic"] == 0.5
        assert m.components["recency"] == 0.3
        assert m.components["graph"] == 0.0

    def test_custom_weights(self) -> None:
        """自定义权重生效。"""
        w = MemoryScoreWeights(semantic=1.0, recency=0.0, activation=0.0, importance=0.0, graph=0.0)
        m = compute("m", MemoryScoreInput(semantic=0.7, recency=0.9), weights=w)
        assert m.score == pytest.approx(0.7)  # 只认语义，新近度权重为 0

    def test_negative_weight_rejected(self) -> None:
        with pytest.raises(ValueError):
            MemoryScoreWeights(semantic=-0.1)

    def test_all_zero_weights_rejected(self) -> None:
        with pytest.raises(ValueError):
            MemoryScoreWeights(semantic=0.0, recency=0.0, activation=0.0, importance=0.0, graph=0.0)

    def test_component_out_of_range_rejected(self) -> None:
        with pytest.raises(ValueError):
            compute("m", MemoryScoreInput(semantic=1.5))


# ---------------------------------------------------------------------------
# rank
# ---------------------------------------------------------------------------

class TestRank:
    def test_descending_order(self) -> None:
        ms = [
            compute("a", MemoryScoreInput(semantic=0.3)),
            compute("b", MemoryScoreInput(semantic=0.9)),
            compute("c", MemoryScoreInput(semantic=0.6)),
        ]
        ranked = rank(ms)
        assert [m.memory_id for m in ranked] == ["b", "c", "a"]
        scores = [m.score for m in ranked]
        assert scores == sorted(scores, reverse=True)

    def test_rank_returns_new_list(self) -> None:
        """rank 不修改原列表。"""
        ms = [
            compute("a", MemoryScoreInput(semantic=0.3)),
            compute("b", MemoryScoreInput(semantic=0.9)),
        ]
        original_order = [m.memory_id for m in ms]
        rank(ms)
        assert [m.memory_id for m in ms] == original_order
