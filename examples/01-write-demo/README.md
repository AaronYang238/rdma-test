# 示例 01 · RDMA WRITE（单边写）

> 📖 对应教材：[第 2 章 · 30 分钟跑起来](../../docs/book/part1-02-quickstart.md) · [第 7 章 · 单边操作 WRITE/READ](../../docs/book/part1-07-write-read.md)

教程基线示例（由仓库最初的 `src/` demo 收编而来）。演示**控制面 + 数据面**的
经典组合：

1. 客户端注册一段可被远端写入的内存（`IBV_ACCESS_REMOTE_WRITE`）。
2. 客户端用 **SEND** 把该内存的 `addr + rkey + size` 告知服务端（控制面）。
3. 服务端用 **RDMA WRITE** 把数据直接写入客户端内存（数据面，客户端 CPU 无感）。
4. 服务端再 **SEND** 一个 ACK；客户端打印被覆盖后的 buffer。

端到端时序：

![本示例端到端时序](../../docs/img/08-end-to-end.svg)

## 构建与运行

```bash
make                                   # 生成 bin/server bin/client
./bin/server <RDMA网卡IP> 7471         # 终端1
./bin/client <RDMA网卡IP> 7471         # 终端2
```

预期：客户端打印的本地 buffer 内容变为服务端写入的字符串。

## 关联章节

详见教材[第 4 章 · 内存注册](../../docs/book/part1-04-memory-registration.md)、[第 6 章 · SEND/RECV](../../docs/book/part1-06-send-recv.md)、[第 7 章 · WRITE/READ](../../docs/book/part1-07-write-read.md)、[第 2 章 · 端到端时序](../../docs/book/part1-02-quickstart.md)。
