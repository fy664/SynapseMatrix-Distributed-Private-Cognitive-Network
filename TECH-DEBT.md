# TECH-DEBT 记录

发现问题时不糊过去，记录 Issue → Impact → 临时方案 → 正解 → 优先级。
优先级：P0 阻塞系统 / P1 严重影响正确性 / P2 性能与可维护性 / P3 优化。

---

## 1. Chunker 按页独立切分，不跨页

- **Issue**：`chunk_pages` 对每页文本单独切分，跨页连续段落会在页边界被切断。
- **Impact**：跨页的完整语义单元（如跨页表格、长段落）被拆到两个 chunk，
  检索时可能两头都搜不全（P2，影响检索质量而非正确性）。
- **Temporary Solution**：V1 接受该行为；页内重叠窗口缓解页内切断，
  页边界切断暂不处理。
- **Proper Solution**：V2 改为按全文连续切分，chunk 记录 [start_page, end_page]
  页码区间；或先做"页边界处尝试向下一页延伸至段落/句子结束"的结构感知切分。
- **Priority**：P2

---

## 2. Chunker 无垃圾 chunk 过滤

- **Issue**：纯空白/纯标点/超短无效块也会产出 chunk。
- **Impact**：浪费 embedding 计算与存储，污染检索结果（P3）。
- **Temporary Solution**：parser 已过滤空页；空文本页不产出 chunk。
- **Proper Solution**：在 chunk 产出后按"有效字符占比"过滤，或合并到
  Normalizer 阶段统一做内容质量清洗。
- **Priority**：P3

---

## 3. PDF 文本提取不做布局/OCR

- **Issue**：`get_text("text")` 只取纯文本，扫描件（无文本层）提取为空，
  双栏/表格布局提取顺序可能错乱。
- **Impact**：扫描版 PDF 无法进入 memory 链路；复杂布局文档提取质量差（P2）。
- **Temporary Solution**：V1 只支持带文本层的 PDF；`is_valid_pdf` 前置过滤。
- **Proper Solution**：L5 多模态阶段引入 OCR（PaddleOCR）与布局分析
  （规格书 11.3）。
- **Priority**：P2

---

## 4. MSVC 中文注释编译需 /utf-8

- **Issue**：本机 MSVC 默认代码页 936（GBK），UTF-8 中文注释会报
  C4819 并可能引发语法错误。
- **Impact**：命令行编译 C++ 代码时容易踩坑（P3，工程环境问题）。
- **Temporary Solution**：编译参数加 `/utf-8`。
- **Proper Solution**：项目 CMake 配置统一加 `/utf-8`（Month 3 C++ Runtime
  建 CMake 时落实）。
- **Priority**：P3

---

## 5. LeetCode 编译产物曾误提交

- **Issue**：`test_two_sum.obj` 被 git add 误提交。
- **Impact**：仓库混入构建产物（已修复，P3）。
- **Temporary Solution**：已 `git rm --cached` 并在 .gitignore 加入
  `*.obj / *.exe / *.o / *.a`。
- **Proper Solution**：提交前 `git status` 自查 + pre-commit 钩子过滤构建产物。
- **Priority**：P3

---

## 6. TF-IDF 无停用词过滤

- **Issue**：分词只过滤标点，"的/了/是"等高频虚词仍进入词表。
- **Impact**：虚词在所有文档都高频出现，会拉高无关文本的相似度，污染检索结果（P2）。
- **Temporary Solution**：V1 接受；IDF 已天然压低高频词权重（出现在全部文档的词 IDF=1）。
- **Proper Solution**：引入中文停用词表（如哈工大停用词表）在 tokenize 后过滤。
- **Priority**：P2

---

## 7. TF-IDF 无语义理解 + OOV 词忽略

- **Issue**：词面重合模型，"苹果"与"水果"相似度为 0；fit 后出现的新词直接忽略。
- **Impact**：同义改写类查询检索不到（P1 影响检索正确性）；新词丢失信息（P2）。
- **Temporary Solution**：V1 接受，作为词面基线。
- **Proper Solution**：V2 换语义 embedding（BGE 系列），稠密向量天然处理同义词；
  新词做增量词表更新或直接由模型 OOV 处理。
- **Priority**：P1

---

## 8. 稀疏向量 dict 表示

