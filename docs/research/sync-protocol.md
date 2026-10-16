# Research Question：两台设备怎么同步记忆？（Month 2 环节 2）

## Problem

环节 1 打通了"跨进程传字节"（TCP 回显），但字节不是记忆。
规格书 5.2 要求两台设备同步记忆库，要解决三个问题：
1. **边界**：一条记忆在哪结束（TCP 是字节流，无消息边界）
2. **增量**：两台设备各有一半记忆，怎么只传差额不传全量
3. **冲突**：同一记忆两边内容不同，以谁为准

## Hypothesis

- 边界：长度前缀帧（4 字节大端长度 + UTF-8 JSON payload）
- 增量：差集传输——对账时只发"对方缺的或比对方新的"
- 冲突：后写者胜（last-write-wins，按 created_at 比较）——
  V1 记忆只增不改，真实冲突概率低，复杂合并留 V2

## Background

**长度前缀帧为什么是通用答案**：
TCP 保证字节有序到达，但一次 recv 拿多少不确定。
收方流程：读 4 字节 -> 得长度 N -> 再收满 N 字节 -> 一条完整消息。
HTTP/2、WebSocket、protobuf 流都用同一思路（帧头 + 载荷）。
帧头用大端（网络字节序）：TCP/IP 族规定跨机器数值统一大端，
否则 x86（小端）与 ARM（大端）读长度会错位。

**增量同步的本质**：
全量同步 = 每次传整个记忆库，O(全部)。增量 = 对账后只传差集，
传输量 O(差额)。对账成本 O(n)（身份集合比较），传输量 O(差集)。
这是 git push 的模型：本地已有对象不重复传。

**跨设备身份（本环节最大的坑）**：
两台设备各自的 chunk_id 都从 1 开始——A 的 #1 和 B 的 #1
不是同一条记忆！本地 id 只在单机内唯一。
V1 方案：身份 = (source, source_chunk_id)。
- MemoryRecord 增加 source_chunk_id 字段：本地创建为 None
  （身份回退 chunk_id），同步应用时保留远端编号
- 陷阱实证：add_record 自动分配本地 id，若直接把远端 chunk_id
  覆盖成本地 id，身份就丢了，二次同步必然错乱
- 教训：**本地索引键 ≠ 跨设备身份键**，必须显式分离

**冲突处理的完整语义**：
只比对"身份缺失"会漏掉更新——同身份不同内容也必须进入差集。
对账规则（missing_in 的最终形态）：
1. 对端无此身份        -> 发送（added）
2. 内容相同            -> 不发（幂等）
3. 内容不同且本方较新  -> 发送（覆盖对端，updated）
4. 内容不同且对端较新  -> 不发（不回退）

## Experiment

1. `net/protocol.py`：长度前缀帧
   - encode_frame：4 字节大端长度 + JSON payload
   - FrameDecoder：流式解码器（半包/粘包/帧头被拆均正确）
2. `net/sync.py`：同步语义
   - SyncRecord：跨设备记录载体（identity = source + source_chunk_id）
   - MemorySync：missing_in（完整对账规则）/ apply_record（added/
     updated/skipped）/ _sync_direction（单方向）/ sync_all（双向）
3. `memory/store/memory_store.py` 扩展：
   - MemoryRecord.source_chunk_id + identity 属性
   - add_remote_record（保留远端身份插入）
   - update_record 改按身份匹配

## Result

- `python -m pytest tests/unit -q`：**156 passed**（累计 135 + 21）
- 端到端演示：A 3 条 + B 2 条 -> 同步后各 5 条，身份集合一致；
  b_to_a 方向对 A 已持有的 3 条对账为 skipped（不重复传输）
- 关键 bug 被测试抓出并修复：
  - 远端身份被本地 id 覆盖（身份丢失）
  - 差集只算身份缺失，冲突更新永不传输（后写者胜失效）

## Analysis

- **已知边界（技术债）**：
  1. 身份用 (source, chunk_id) 复合键而非 UUID——同一文档在两台
     设备导入时身份一致（合理），但不同来源的同号记录依赖 source
     区分；V2 建议直接 UUID（技术债 #24）
  2. 冲突策略只有后写者胜：时间戳完全相同时不处理（文本不同但
     created_at 相等 -> 保持 target 版本，无告警）
  3. 同步仍在本进程内对账，真实 socket 传输（帧 + 循环收满 +
     ACK）是下一环节的接入点
  4. updated 会改写 created_at——严格说应区分"创建时间"与
     "更新时间"，V2 引入 updated_at
- 大端序测试验证了长度 >255 时小端解析会错位——字节序不是理论，
  是必须显式处理的工程决策

## Conclusion

同步协议 v1 语义层完成：帧格式解粘包、差集传输解增量、
后写者胜解冲突、跨设备身份解 id 冲突。156 单测全过。
下一步把语义接到真实 socket：SYNC 帧过线、ACK 确认、断线重传。

## Next Question

语义已验证，但传输还是进程内直调。下一环节：
- 把 sync 会话接到 TCP（服务端/客户端各跑一个 MemorySync）
- 帧 + 循环收满 + ACK 重试（断线不丢记忆）
- 规格书 5.2 的"离线同步"：设备 A 改的，B 下次上线补收
