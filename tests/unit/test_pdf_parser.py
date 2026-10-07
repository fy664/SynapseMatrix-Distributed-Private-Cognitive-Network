"""
SynapseMatrix — tests/unit/test_pdf_parser.py
=============================================
PDF Parser 的单元测试。

策略：
    - 用 PyMuPDF 在测试中动态生成 PDF（含中英文、多页、空页场景），
      保证测试可复现且不依赖外部文件。
    - 覆盖：正常多页解析、文本规范化、空页过滤、统计字段、
      文件不存在、无效 PDF、is_valid_pdf 校验。

运行：cd 项目根目录 && python -m pytest tests/unit -q
"""

from pathlib import Path

import fitz
import pytest

from memory.parser.pdf_parser import (
    Page,
    ParseResult,
    extract_pdf_text,
    is_valid_pdf,
)


# ---------------------------------------------------------------------------
# 测试辅助：动态生成 PDF
# ---------------------------------------------------------------------------

def _make_pdf(path: Path, pages: list[str]) -> None:
    """按给定页文本列表生成一个 PDF 文件。

    每页插入一段文本；空字符串表示“页内无可提取文本”。
    """
    doc = fitz.open()
    for text in pages:
        page = doc.new_page()  # 默认 A4
        if text:
            # fontname="china-s": 使用 MuPDF 内置简体中文 CJK 字体，
            # 否则 insert_text 默认字体缺中文字形，中文会被丢弃/显示为乱码
            page.insert_text((72, 72), text, fontsize=12, fontname="china-s")
    doc.save(path)
    doc.close()


# ---------------------------------------------------------------------------
# 正常解析路径
# ---------------------------------------------------------------------------

class TestExtractPdfText:
    def test_multi_page_chinese_english(self, tmp_path: Path) -> None:
        """多页 + 中英文混合：应逐页提取且页码从 1 开始。"""
        pdf = tmp_path / "mixed.pdf"
        _make_pdf(
            pdf,
            [
                "Hello SynapseMatrix, 第一页",
                "Second page content 第二页内容",
                "Third page 第三页",
            ],
        )

        result = extract_pdf_text(pdf)

        assert isinstance(result, ParseResult)
        assert result.page_count == 3
        assert result.non_empty == 3
        assert result.path == pdf
        assert [p.page_no for p in result.pages] == [1, 2, 3]
        # 文本应包含关键内容（PyMuPDF 提取时中文与英文均保留）
        assert "第一页" in result.pages[0].text
        assert "Second page content" in result.pages[1].text
        assert result.total_chars == sum(p.char_count for p in result.pages)

    def test_blank_pages_excluded_from_non_empty(self, tmp_path: Path) -> None:
        """空页：page_count 计全，non_empty 只计有文本的页。"""
        pdf = tmp_path / "blank.pdf"
        _make_pdf(pdf, ["有内容的一页", "", "又一页"])

        result = extract_pdf_text(pdf)

        assert result.page_count == 3
        assert result.non_empty == 2
        assert result.pages[1].text == ""
        assert result.pages[1].char_count == 0

    def test_normalize_whitespace(self, tmp_path: Path) -> None:
        """规范化：多行/多余空白应折叠为单个空格并去除首尾。"""
        pdf = tmp_path / "ws.pdf"
        doc = fitz.open()
        page = doc.new_page()
        # 插入三行文本，模拟多行布局（同样使用 CJK 字体保证中文可测）
        page.insert_text((72, 72), "line one", fontsize=12, fontname="china-s")
        page.insert_text((72, 92), "line two", fontsize=12, fontname="china-s")
        doc.save(pdf)
        doc.close()

        result = extract_pdf_text(pdf)
        text = result.pages[0].text

        # 不包含换行符（已被折叠为空格）
        assert "\n" not in text
        assert "line one" in text and "line two" in text

    def test_file_not_found(self) -> None:
        """不存在的文件应抛 FileNotFoundError。"""
        with pytest.raises(FileNotFoundError):
            extract_pdf_text("Z:/definitely/missing.pdf")

    def test_invalid_file(self, tmp_path: Path) -> None:
        """非 PDF 文件应抛 ValueError。"""
        bogus = tmp_path / "not_a_pdf.pdf"
        bogus.write_text("this is not a pdf", encoding="utf-8")
        with pytest.raises(ValueError):
            extract_pdf_text(bogus)

    def test_path_accepts_string_and_path(self, tmp_path: Path) -> None:
        """str 与 Path 两种入参都应可用。"""
        pdf = tmp_path / "both.pdf"
        _make_pdf(pdf, ["hello"])
        assert extract_pdf_text(str(pdf)).page_count == 1
        assert extract_pdf_text(pdf).page_count == 1


# ---------------------------------------------------------------------------
# Page / ParseResult 结构
# ---------------------------------------------------------------------------

class TestStructures:
    def test_page_char_count_derived(self) -> None:
        """Page.char_count 应等于文本长度。"""
        page = Page(page_no=1, text="abcde")
        assert page.char_count == 5

    def test_parse_result_total_chars_matches(self) -> None:
        """total_chars 应与各页 char_count 之和一致。"""
        pages = [Page(1, "hello"), Page(2, "world")]
        result = ParseResult(
            path=Path("x.pdf"),
            page_count=2,
            non_empty=2,
            total_chars=sum(p.char_count for p in pages),
            pages=pages,
        )
        assert result.total_chars == 10


# ---------------------------------------------------------------------------
# is_valid_pdf
# ---------------------------------------------------------------------------

class TestIsValidPdf:
    def test_valid_pdf_returns_true(self, tmp_path: Path) -> None:
        pdf = tmp_path / "ok.pdf"
        _make_pdf(pdf, ["valid"])
        assert is_valid_pdf(pdf) is True

    def test_missing_pdf_returns_false(self) -> None:
        assert is_valid_pdf("Z:/missing.pdf") is False

    def test_garbage_pdf_returns_false(self, tmp_path: Path) -> None:
        bogus = tmp_path / "garbage.pdf"
        bogus.write_bytes(b"%PDF-1.4 garbage not really a pdf")
        assert is_valid_pdf(bogus) is False