- **Issue**：向量用 {词: 权重} dict 存储，检索需全量线性扫描。
- **Impact**：记忆量达万级 chunk 后检索变慢（P2，性能问题）。
- **Temporary Solution**：V1 文本量小（<100 chunk），线性扫描足够。
- **Proper Solution**：V2 换稠密向量 + ANN 索引（如 faiss / hnswlib），
  或稀疏倒排索引。
- **Priority**：P2

---

## 9. VectorStore 索引只存内存，无持久化

- **Issue**：`memory/retrieval/vector_store.py` 的索引是进程内列表，退出即丢失。
- **Impact**：重启后需重新解析 + 向量化全部文档（P2，数据不持久）。
- **Temporary Solution**：V1 接受；每次使用先跑完整 pipeline。
- **Proper Solution**：V2 接向量数据库（如 sqlite-vec / qdrant / pgvector），
  或自定义二进制持久化（chunk_id + 向量 + 元数据）。
- **Priority**：P2

---

## 10. VectorStore 无删除/更新能力

- **Issue**：索引只有 add/search，无 remove/update。
- **Impact**：文档变更后旧 chunk 无法移除，会产生过期记忆（P2，正确性问题）。
- **Temporary Solution**：V1 接受；全量重建索引规避。
- **Proper Solution**：V2 按 chunk_id 支持删除/覆盖，配合 memory versioning
  （规格书 16.2）做增量更新。
- **Priority**：P2

---

## 11. 检索无多字段过滤（时间/来源）

- **Issue**：search 只有相似度一个维度，无法按时间范围/来源文档过滤。
- **Impact**：跨时间认知检索（规格书场景 B）无法实现——"去年的决定"无法限定时间。
- **Temporary Solution**：V1 接受；检索后在上层做时间过滤（先取相似再筛）。
- **Proper Solution**：V2 在 VectorStore 增加 metadata 过滤条件
  （created_at 范围、source_type 等），或由 Memory Score 综合排序
  （规格书 9.4：Semantic + Recency + Importance + Activation + GraphRelevance）。
- **Priority**：P1

---

## 12. graph_relevance 用节点度近似，非查询距离

- **Issue**：`MemoryGraph.graph_relevance` 用 degree(node)/max_degree 近似，
  严格说图相关度应计算"节点到查询实体的距离"。
- **Impact**：图相关度不感知查询——同一节点对不同 query 相关度相同（P2）。
- **Temporary Solution**：V1 接受；作为图维度的占位信号。
- **Proper Solution**：V2 改为 BFS 最短路径倒数（1/distance），
  查询实体确定后逐节点计算。
- **Priority**：P2

---

## 13. 图无关系类型/属性/权重

- **Issue**：`MemoryGraph` 只有无向边，规格书 10.2 的 RELATION 类型
  （MENTIONS/PART_OF/DERIVED_FROM）、节点属性、边权重均未实现。
- **Impact**："DERIVED_FROM（源于）"这类语义关系无法表达，图谱表达力受限（P2）。
- **Temporary Solution**：V1 统一按"有边"处理；关系类型留待 V2。
- **Proper Solution**：V2 升级为有向带权图 + 关系类型枚举，
  或直接引入 Neo4j（规格书 L4 Memory Fabric 技术栈）。
- **Priority**：P2

---

## 14. Memory Score 权重未 Benchmark

- **Issue**：默认权重（语义0.5/新近0.2/激活0.15/重要0.1/图0.05）是拍脑袋起点。
- **Impact**：排序质量未验证，可能劣于纯相似度（P2，正确性待证）。
- **Temporary Solution**：V1 接受；权重经构造注入可随时调整。
- **Proper Solution**：规格书 20.2 Memory Benchmark（Recall/Precision/
  Retrieval Latency）对比 Naive RAG / Vector RAG / Graph RAG / Synapse Memory，
  用数据调权重。
- **Priority**：P2

---

## 15. Importance 分量无数据来源

- **Issue**：Memory Score 的 importance 分量在 V1 无输入，全为默认值 0.5。
- **Impact**：该维度当前不起区分作用（P3）。
- **Temporary Solution**：V1 接受；接口已预留该字段。
- **Proper Solution**：V2 由来源类型（用户笔记>网页>聊天）、访问频率、
  用户显式标记共同驱动 importance。
- **Priority**：P3
