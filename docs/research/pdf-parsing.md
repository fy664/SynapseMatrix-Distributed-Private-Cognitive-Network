# Research Question：如何从 PDF 中可靠提取文本（Month 1 最小实验）

## Problem

Month 1 验收第一环：导入一份 PDF 后系统能够提取文本。需要回答：
- 用哪个库 / API 提取？
- 提取出的文本质量如何保证（空白、多行、中文）？
- 如何把"提取"做成可测试、可复现的模块？

## Hypothesis

PyMuPDF（fitz）的 `page.get_text("text")` 足以覆盖 V1 场景：
单文件顺序解析 + 空白折叠 + 空页过滤，即可满足"提取文本"验收，
不需要引入 OCR / 布局分析框架。

## Background

- PyMuPDF 是 MuPDF 的 Python 绑定，纯 C 内核，无需系统级依赖（Windows 下 pip 直接可用）。
- `get_text("text")` 返回按阅读顺序组织的纯文本；`"blocks"` / `"dict"` 模式带布局坐标，V1 不需要。
- 中文支持：解析 PDF 时文本由 PDF 内嵌字体的 ToUnicode CMap 决定，与生成时所用字体有关；
  生成测试 PDF 时必须用支持中文的字体（`fontname="china-s"`，MuPDF 内置 CJK 字体），
  否则默认字体不含中文字形，提取时中文丢失（本实验踩到的坑）。

## Experiment

1. 实现 `memory/parser/pdf_parser.py`：`extract_pdf_text()` 返回页级 `Page` 与整体 `ParseResult`。
2. 单元测试 11 个：多页中英混合、空页过滤、空白折叠、文件不存在、非法文件、str/Path 双入参、结构字段。
3. 端到端：把规格书 V1.0（markdown）渲染为 27 页 PDF，再解析回文本。

## Result

- `python -m pytest tests/unit -q`：**11 passed**。
- 端到端：27 页 / 27 有效页 / 1136 字符，中文（"端侧优先、P2P 协同"等）完好提取。
- 关键坑：`insert_text` 默认字体不含中文 → 中文变乱码/丢失；
  解法为 `fontname="china-s"`（对测试夹具生效，解析器本身无需处理）。

## Analysis

- 空白折叠策略：逐行 strip 后按空格 join，可消除"行尾空格+换行"产生的双空格；
  代价是丢失原始换行结构——对后续 Chunker 语义分段不利，V2 需要保留段落边界（如空行分隔）。
- 空页判定：`non_empty` 只计有文本页，`page_count` 计全页，统计口径清晰。
- 错误处理：文件不存在 → FileNotFoundError；打不开 → ValueError；
  统一由 `is_valid_pdf()` 供 ingestion 前置过滤。

## Conclusion

"PDF 文本提取"这一环已达标（可运行代码 + 单元测试 + 端到端样例）。
已知边界：本实现只提取纯文本，不处理扫描件 OCR、双栏布局、表格结构——
这些留到 L5 Normalizer / 多模态阶段。

## Next Question

提取文本之后：Chunker 如何按语义切分（而不按固定字符数）？
即 Month 1 链路的下一步：Parser → Chunker → Embedding。
