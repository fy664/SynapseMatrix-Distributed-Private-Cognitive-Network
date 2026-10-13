"""
SynapseMatrix — Memory Activation 层：Memory Score 综合排序（Month 1 环节 5/6）
================================================================================

定位：对应规格书 9.4「Memory Score」。

为什么需要综合打分：
    规格书场景 B（跨时间认知检索）问"我之前为什么决定学 Agent"。
    纯向量检索只比较语义相似度——但记忆还有四个维度：
        - 新近度（Recency）   : 刚记的事比陈年旧事更相关
        - 重要度（Importance） : 用户标记过/高频出现的内容更重要
        - 激活度（Activation）: 常被访问的记忆更活跃（与衰减联动）
        - 图相关度（GraphRelevance）: 与查询实体关系越近的记忆越相关

Memory Score（规格书 9.4 初始模型）：
    MemoryScore =
        a * SemanticSimilarity
      + b * Recency
      + c * Importance
      + d * Activation
      + e * GraphRelevance

设计决策：
    - 权重是超参数：规格书明确说"参数不在 V1 固定为科学真理，
      必须通过 Benchmark 调整"。因此权重通过 __init__ 注入，默认值仅为起点。
    - 各分量归一化到 [0,1] 后加权求和，保证最终分数也在 [0,1] 附近，
      可与其他系统（检索阈值、日志）直接比较。
    - 纯函数设计：compute() 不修改任何状态，可单测、可复现。

依赖：标准库 dataclasses（无第三方依赖）
"""

from __future__ import annotations

from dataclasses import dataclass, field


@dataclass(frozen=True)
class MemoryScoreWeights:
    """Memory Score 的权重配置（超参数，后续用 Benchmark 调优）。

    默认值说明（仅为合理起点，不是结论）：
        - 语义相似度 0.5：检索的主信号，占比最高
        - 新近度 0.2     ：个人记忆场景下"新旧"很重要，但不应压过语义
        - 激活度 0.15    ：访问频率反映活跃度
        - 重要度 0.1     ：用户显式重要度（V1 多为默认值）
        - 图相关度 0.05  ：V1 图谱刚起步，占比低；V2 图谱成熟后上调
    """

    semantic: float = 0.50
    recency: float = 0.20
    activation: float = 0.15
    importance: float = 0.10
    graph: float = 0.05

    def __post_init__(self) -> None:
        # 校验：权重非负且总和 > 0（避免全零导致除零/恒零分）
        weights = (self.semantic, self.recency, self.activation, self.importance, self.graph)
        if any(w < 0 for w in weights):
            raise ValueError(f"权重不能为负: {weights}")
        if sum(weights) <= 0:
            raise ValueError("权重之和必须 > 0")


@dataclass(frozen=True)
class MemoryScoreInput:
    """compute() 的输入：一条记忆在五个维度上的打分（均已归一化到 [0,1]）。

    各字段由上层模块产出：
        semantic:    向量检索的余弦相似度（retrieval 层）
        recency:     新近度归一化值，1.0=刚创建，0.0=很久以前（activation 层）
        activation:  当前激活度，经衰减/强化后（activation 层）
        importance:  重要度，1.0=非常重要（V1 可暂由来源类型/人工标记决定）
        graph:       图相关度归一化，1.0=与查询实体直接相连（graph 层）
    """

    semantic: float
    recency: float = 0.0
    activation: float = 0.0
    importance: float = 0.0
    graph: float = 0.0


@dataclass
class ScoredMemory:
    """打分结果：记忆 id + 各分量 + 最终分数。

    score: 加权综合分；components 保留各分量，便于日志与 Benchmark 分析
    （"为什么这条排第一"——可解释性是项目核心原则）。
    """

    memory_id: str
    score: float
    components: dict[str, float] = field(default_factory=dict)


def compute(
    memory_id: str,
    inp: MemoryScoreInput,
    weights: MemoryScoreWeights | None = None,
) -> ScoredMemory:
    """计算一条记忆的综合分数（纯函数，不修改状态）。

    公式：score = Σ w_i * component_i

    参数：
        memory_id: 记忆标识（如 chunk_id 字符串）
        inp      : 五维输入（各分量 [0,1]）
        weights  : 权重配置；为 None 时用默认权重

    返回：
        ScoredMemory：含总分与各分量明细

    说明：
        - 各分量必须落在 [0,1]：校验防越界，避免权重被"超范围的异常值"放大
        - 最终分数天然在 [0, 1]（凸组合），可与其他检索分数直接比较
    """
    if weights is None:
        weights = MemoryScoreWeights()

    # 校验各分量范围（与 decay.py 的防御风格一致）
    comps = {
        "semantic": inp.semantic,
        "recency": inp.recency,
        "activation": inp.activation,
        "importance": inp.importance,
        "graph": inp.graph,
    }
    for name, value in comps.items():
        if not 0.0 <= value <= 1.0:
            raise ValueError(f"{name} 必须在 [0,1]，收到 {value}")

    score = (
        weights.semantic * inp.semantic
        + weights.recency * inp.recency
        + weights.activation * inp.activation
        + weights.importance * inp.importance
        + weights.graph * inp.graph
    )

    return ScoredMemory(memory_id=memory_id, score=score, components=comps)


def rank(memories: list[ScoredMemory]) -> list[ScoredMemory]:
    """对打分结果按分数降序排序（不修改原列表）。

    参数：
        memories: compute() 产出的列表

    返回：
        新列表，按 score 降序
    """
    return sorted(memories, key=lambda m: m.score, reverse=True)


if __name__ == "__main__":
    # 演示：两条语义相似度相同，但新旧不同，看总分如何区分
    old = compute("mem_old", MemoryScoreInput(semantic=0.60, recency=0.10, activation=0.05))
    new = compute("mem_new", MemoryScoreInput(semantic=0.60, recency=0.90, activation=0.80))
    for m in rank([old, new]):
        print(f"{m.memory_id}: score={m.score:.4f} (语义={m.components['semantic']}, "
              f"新近={m.components['recency']}, 激活={m.components['activation']})")
