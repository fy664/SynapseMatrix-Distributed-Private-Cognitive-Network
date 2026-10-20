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
  用数据调权重。**2026-10-20 进展**：Vector RAG vs Synapse Memory 基准
  已落地（`benchmarks/memory/run_benchmark.py`，12 问 / Recall / Precision /
  MRR / Top-1），结论：无时间差异时两套等价；新旧冲突时 Synapse Memory
  明确偏向新记忆（Top-1 新记忆 9/12 vs 4/12）。下一步用同一基准网格调权
  重。
- **Priority**：P2

---

## 15. Importance 分量无数据来源

- **Issue**：Memory Score 的 importance 分量在 V1 无输入，全为默认值 0.5。
- **Impact**：该维度当前不起区分作用（P3）。
- **Temporary Solution**：V1 接受；接口已预留该字段。
- **Proper Solution**：V2 由来源类型（用户笔记>网页>聊天）、访问频率、
  用户显式标记共同驱动 importance。
- **Priority**：P3

---

## 16. Embedding 词典不增量更新（新词丢失）

- **Issue**：pipeline 导入新文档时，向量化模型沿用旧词典（fit 一次），
  新文档中出现的新词在 encode 时被忽略。
- **Impact**：新文档的专属词汇无向量权重，检索召回下降（P2）。
- **Temporary Solution**：V1 接受；force 重建时全量重新 fit。
- **Proper Solution**：V2 换 BGE 等预训练语义模型——词典固定、OOV 天然解决；
  若保留 TF-IDF，则实现增量 IDF 更新（需记录每词的 df 计数）。
- **Priority**：P2

---

## 17. search 的 activation 分量用 recency 近似

- **Issue**：pipeline.search 组装 Memory Score 时，
  activation 直接取 recency 值，没有独立的访问计数。
- **Impact**：激活度不反映真实访问频率——常访问的记忆与刚创建的记忆
  无法区分（P2）。
- **Temporary Solution**：V1 接受；接口已预留。
- **Proper Solution**：V2 在 MemoryRecord 增加访问计数器 access_count，
  检索命中时 reinforce()（衰减 + 强化闭环，规格书 9.5）。
- **Priority**：P2

---

## 18. JSON 全量读写，记忆量大后变慢

- **Issue**：MemoryStore 每次 save/load 读写整个 JSON 文件。
- **Impact**：万级 chunk 后落盘/加载延迟明显（P2，性能问题）。
- **Temporary Solution**：V1 接受（当前 <100 chunk）。
- **Proper Solution**：V2 换 SQLite（向量存 BLOB 或稀疏表示）
  或专用向量库（qdrant/pgvector，规格书 L5 技术栈）。
- **Priority**：P2

---

## 19. 消息无帧边界（粘包未解）

- **Issue**：echo 链路 recv 循环以"已知长度"收满，是回显场景特例；
  真实同步消息长度未知，一次 recv 无法确定一条消息的边界。
- **Impact**：多消息连续发送时无法区分边界——同步协议不可实现（P1）。
- **Temporary Solution**：V1 echo 场景用 expected=len(payload) 收满。
- **Proper Solution**：V2 同步协议引入"长度前缀"帧格式
  （4 字节大端长度 + payload），recv 先读 4 字节得长度，再收满。
- **Priority**：P1

---

## 20. 无 send_all 封装

- **Issue**：send 不保证一次写完（大消息可能部分发送），
  echo_server 靠 sendall（Python 内置封装）规避；C++ 版需自行实现。
- **Impact**：C++ 端侧实现时若直接 send 会丢数据（P1，正确性）。
- **Temporary Solution**：Python 用 socket.sendall。
- **Proper Solution**：同步协议环节封装 send_all（循环 send 直至全部写完，
  检查返回值累加偏移）。
- **Priority**：P1

---

## 21. 线程模型为 V1 妥协

- **Issue**：echo_server 用 ThreadPoolExecutor 每连接一线程。
- **Impact**：连接数上升时线程切换/GIL 开销明显；C++ 端不适合（P2）。
- **Temporary Solution**：V1 接受（个人端侧并发 <8）。
- **Proper Solution**：V2 换 selectors 事件循环；规格书 L4 端侧 C++
  用 epoll（非阻塞 + 事件驱动）。
- **Priority**：P2

---

## 22. 同步仍在本进程内对账，未接真实 socket

- **Issue**：`MemorySync.sync_all` 直接读对方 store 对象，
  没有走 TCP + 帧传输。
- **Impact**：跨机器同步不可用——当前仅验证语义（P1，功能缺口）。
- **Temporary Solution**：V1 进程内直调（语义正确）。
- **Proper Solution**：下一环节接入——SYNC 帧过线、对端 apply 后回
  ACK、断线重试（离线补收，规格书 5.2）。
- **Priority**：P1

---

## 23. 冲突仅后写者胜，时间戳相同时静默

- **Issue**：同身份不同文本且 created_at 完全相等时，
  _sync_direction 走 skipped（保留 target），无告警。
