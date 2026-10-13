"""
SynapseMatrix — Memory Activation 层：时间衰减（Month 1 环节 5/6 最小实现）
============================================================================

定位：对应规格书 9.5「激活与衰减」。

为什么记忆需要衰减：
    纯向量检索只回答"语义像不像"，不回答"还该不该记着"。
    个人记忆里，同样语义的两条记忆，新的比旧的相关——
    上周的"为什么决定学 Agent"和去年的，检索时应优先给出上周的。
    衰减函数把"时间流逝"转化为"激活度下降"，让排序能体现新旧。

公式（规格书 9.5 初版）：
    activation(t) = activation_0 * exp(-lambda * dt)

    - activation_0 : 初始激活度（记忆刚创建时为 1.0）
    - lambda       : 衰减速率常数（越大忘得越快）
    - dt           : 距创建/上次强化经过的时间（秒）

    指数衰减的性质：
        - dt = 0 时 activation = activation_0（刚创建，满值）
        - dt -> ∞ 时 activation -> 0（无限久远，趋于遗忘）
        - 每经过 T = ln2 / lambda 秒，激活度减半（半衰期概念）

重新访问强化（规格书 9.5）：
    activation_new = min(1.0, activation_old + reinforcement)
    即：每次被检索/访问，激活度加上一个奖励值，但不超过 1.0——
    对应"常想起的事记得更牢"。

依赖：标准库 math（无第三方依赖）
"""

from __future__ import annotations

import math


def exponential_decay(activation_0: float, lambda_: float, dt_seconds: float) -> float:
    """指数衰减：计算经过 dt 秒后的激活度。

    参数：
        activation_0: 初始激活度（0.0 ~ 1.0）
        lambda_     : 衰减速率常数（>= 0，越大忘得越快）
        dt_seconds  : 经过的时间（秒，>= 0）

    返回：
        衰减后的激活度，范围 (0, activation_0]

    边界：
        - dt = 0   -> 返回 activation_0（时间未流逝，不变）
        - lambda=0 -> 返回 activation_0（不衰减，永不遗忘）
        - dt -> 大 -> 指数趋近 0（但不会为负）

    参数校验（防御非法输入，参照 chunker 的风格）：
        - activation_0 不在 [0, 1] -> ValueError
        - lambda_ < 0              -> ValueError
        - dt_seconds < 0           -> ValueError
    """
    if not 0.0 <= activation_0 <= 1.0:
        raise ValueError(f"activation_0 必须在 [0,1]，收到 {activation_0}")
    if lambda_ < 0:
        raise ValueError(f"lambda_ 必须 >= 0，收到 {lambda_}")
    if dt_seconds < 0:
        raise ValueError(f"dt_seconds 必须 >= 0，收到 {dt_seconds}")

    # 指数部分；math.exp(-x) 当 x 很大时趋近 0，不会溢出为负
    return activation_0 * math.exp(-lambda_ * dt_seconds)


def half_life(lambda_: float) -> float:
    """由衰减常数 lambda_ 求半衰期（激活度减半所需秒数）。

    推导：activation / activation_0 = 0.5 = exp(-lambda * T)
          => -lambda * T = ln(0.5)
          => T = ln(2) / lambda

    参数：
        lambda_: 衰减速率常数（> 0）

    返回：
        半衰期（秒）

    说明：
        lambda_ = 0 时永不衰减，半衰期为无穷——用返回 None/异常表达都不合适，
        这里按约定抛 ValueError，提示调用方这是无意义的查询。
    """
    if lambda_ <= 0:
        raise ValueError(f"lambda_ 必须 > 0 才有有限半衰期，收到 {lambda_}")
    return math.log(2) / lambda_


def reinforce(activation: float, reinforcement: float = 0.1) -> float:
    """重新访问强化：激活度加奖励，封顶 1.0。

    公式（规格书 9.5）：activation_new = min(1.0, activation_old + reinforcement)

    参数：
        activation   : 当前激活度（0.0 ~ 1.0）
        reinforcement: 每次访问的奖励增量（默认 0.1，超参数）

    返回：
        强化后的激活度（<= 1.0）

    用途：
        每次记忆被检索命中/被引用时调用，对应"常访问的记忆更活跃"。
    """
    if not 0.0 <= activation <= 1.0:
        raise ValueError(f"activation 必须在 [0,1]，收到 {activation}")
    if reinforcement < 0:
        raise ValueError(f"reinforcement 必须 >= 0，收到 {reinforcement}")
    return min(1.0, activation + reinforcement)


if __name__ == "__main__":
    # 演示：半衰期 30 天，观察 0/30/60/90 天的激活度
    import datetime

    lam = math.log(2) / (30 * 24 * 3600)  # 半衰期 30 天对应的 lambda
    now = datetime.datetime.now(datetime.timezone.utc)
    print(f"lambda_ = {lam:.3e}（半衰期 {half_life(lam)/86400:.0f} 天）")
    for days in (0, 30, 60, 90):
        dt = days * 86400
        a = exponential_decay(1.0, lam, dt)
        print(f"  {days:>3} 天后激活度: {a:.4f}")
    print(f"强化示例: 0.50 + 0.10 -> {reinforce(0.50):.2f}")
