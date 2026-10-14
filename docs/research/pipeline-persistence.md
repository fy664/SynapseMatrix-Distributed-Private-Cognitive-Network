# Research Question：五环怎么串成可复用管道？（Month 1 环节 6/6）

## Problem

前五环节各自可用但彼此孤立：Parser、Chunker、Embedding、Retrieval、
Memory Score 都是散落的命令行调用。每次使用都要
"重新解析 → 重新切分 → 重新 fit → 重新建索引"，
且进程一退出，所有中间状态全部丢失（VectorStore 索引在内存里，
TF-IDF 词典在内存里）。

核心问题两个：
1. **编排**：谁来统一"PDF 进 → 记忆出"的调用顺序与状态管理？
2. **持久化**：重启后如何免于全量重跑？

## Hypothesis

- 编排层只负责"串"，不负责"实现"：调度顺序 + 状态加载保存 + 结果报告，
  算法全部委托给既有模块——分层思想（上层编排、下层实现）
- 持久化拆两层：**模型**（向量化器词典/IDF，定义向量空间）与**数据**
  （记忆记录），分开存文件
- 幂等语义：同源文档二次导入直接跳过（不产生重复记忆）；
  文档更新后 force=True 全量重建

## Background

**为什么模型和数据必须分开存**：
向量空间 = 词典 + IDF + 文档数（TF-IDF 的 fit 产物）。
如果不存词典，下次 encode 的向量落在另一个空间，检索必然失配。
模型是"空间的定义"，数据是"空间的填充"——这个区分在 V2 换 BGE 时
价值最大：换模型只换 embedder.json，memory.json 原样可用
（BGE 稠密向量的表示不同，但接口层允许后端切换，这是 Embedder 抽象的意义）。

**为什么 JSON 而不是数据库**（V1 取舍）：
向量是 {词: 权重} 稀疏 dict，JSON 天然可序列化；零依赖、单文件、
中文可读可 diff。代价是全量读写——记忆量万级以后才成为瓶颈，
届时换 SQLite/向量库，接口不动。

## Experiment

新增三个模块：
1. `memory/store/memory_store.py`：MemoryStore JSON 记忆库
   - add_record 分配全局递增 id（重启续号）
   - has_source / delete_by_source（幂等判断与重建）
   - save / load 往返 + schema 版本校验
2. `memory/embedding/embedder.py` 扩展：save / load / is_fitted
   （词典 + IDF + 文档数落盘；load 时校验 type=tfidf）
3. `memory/pipeline.py`：MemoryPipeline 编排入口
   - ingest_pdf：加载/创建库 → 幂等检查 → 模型加载/训练 →
     解析 → 切分 → 向量化 → 入库 → 双落盘
   - search：加载库+模型 → 重建索引 → 检索 → Memory Score 综合排序
     （use_memory_score=False 可退回纯相似度，作对照实验）
   - rebuild_index：从持久化记录重建内存索引（解决技术债 #9）

单元测试 21 个：store 往返/续号/schema 校验、pipeline 幂等/force 重建/
重启后直接检索/排序降序/无库报错/多文档 id 不冲突。

## Result

- `python -m pytest tests/unit -q`：**129 passed**（累计 108 + 21）
- 端到端验证（27 页规格书样例）：
  - 首次导入：页 27、chunk +27，记忆库与模型双落盘
  - 二次导入同源：skipped=True（幂等）
  - 模拟重启（全新 pipeline 对象，零解析）：4 个查询全部命中
    - "这个项目的定位是什么" → p1 项目定位段（综合 0.548）
    - "隐私保护" → p2 定位句（综合 0.619，相似度 0.438 全场最高）
    - "分布式协同" → p2（综合 0.584）
    - "记忆衰减" → 记忆衰减相关章节（综合 0.516）

## Analysis

- **已知边界（技术债）**：
  1. 不增量更新 IDF：新文档导入后，词典外的词直接忽略
     ——V2 换 BGE 后彻底消除（BGE 预训练词典固定）
  2. search 中 activation 暂用 recency 近似（激活度应有独立访问计数）
  3. importance 无来源（全默认 0.5）、graph 分量未接入（chunk 级实体抽取未做）
  4. JSON 全量读写，万级 chunk 后变慢（V2 换 SQLite/向量库）
  5. 无删除/更新单条记忆（沿用 VectorStore 限制，force 只支持整源重建）
- 分层设计的收益兑现：pipeline 不 import 任何算法细节，
  只依赖各模块公开接口；V2 换 BGE 或换向量库时 pipeline 主体不变

## Conclusion

Month 1 收官：Local Cognitive Core 六环节全部落地
（Parser → Chunker → Embedding → Retrieval → Memory Score → Pipeline），
129 单测全过，端到端验证"导入一次、重启可查"。
这条链路 = 规格书 L2 的最小可用版本，也是 Month 2 网络层的基础设施。

## Next Question

记忆在单机内已经闭环（解析 → 检索 → 排序 → 持久化）。
Month 2 起进入网络：两台设备如何同步各自的记忆库？
"P2P 协同 + 隐私保护"从规格书第 5 章开始——先搞懂 TCP/UDP、
序列化、去重合并，再动手写同步协议。