- **Impact**：真正的双写冲突可能静默丢一方（P2，正确性风险）。
- **Temporary Solution**：V1 接受（created_at 精确到微秒，概率极低）。
- **Proper Solution**：V2 引入向量合并策略（规格书 5.2）
  或至少记录冲突日志供人工裁决。
- **Priority**：P2

---

## 24. 身份用 (source, chunk_id) 复合键，非 UUID

- **Issue**：跨设备身份依赖 source 字符串（文档名）+ 源内编号，
  文档重命名后身份漂移；两台设备对同一文档的导入路径不同时
  source 不一致。
- **Impact**：同一批记忆在不同设备可能被识别为不同身份（P2）。
- **Temporary Solution**：V1 接受（个人场景 source 约定一致）。
- **Proper Solution**：V2 每条记忆分配 UUID（创建时生成），
  identity 直接为 UUID，与来源解耦。
- **Priority**：P2

---

## 25. created_at 兼作"更新时间"，语义不纯

- **Issue**：冲突 updated 时改写 created_at——"创建时间"与
  "更新时间"混淆。
- **Impact**：跨设备时间语义不一致，后续排序/衰减计算可能偏差（P3）。
- **Temporary Solution**：V1 接受（更新场景少）。
- **Proper Solution**：V2 增加 updated_at 字段，created_at 只写一次。
- **Priority**：P3

---

## 26. 测试基建：单连接服务端线程不 join、端口用完即弃

- **Issue**：test_sync_tcp 的 _start_server 每测试起一个
  daemon 线程 + 随机端口，线程不 join（靠 daemon 终止），
  socket 用完即 close。
- **Impact**：测试间无显式同步；本环境曾出现闭包引用 socket
  句柄失效（WinError 10038）导致间歇失败（P2，测试可靠性）。
- **Temporary Solution**：线程回调用参数传 socket 对象（不用闭包）；
  每测试独立 tmp_path 与随机端口。
- **Proper Solution**：V2 用 pytest fixture 提供"生命周期可控的
  服务端"（start -> yield -> stop 并 join）；或改用 asyncio/多进程
  隔离网络测试。
- **Priority**：P2

---

## 27. ACK 等待无超时（服务端）

- **Issue**：服务端 handle_connection 在等客户端 ACK 时
  recv 无超时，客户端若静默消失，服务端线程永久阻塞。
- **Impact**：僵尸线程堆积（P2，资源泄漏）。
- **Temporary Solution**：V1 依赖客户端超时后主动断开，
  服务端 recv 到 b'' 抛 ConnectionError 退出。
- **Proper Solution**：V2 服务端 recv 设置 socket 超时
  （如 30s），超时即判定会话失败并关闭连接。
- **Priority**：P2

---

## 28. recv 读块 64KB 与 MAX_PAYLOAD 的关系未显式对齐

- **Issue**：_recv_one 每次 recv(64*1024)，与协议
  MAX_PAYLOAD_BYTES=64*1024 数值相等但无常量关联。
- **Impact**：若未来调大 MAX_PAYLOAD，recv 块小于单帧
  时仍正确（FrameDecoder 半包累积），但数值耦合不显式（P3）。
- **Temporary Solution**：V1 两者相等，无实际影响。
- **Proper Solution**：定义 READ_CHUNK_BYTES = MAX_PAYLOAD_BYTES
  并注释关联；或 recv 用更大读块减少系统调用。
- **Priority**：P3

---

## 29. Benchmark ground truth 用关键词定位，非人工标注

- **Issue**：`run_benchmark.py` 的 ground truth 是"chunk 文本包含
  概念词即相关"（如 'Memory Score'），不是人工逐条标注相关性。
- **Impact**：Recall/Precision 绝对数值偏乐观（关键词命中天然
  与 TF-IDF 检索同源），相对对比（RAG vs Synapse）仍有效（P2）。
- **Temporary Solution**：V1 接受；12 问全命中，结论聚焦时间
  维度的相对差异。
- **Proper Solution**：V2 引入人工标注的相关性集合（或 LLM 辅助
  标注 + 人工抽验），并对同义改写做查询扩充测试；语料换成真实
  个人记忆分布（聊天/笔记/网页），而非规格书本身。
- **Priority**：P2

---

## 30. spec_full.pdf 语料是规格书转印，非自然排版

- **Issue**：Benchmark 语料由完整规格书 MD 直接转 PDF
  （`benchmarks/memory/spec_full.pdf`，66 页，每页 45 行等宽文本），
  不是真实文档排版（无标题层级/表格/图片/多栏）。
- **Impact**：chunk 分布与真实 PDF 不同（页内文本密度均匀），
  检索难度的代表性有限（P3）。
- **Temporary Solution**：V1 接受；规格书概念密度高，关键词命中
  充分，足够支撑"时间感知"对比。
- **Proper Solution**：V2 用真实多主题语料（混合笔记/网页/论文），
  或至少混合多个来源 PDF。
- **Priority**：P3
