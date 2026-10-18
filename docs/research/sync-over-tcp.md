# Research Question：同步协议怎么真正过 TCP？（Month 2 环节 3）

## Problem

环节 2 完成了同步语义（帧格式/差集/冲突），但 sync_all 仍是
进程内直调两个 MemorySync——记忆没有真正"过线"。
规格书 5.2 要求设备间通过真实网络同步，要解决四个问题：
1. **线上对账**：两台设备各自持有独立记忆库，怎么先知道对方有什么
2. **过线传输**：帧怎么通过 socket 逐条发送并确认落库
3. **可靠性**：断线、粘包、半包下不丢记忆、不重复
4. **重试**：连接失败后重连，重发必须幂等

## Hypothesis

- 对账：先交换身份集合（SYNC_IDS），再按差集传输——传输量 O(差集)
- 确认：对端逐条 apply 进记忆库后回 ACK（带 added/updated/skipped
  统计）——"发出去了"不等于"对方落库了"
- 可靠：客户端循环收满 + 帧解码器应对半包/粘包；ACK 缺失即会话失败
- 重试：客户端断线重连重跑完整会话；幂等靠"已存在则 skipped"

## Background

**为什么必须"先对账再传"（线上形态的增量同步）**：
环节 2 的差集是本进程内"两个 store 直接比"。线上没有共享内存，
服务端不知道客户端有什么——必须先交换身份集合，各自在本地算差集。
7 步时序（一次会话完成双向）：
1. 客户端 -> 服务端：SYNC_IDS（客户端全部身份）
2. 服务端 -> 客户端：SYNC_IDS（服务端全部身份）
3. 客户端 -> 服务端：SYNC_RECORDS 逐帧（服务端缺的，done 标记批次结束）
4. 服务端 apply+save -> 客户端：SYNC_ACK（统计）
5. 服务端 -> 客户端：SYNC_RECORDS 逐帧（客户端缺的，按步骤 1 算差集）
6. 客户端 apply+save -> 服务端：SYNC_ACK（统计）
7. 关闭连接（BYE 由断开代替）

**ACK 的语义（本环节核心知识点）**：
TCP 只保证"字节到达网卡"，不保证"应用层已处理"。
服务端收到记录帧 -> apply 进 MemoryStore -> save 落盘 -> 才回 ACK。
这就是应用层可靠传输：确认的是"语义被应用"，不是"字节被接收"。
SYNC_ACK 带统计（added/updated/skipped），客户端据此判断哪些生效。

**幂等重试为什么安全**：
客户端断线后重连重跑完整会话（不记忆上次进度）。重发的记录
对端 apply 时若已存在 -> skipped（不产生重复）。这是应用层
"超时重传"思想的复刻：TCP 在字节层重传，应用层靠幂等容忍重发。

## Experiment

1. `net/protocol.py` 扩展消息类型：SYNC_IDS / SYNC_RECORDS / SYNC_ACK
2. `net/sync_server.py`：SyncServer.handle_connection 完整 7 步
   - 每连接一线程（ThreadPoolExecutor，测试中为单连接线程）
   - _recv_one：循环收满指定类型的一条消息
3. `net/sync_client.py`：SyncClient.sync_once / sync_with_retry
   - MAX_RETRIES=3、RETRY_DELAY=1.0，断线重连重发
4. `tests/unit/test_sync_tcp.py`：真实 socket 双向同步
   - end_to_end_union：两边并集一致
   - ack_stats：ACK 统计正确
   - idempotent_second_sync：二次同步零新增
   - retry_after_connection_failure：断线重试成功
   - protocol_type_mismatch_raises：协议错乱防御

## Result

- `python -m pytest tests/unit -q`：**161 passed**（累计 156 + 5）
- 真实进程端到端（命令行服务端 + 客户端）：A 1 条 + B 1 条 ->
  同步后各 2 条，身份集合一致，ACK 统计正确
- 三个真实 bug 被测试/调试抓出并修复：

**Bug 1：闭包引用 socket 句柄失效（WinError 10038）**
测试基建的服务端线程函数用闭包引用 srv/server，本环境实测
闭包引用的 socket 句柄会变成无效（setsockopt 报 10038）。
修复：线程回调用参数传递 socket/服务端对象，不用闭包。
（这是测试基建问题，不是协议问题；正式代码 listen() 一直
用 executor.submit 传参数，无此问题。）

**Bug 2（真正的根因）：粘包时"其余帧"被丢弃**
客户端 step4 一次 recv 可能拿到 ACK + 记录帧 + done（粘包），
FrameDecoder.feed 解出全部 3 条——但 _recv_one 若只返回第一条，
其余帧随局部变量销毁，step5 永远等不到 done -> 10 秒超时。
修复：pending 队列提升为会话级（与 decoder 同级），
recv_one 先消费 pending、不足才 recv，feed 结果 extend 进 pending。
这是本环节约 10% 概率间歇失败的真正根因——粘包是概率事件，
单跑偶发通过、高频连跑必现。

**Bug 3：feed 解出即消费的语义**
FrameDecoder.feed 解析出完整帧后从内部 buffer 删除（消费），
返回值是"本次解出的消息"。若丢弃返回值再 feed(b"") 查缓冲，
永远查不到已消费的帧——必须先缓存 feed 返回值。

## Analysis

- **粘包/半包不是理论问题，是必须实测的工程问题**：本环节的
  间歇性失败（测试时而全过、时而超时）最终定位到粘包时消息
  队列的作用域错误。FrameDecoder 正确应对了字节层粘包/半包，
  但"解出的多条消息怎么跨调用保留"是应用层的第二个坑。
- **ACK 让"同步完成"可验证**：没有统计的同步像黑盒——发完
  不知道对端加了什么。SYNC_ACK 让测试能断言 added/updated/skipped。
- **单连接线程模型是测试基建的妥协**：真服务端 accept 循环 +
  线程池，测试用一个连接就够。技术债：测试服务端线程不 join，
  靠 daemon 终止；端口用完即 close。

## Conclusion

同步协议 v1 完整闭环：语义（环节 2）+ 线上传输（本环节）。
7 步时序过线、ACK 确认落库、断线重连幂等重发、粘包/半包/闭包
三个坑全部修复，161 单测全过，真实进程双向同步演示成功。

## Next Question

线上同步通了，但"同步"只是设备间搬运记忆。下一步（Month 3）：
- 记忆衰减与遗忘：activation 随时间的指数衰减如何驱动归档/合并
- 规格书 5.3 的分布式认知：多设备记忆如何在推理时被统一召回
