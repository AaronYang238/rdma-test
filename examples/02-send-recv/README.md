# 示例 02 · SEND/RECV 双边乒乓（延迟基准）

> 📖 对应教材：[第 6 章 · 双边操作 SEND/RECV](../../docs/book/part1-06-send-recv.md)

纯双边操作：客户端发 ping，服务端 echo 回 pong，循环 N 次并测量平均往返延迟。

要点：
- **接收方必须先 `post_recv`**，否则对端 SEND 到达时触发 RNR。两端都在循环内
  为「下一发」预投递接收。
- 用 `IBV_SEND_SIGNALED` + `wait_send_comp` 保证每发都被确认（最朴素、未优化）。
- 输出 avg RTT 与单向延迟估计，作为第 11 章性能优化的对照基线。

![SEND/RECV 乒乓与计时](../../docs/img/ex02-pingpong.svg)

## 构建与运行

```bash
make
./bin/server <RDMA网卡IP> 7471 10000   # 终端1：监听并 echo 10000 次
./bin/client <RDMA网卡IP> 7471 10000   # 终端2：发起并计时
```

客户端示例输出：

```
[client] 10000 round-trips in 21500.0 us
[client] avg RTT = 2.15 us, one-way ~= 1.07 us (msg 64 B)
```

## 关联章节

详见教材[第 6 章 · SEND/RECV](../../docs/book/part1-06-send-recv.md)、[第 8 章 · post/poll](../../docs/book/part1-08-post-poll.md)、[第 11 章 · 性能工程（基准方法）](../../docs/book/part2-11-performance.md)。
