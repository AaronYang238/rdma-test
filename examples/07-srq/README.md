# 示例 07 · SRQ（共享接收队列）

> 📖 对应教材：[第 12 章 · 可扩展架构](../../docs/book/part2-12-scalability.md)

把教材第 12 章做成可运行示例：服务端创建**一个 SRQ**，两个连接（QP）共享同一份
预投递的接收 WR；用 `wc.qp_num` 区分消息来源。

- **无 SRQ**：每个 QP 各自维护 RQ，接收缓冲内存 = `QP数 × N`，连接越多越浪费。
- **有 SRQ**：所有 QP 共享一份 `N` 个 WR 的接收池，内存与连接数无关。

![SRQ 多 QP 共享接收队列](../../docs/img/s4-1-srq.svg)

## 要点与陷阱

- 用 SRQ **必须手工 `rdma_create_qp`**（把 `qp_init_attr.srq` 指向 SRQ），不能用
  `rdma_create_ep` 的自动 QP；监听端 `rdma_create_ep(..., NULL)` 只用于 listen。
- 所有 QP、SRQ、MR 必须在**同一个 PD** 下（本例用 `listen_id->verbs` 自建 PD）。
- 接收缓冲用 `ibv_post_srq_recv` 投递到 SRQ，而非各 QP 的 `ibv_post_recv`。
- 绑定 SRQ 的 QP，其 `cap.max_recv_wr` 由 SRQ 决定，建 QP 时无需设置。
- **SRQ 耗尽**时所有关联 QP 都收不到消息（静默 RNR），生产中需补投递维持水位。

## 构建与运行

```bash
make
./bin/server <RDMA网卡IP> 7471                    # 终端1
./bin/client <RDMA网卡IP> 7471 client-A           # 终端2
./bin/client <RDMA网卡IP> 7471 client-B           # 终端3
```

预期：服务端打印两个不同 `QP num` 的连接，并从**同一个 SRQ** 收到 A、B 两条消息，
末尾强调"内存只用了单份 N 个 WR"。

## 关联章节

详见教材[第 12 章 · 可扩展架构](../../docs/book/part2-12-scalability.md)（SRQ、手工 QP 状态机）；连接建立见[第 5 章](../../docs/book/part1-05-connection.md)。
