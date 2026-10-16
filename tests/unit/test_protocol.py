"""
SynapseMatrix — tests/unit/test_protocol.py
===========================================
同步协议 v1 帧格式的单元测试（Month 2 环节 2）。

覆盖范围：
    - 帧编码：长度前缀正确性（大端序）
    - 单帧解码往返
    - 粘包：一次 feed 两帧 -> 解出两条
    - 半包：分两次 feed -> 第二次解出
    - 超限帧拒绝
    - 中文 payload 往返

运行：cd 项目根目录 && python -m pytest tests/unit -q
"""

import struct

import pytest

from net.protocol import (
    FRAME_HEADER_BYTES,
    MAX_PAYLOAD_BYTES,
    MSG_ACK,
    MSG_SYNC,
    FrameDecoder,
    decode_frame_header,
    encode_frame,
)


class TestEncodeFrame:
    def test_header_is_big_endian_length(self) -> None:
        """帧头 4 字节 = payload 长度，且为大端序。"""
        payload = {"type": MSG_SYNC, "text": "记忆"}
        frame = encode_frame(payload)
        assert len(frame) >= FRAME_HEADER_BYTES

        header = frame[:FRAME_HEADER_BYTES]
        # 大端解析出长度 == payload 字节数
        payload_len = struct.unpack(">I", header)[0]
        assert payload_len == len(frame) - FRAME_HEADER_BYTES

    def test_little_endian_would_differ(self) -> None:
        """验证大端序的关键性：长度 > 255 时小端解析结果不同。"""
        long_text = "长文本" * 100  # 300 字符 -> 900 字节 UTF-8
        frame = encode_frame({"type": MSG_SYNC, "text": long_text})
        payload_len = struct.unpack(">I", frame[:4])[0]
        # 900 > 255：若误用 '<'（小端）会解析出不同数值
        assert payload_len != struct.unpack("<I", frame[:4])[0]

    def test_round_trip(self) -> None:
        payload = {"type": MSG_ACK, "count": 3, "note": "中文也OK"}
        decoder = FrameDecoder()
        msgs = decoder.feed(encode_frame(payload))
        assert len(msgs) == 1
        assert msgs[0] == payload

    def test_oversized_payload_rejected(self) -> None:
        with pytest.raises(ValueError, match="超限"):
            encode_frame({"big": "x" * (MAX_PAYLOAD_BYTES + 1)})

    def test_header_decode_oversized(self) -> None:
        """帧头声明长度超限 -> 拒绝（协议错乱防御）。"""
        bogus = struct.pack(">I", MAX_PAYLOAD_BYTES + 1)
        with pytest.raises(ValueError, match="超限"):
            decode_frame_header(bogus)


class TestFrameDecoder:
    def test_sticky_packets(self) -> None:
        """粘包：两帧一次 feed -> 解出两条（TCP 常见场景）。"""
        d = FrameDecoder()
        frames = encode_frame({"type": MSG_SYNC, "n": 1}) + encode_frame(
            {"type": MSG_ACK, "n": 2}
        )
        msgs = d.feed(frames)
        assert [m["n"] for m in msgs] == [1, 2]
        assert d.buffered_bytes() == 0

    def test_half_packet(self) -> None:
        """半包：第一次只喂一半 -> 0 条；补全 -> 1 条。"""
        d = FrameDecoder()
        frame = encode_frame({"type": MSG_SYNC, "text": "半包测试"})
        half = len(frame) // 2

        assert d.feed(frame[:half]) == []
        assert d.buffered_bytes() == half

        msgs = d.feed(frame[half:])
        assert len(msgs) == 1
        assert msgs[0]["text"] == "半包测试"
        assert d.buffered_bytes() == 0

    def test_split_in_header(self) -> None:
        """极端半包：帧头本身被拆成两半。"""
        d = FrameDecoder()
        frame = encode_frame({"type": MSG_SYNC, "text": "x"})
        # 只喂 2 字节（帧头 4 字节的一半）
        assert d.feed(frame[:2]) == []
        # 补剩余全部
        msgs = d.feed(frame[2:])
        assert len(msgs) == 1

    def test_mixed_chunks(self) -> None:
        """任意切分喂入 -> 解码结果与整帧一致（流式健壮性）。"""
        frames = b"".join(
            encode_frame({"type": MSG_SYNC, "i": i, "text": f"第{i}条"})
            for i in range(5)
        )
        d = FrameDecoder()
        all_msgs: list[dict] = []
        # 随机切分喂入（逐字节）
        for b in frames:
            all_msgs.extend(d.feed(bytes([b])))
        assert len(all_msgs) == 5
        assert [m["i"] for m in all_msgs] == [0, 1, 2, 3, 4]
        assert all(m["text"].startswith("第") for m in all_msgs)
