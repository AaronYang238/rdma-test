# 示例 08 · 极简 RPC over RDMA

> 📖 对应教材：[第 15 章 · 与上层系统集成](../../docs/book/part3-15-integration.md)

把教材第 15 章做成可运行示例：一个最小的请求/响应 RPC。客户端发
`{op, seq, a, b}`，服务端计算（ADD/MUL）后回 `{seq, status, result}`。

- 先做两次演示调用（`ADD(3,4)`、`MUL(6,7)`）打印结果；
- 再压测 `iters` 次同步 RPC，报告平均每次往返延迟。

![RPC over RDMA](../../docs/img/s7-1-rpc.svg)

## 要点与陷阱

- 本示例用**双边 SEND/RECV** 承载 RPC，结构最清晰：服务端 CPU 参与每次调用。
- **响应的 recv 必须在对应请求发出前预投递**——客户端在 connect 前投递第一个，
  之后每收到一个响应就补投递下一个，形成深度 1 的接收流水线，避免 RNR。
- 协议结构体 `rpc.h` 在收发双方共享，字段定长便于直接 SEND/RECV。
- **进一步优化**（见教材[第 15 章](../../docs/book/part3-15-integration.md)）：用单边
  `WRITE_WITH_IMM` 把请求直接写入对端环形缓冲、用 IMM 立即数当通知，
  可省去一次 ACK 往返，把 RTT 压到 ~1µs。

## 构建与运行

```bash
make
./bin/server <RDMA网卡IP> 7471 [iters]    # 终端1
./bin/client <RDMA网卡IP> 7471 [iters]    # 终端2，两端 iters 须一致（默认 10000）
```

预期：客户端打印 `ADD(3,4)=7`、`MUL(6,7)=42`，以及 `iters` 次 RPC 的平均延迟；
服务端打印接受连接与完成的调用数。

## 关联章节

详见教材[第 15 章 · 与上层系统集成](../../docs/book/part3-15-integration.md)；双边操作见[第 6 章](../../docs/book/part1-06-send-recv.md)，完成机制见[第 8 章](../../docs/book/part1-08-post-poll.md)。
