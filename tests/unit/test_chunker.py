"""
SynapseMatrix — tests/unit/test_chunker.py
==========================================
Chunker 的单元测试。

覆盖范围：
    - chunk_text 纯函数：空文本、短于块长、恰等块长、超长文本、
      重叠正确性、最后一块不足块长、非法参数（size/overlap）
    - chunk_pages 批量接口：跨页编号、空页跳过、统计字段

运行：cd 项目根目录 && python -m pytest tests/unit -q
"""

import pytest

from memory.parser.chunker import Chunk, ChunkResult, chunk_pages, chunk_text
from memory.parser.pdf_parser import Page


# ---------------------------------------------------------------------------
# chunk_text：纯函数切分
# ---------------------------------------------------------------------------

class TestChunkText:
    def test_empty_text_returns_empty_list(self) -> None:
        """空文本不应产出任何 chunk。"""
        assert chunk_text("", chunk_size=10) == []

    def test_shorter_than_chunk_size_single_chunk(self) -> None:
        """文本短于块长：整体作为一个 chunk，不做无意义切分。"""
        text = "hello"
        assert chunk_text(text, chunk_size=10) == ["hello"]

    def test_exactly_chunk_size_single_chunk(self) -> None:
        """文本长度恰好等于块长：一个 chunk，内容完整。"""
        text = "a" * 10
        assert chunk_text(text, chunk_size=10) == [text]

    def test_long_text_no_overlap(self) -> None:
        """无重叠：等长切分，最后一块不足则取剩余。"""
        text = "abcdefghijklmnopqrstuvwxyz"  # 26 字符
        chunks = chunk_text(text, chunk_size=10, overlap=0)
        assert chunks == ["abcdefghij", "klmnopqrst", "uvwxyz"]  # 20+6

    def test_long_text_with_overlap(self) -> None:
        """有重叠：相邻 chunk 共享 overlap 个字符，窗口步长 = size - overlap。"""
        text = "abcdefghijklmnopqrstuvwxyz"  # 26 字符
        chunks = chunk_text(text, chunk_size=10, overlap=4)
        # step = 10 - 4 = 6
        # [0:10)  abcdefghij
        # [6:16)  ghijklmnop
        # [12:22) mnopqrstuv
        # [18:26) stuvwxyz
        assert chunks == [
            "abcdefghij",
            "ghijklmnop",
            "mnopqrstuv",
            "stuvwxyz",
        ]

    def test_overlap_is_shared_between_neighbors(self) -> None:
        """重叠正确性：chunk[i] 的尾部 == chunk[i+1] 的头部，长度恰为 overlap。"""
        text = "x" * 100
        overlap = 8
        chunks = chunk_text(text, chunk_size=20, overlap=overlap)
        for i in range(len(chunks) - 1):
            tail = chunks[i][-overlap:]          # 前一块尾部
            head = chunks[i + 1][:overlap]       # 后一块头部
            assert tail == head == "x" * overlap

    def test_chunks_are_substrings_of_source(self) -> None:
        """不变式：每个 chunk 必须是源文本的连续子串。"""
        text = "The quick brown fox jumps over the lazy dog. " * 5
        for chunk in chunk_text(text, chunk_size=17, overlap=3):
            assert chunk in text

    def test_whole_text_is_covered(self) -> None:
        """覆盖性：所有 chunk 的并集覆盖源文本全部字符（无遗漏）。"""
        text = "y" * 250
        chunk_size, overlap = 32, 8
        chunks = chunk_text(text, chunk_size, overlap)
        # 收集每个 chunk 在源文本中的位置区间
        covered: set[int] = set()
        start = 0
        step = chunk_size - overlap
        for c in chunks:
            end = start + len(c)
            covered.update(range(start, end))
            start += step
        assert covered == set(range(len(text)))

    def test_unicode_chinese_text(self) -> None:
        """中文字符按字符数切分（Python str 按码点计 len，中文一个字符=1）。"""
        text = "一二三四五六七八九十"  # 10 个中文字符
        chunks = chunk_text(text, chunk_size=4, overlap=1)
        # step = 4 - 1 = 3
        # [0:4)  一二三四
        # [3:7)  四五六七
        # [6:10) 七八九十
        assert chunks == ["一二三四", "四五六七", "七八九十"]

    def test_invalid_chunk_size(self) -> None:
        """chunk_size < 1 应报错。"""
        with pytest.raises(ValueError):
            chunk_text("abc", chunk_size=0)

    def test_invalid_overlap_negative(self) -> None:
        """overlap < 0 应报错。"""
        with pytest.raises(ValueError):
            chunk_text("abc", chunk_size=5, overlap=-1)

    def test_invalid_overlap_ge_size(self) -> None:
        """overlap >= chunk_size 应报错（否则窗口无法前进，死循环）。"""
        with pytest.raises(ValueError):
            chunk_text("abc", chunk_size=5, overlap=5)
        with pytest.raises(ValueError):
            chunk_text("abc", chunk_size=5, overlap=6)


