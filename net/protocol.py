"""
SynapseMatrix — Net 层：同步协议 v1 帧格式（Month 2 环节 2）
===============================================================

定位：规格书 5.2「记忆同步」的传输格式。解决技术债 #19（消息无帧边界）。

问题回顾（Month 2 环节 1 踩的坑）：
    TCP 是字节流，没有消息边界。一次 recv 可能拿到：
        - 半条消息（前 512 字节）
        - 一条消息
        - 两条消息粘在一起
    回显场景靠"已知长度"收满，是特例；真实同步不知道对端发多少。

解法（长度前缀帧，所有二进制协议的共同答案）：
    [ 4 字节大端长度 ] [ payload ]
    收方流程：读 4 字节 -> 得长度 N -> 再收满 N 字节 -> 得一条完整消息。
    payload 用 UTF-8 JSON（V1 可读、可调试；V2 可换 protobuf 压缩，
    帧格式不变——长度前缀与内容编码解耦）。

为什么 4 字节大端：
    - 4 字节 = 最大 4GB，V1 记忆单条远小于此，留足余量
    - 大端（网络字节序）：TCP/IP 协议族规定跨机器数值统一用大端，
      否则不同架构（x86 小端 vs ARM 大端）读长度会错位
    - Python 用 struct.pack(">I", n) 显式大端，不依赖机器字节序

本模块提供：
    - encode_frame(payload: dict) -> bytes    消息 -> 完整帧
    - FrameDecoder                             流式解码器（应对半包/粘包）
    - 消息类型常量：SYNC / ACK / BYE
"""

from __future__ import annotations

import json
import struct

# 帧头长度：4 字节大端无符号整数
FRAME_HEADER_BYTES = 4
# 单帧 payload 上限：64KB（V1 单条记忆远小于此；防御超大帧撑爆内存）
MAX_PAYLOAD_BYTES = 64 * 1024

# 消息类型（协议 v1 枚举）
MSG_SYNC = "SYNC"       # 同步请求：携带记忆记录（增量差集）
MSG_ACK = "ACK"         # 确认：对端已应用某批记录
MSG_BYE = "BYE"         # 会话结束（可选，V1 用连接关闭代替）
MSG_IDS = "SYNC_IDS"    # 身份对账：携带一方的全部 identity 列表
MSG_RECORDS = "SYNC_RECORDS"  # 记录传输：携带单条记忆记录
MSG_ACK_STATS = "SYNC_ACK"    # 带统计的确认：added/updated/skipped 计数


def encode_frame(payload: dict) -> bytes:
    """把一条消息编码为完整帧：[4 字节大端长度][UTF-8 JSON payload]。

    参数：
        payload: 消息体（dict，序列化为 JSON）

    返回：
        完整帧字节流（含帧头）

    抛错：
        ValueError：payload 序列化后超过 MAX_PAYLOAD_BYTES
    """
    body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    if len(body) > MAX_PAYLOAD_BYTES:
        raise ValueError(
            f"payload 超限: {len(body)} > {MAX_PAYLOAD_BYTES} 字节"
        )
    # ">I" = 大端无符号 4 字节整数（网络字节序）
    header = struct.pack(">I", len(body))
    return header + body


def decode_frame_header(frame: bytes) -> tuple[int, int]:
    """解析帧头，返回 (长度, 帧头长度)。

    参数：
        frame: 至少 FRAME_HEADER_BYTES 字节的帧头

    返回：
        (payload_len, FRAME_HEADER_BYTES)

    抛错：
        ValueError：帧头不足 4 字节，或长度超限（协议损坏）
    """
    if len(frame) < FRAME_HEADER_BYTES:
        raise ValueError(
            f"帧头不足: {len(frame)} < {FRAME_HEADER_BYTES}（半包，等待更多数据）"
        )
    payload_len = struct.unpack(">I", frame[:FRAME_HEADER_BYTES])[0]
    if payload_len > MAX_PAYLOAD_BYTES:
        raise ValueError(f"帧长度超限: {payload_len} > {MAX_PAYLOAD_BYTES}（协议错乱？）")
    return payload_len, FRAME_HEADER_BYTES


class FrameDecoder:
    """流式帧解码器：应对 TCP 半包/粘包。

    用法（服务端/客户端各自维护一个实例，逐次喂 recv 结果）：
        decoder = FrameDecoder()
        for data in recv_loop():
            for msg in decoder.feed(data):
                handle(msg)   # msg 为完整消息 dict

    内部结构：
        self._buffer: bytearray 未解析完的字节流累积区
        每次 feed：把新字节追加进 buffer，循环尝试
        "读 4 字节头 -> 够长则取出整帧"，直到 buffer 不足一帧。

    为什么用字节累积而不是一次性读：TCP 一次 recv 的数据量不确定，
    必须把"还没凑够一帧"的字节存下来，等下次 recv 补全。
    """

    def __init__(self) -> None:
        self._buffer = bytearray()

    def feed(self, data: bytes) -> list[dict]:
        """喂入一段字节流，返回其中完整解码的消息列表。

        参数：
            data: recv 拿到的原始字节（任意长度，可为半包/多包）

        返回：
            完整消息 dict 列表；不足一帧时返回空列表
            （剩余字节留在内部 buffer，等下次 feed）
        """
        self._buffer.extend(data)
        messages: list[dict] = []

        while True:
            # 帧头都不够 -> 等待更多数据
            if len(self._buffer) < FRAME_HEADER_BYTES:
                break

            payload_len, _ = decode_frame_header(bytes(self._buffer))
            total_len = FRAME_HEADER_BYTES + payload_len

            # 整帧还没收齐 -> 等待更多数据
            if len(self._buffer) < total_len:
                break

            # 取出一整帧，解析 payload
            body = bytes(self._buffer[FRAME_HEADER_BYTES:total_len])
            del self._buffer[:total_len]  # 消费掉已解析部分
            messages.append(json.loads(body.decode("utf-8")))

        return messages

    def buffered_bytes(self) -> int:
        """缓冲区中未解析的字节数（供诊断/测试）。"""
        return len(self._buffer)


if __name__ == "__main__":
    # 演示：编码两帧 -> 模拟 TCP 粘包（一次喂两帧）-> 完整解码
    d = FrameDecoder()
    frames = (
        encode_frame({"type": MSG_SYNC, "record": {"text": "记忆一"}})
        + encode_frame({"type": MSG_ACK, "count": 1})
    )
    # 模拟粘包：两帧一起到达
    msgs = d.feed(frames)
    print(f"粘包一次喂入 -> 解出 {len(msgs)} 条:")
    for m in msgs:
        print(f"  {m['type']}: {m}")

    # 模拟半包：先喂一半
    d2 = FrameDecoder()
    frame = encode_frame({"type": MSG_SYNC, "record": {"text": "记忆二"}})
    half = len(frame) // 2
    print(f"半包喂入 {half} 字节 -> 解出 {len(d2.feed(frame[:half]))} 条"
          f"（剩余 {d2.buffered_bytes()} 字节在缓冲）")
    msgs2 = d2.feed(frame[half:])
    print(f"补全剩余 -> 解出 {len(msgs2)} 条: {msgs2[0]['record']['text']}")
