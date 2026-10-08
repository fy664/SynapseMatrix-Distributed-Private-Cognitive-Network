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