# ---------------------------------------------------------------------------
# chunk_pages：页级批量接口
# ---------------------------------------------------------------------------

class TestChunkPages:
    def _pages(self, *texts: str) -> list[Page]:
        """按给定文本构造 Page 列表（页码从 1 开始）。"""
        return [Page(page_no=i + 1, text=t) for i, t in enumerate(texts)]

    def test_chunk_id_global_increment(self) -> None:
        """chunk_id 全局递增，不随页重置。"""
        pages = self._pages("a" * 30, "b" * 30)  # 两页，各切 2 块
        result = chunk_pages(pages, chunk_size=15, overlap=0)
        ids = [c.chunk_id for c in result.chunks]
        assert ids == [1, 2, 3, 4]
        assert ids == sorted(ids)  # 且有序

    def test_page_no_preserved(self) -> None:
        """每个 chunk 保留正确的来源页码。"""
        pages = self._pages("a" * 20, "b" * 20)
        result = chunk_pages(pages, chunk_size=10, overlap=0)
        assert [c.page_no for c in result.chunks] == [1, 1, 2, 2]

    def test_empty_page_skipped(self) -> None:
        """空文本页不产出 chunk。"""
        pages = self._pages("a" * 20, "", "b" * 20)
        result = chunk_pages(pages, chunk_size=10, overlap=0)
        assert result.chunk_count == 4
        assert all(c.page_no != 2 for c in result.chunks)

    def test_stats_fields(self) -> None:
        """统计字段：chunk_count 与 total_chars（含重叠）。"""
        pages = self._pages("a" * 20, "b" * 20)
        result = chunk_pages(pages, chunk_size=10, overlap=0)
        assert result.chunk_count == 4
        assert result.total_chars == 40  # 无重叠时 = 源文本总字符数

    def test_overlap_increases_total_chars(self) -> None:
        """有重叠时 total_chars 大于源文本字符数（重叠部分重复计数）。"""
        pages = self._pages("a" * 20)
        result = chunk_pages(pages, chunk_size=10, overlap=5)
        assert result.chunk_count == 3          # step=5: [0:10),[5:15),[10:20)
        assert result.total_chars == 30         # 源 20 字符 + 重叠 10 字符

    def test_empty_pages_list(self) -> None:
        """空页列表返回空结果。"""
        result = chunk_pages([], chunk_size=10)
        assert result.chunk_count == 0
        assert result.chunks == []
        assert result.total_chars == 0

    def test_chunk_structure(self) -> None:
        """Chunk 结构与 char_count 派生字段。"""
        pages = self._pages("hello world")
        result = chunk_pages(pages, chunk_size=100)
        c = result.chunks[0]
        assert isinstance(c, Chunk)
        assert c.text == "hello world"
        assert c.char_count == 11


# ---------------------------------------------------------------------------
# ChunkResult 结构
# ---------------------------------------------------------------------------

class TestChunkResult:
    def test_result_is_chunk_result(self) -> None:
        result = chunk_pages([Page(1, "abc")], chunk_size=5)
        assert isinstance(result, ChunkResult)
        assert result.chunk_count == 1
        assert result.total_chars == 3
