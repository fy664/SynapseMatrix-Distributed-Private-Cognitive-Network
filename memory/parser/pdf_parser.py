"""
SynapseMatrix — Memory Parser 层：PDF 文本提取（Month 1 最小实现）
=================================================================

定位：L5 AI Pipeline 的入口环节之一，对应规格书第 11 章 Pipeline 的
"Input -> Parser -> Normalizer -> ..." 中的 Parser。

本模块职责（V1 范围）：
    1. 读取本地 PDF 文件
    2. 逐页提取文本
    3. 对文本做基础规范化（去除多余空白、过滤纯空页）
    4. 输出结构化的 Page 对象，供后续 Chunker / Embedding 使用

设计约束（对应规格书 0.2 核心原则）：
    - 先正确，后性能：V1 只做单文件顺序解析，不做并发
    - 先手写最小实现：直接使用 PyMuPDF 基础 API，不引入框架
    - 可观测：返回每页字符数、页数等元信息，供 telemetry 使用

依赖：PyMuPDF (fitz) >= 1.24
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

import fitz  # PyMuPDF：封装 MuPDF 的 Python 绑定


@dataclass(frozen=True)
class Page:
    """PDF 中一页的解析结果。

    属性说明：
        page_no   : 页码，从 1 开始计数（与 PDF 阅读器一致）
        text      : 该页提取出的原始文本（已去除首尾空白）
        char_count: 该页有效字符数（len(text)），供统计与阈值判断
    """

    page_no: int
    text: str
    char_count: int = field(init=False)

    def __post_init__(self) -> None:
        # frozen dataclass 下用 object.__setattr__ 给派生字段赋值
        object.__setattr__(self, "char_count", len(self.text))


@dataclass
class ParseResult:
    """一次 PDF 解析的整体结果。

    属性说明：
        path         : 源文件路径
        page_count   : 文档总页数（含空页）
        non_empty    : 有效页（提取到非空白文本的页）数量
        total_chars  : 全部页字符数之和
        pages        : Page 对象列表（保持页码顺序）
    """

    path: Path
    page_count: int
    non_empty: int
    total_chars: int
    pages: list[Page]


def extract_pdf_text(pdf_path: str | Path) -> ParseResult:
    """从本地 PDF 提取逐页文本（最小实现）。

    参数：
        pdf_path: PDF 文件路径（str 或 Path）

    返回：
        ParseResult：包含页级文本与统计信息

    异常：
        FileNotFoundError: 文件不存在
        ValueError       : 文件不是有效 PDF / 无法被 MuPDF 打开
    """
    path = Path(pdf_path)

    # 1) 前置检查：文件必须存在且是文件
    if not path.is_file():
        raise FileNotFoundError(f"PDF 文件不存在: {path}")

    # 2) 打开文档；fitz 打开失败会抛 RuntimeError，这里统一转成 ValueError
    try:
        doc = fitz.open(path)  # 支持 .pdf；其他扩展名也会尝试解析
    except Exception as exc:  # noqa: BLE001 - 统一转为业务异常
        raise ValueError(f"无法打开 PDF（可能不是有效 PDF）: {path}") from exc

    pages: list[Page] = []
    total_chars = 0
    non_empty = 0

    # 3) 逐页提取文本并规范化
    try:
        for page_index in range(doc.page_count):
            page = doc.load_page(page_index)  # 0 基页码
            raw = page.get_text("text")       # 提取纯文本（布局模式 text）
            # 基础规范化：折叠连续空白为单个空格，去除首尾空白
            normalized = _normalize_text(raw)
            if normalized:
                non_empty += 1
                total_chars += len(normalized)
            pages.append(Page(page_no=page_index + 1, text=normalized))
    finally:
        doc.close()  # 确保资源释放（RAII 思想的 Python 对应物）

    return ParseResult(
        path=path,
        page_count=len(pages),
        non_empty=non_empty,
        total_chars=total_chars,
        pages=pages,
    )


def _normalize_text(raw: str) -> str:
    """文本基础规范化。

    - 将连续的换行/制表符/空格折叠为单个空格
    - 去除首尾空白
    - 保留中英文与标点原样（V1 不做语言处理）

    这是后续 Normalizer 阶段的简化占位，V2 将在此扩展
    （段落重组、标题识别、去页眉页脚等）。
    """
    if not raw:
        return ""
    # 按行拆分后逐行 strip，再重新 join，避免“行内尾部空格 + 换行”产生双空格
    lines = [line.strip() for line in raw.splitlines()]
    joined = " ".join(line for line in lines if line)
    return joined.strip()


def is_valid_pdf(pdf_path: str | Path) -> bool:
    """轻量校验：文件存在且可被 MuPDF 打开（用于 ingestion 前置过滤）。"""
    try:
        extract_pdf_text(pdf_path)
        return True
    except (FileNotFoundError, ValueError):
        return False


if __name__ == "__main__":
    # 命令行直接运行示例：python pdf_parser.py <path-to-pdf>
    import sys

    if len(sys.argv) != 2:
        print("用法: python pdf_parser.py <path-to-pdf>")
        sys.exit(1)
    result = extract_pdf_text(sys.argv[1])
    print(f"文件: {result.path}")
    print(f"总页数: {result.page_count}, 有效页: {result.non_empty}, 总字符: {result.total_chars}")
    for p in result.pages:
        print(f"--- 第 {p.page_no} 页 ({p.char_count} 字符) ---")
        print(p.text[:200])
