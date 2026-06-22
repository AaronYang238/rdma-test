# 示例 06 · 选择性 signaling（selective signaling）

把阶段三 3.1 的核心优化做成可运行对比：同样 N 次 RDMA WRITE，比较

- **轮 A 全 signaled**：每个 WR 都置 `IBV_SEND_SIGNALED`，每发一个就 `ibv_poll_cq`
  等一个完成 —— CQE 生成 + poll 同步开销摊在每次操作上。
- **轮 B 选择性 signaled**：每 `SIGNAL_EVERY`(=64) 个 WR 才置一次 `IBV_SEND_SIGNALED`，
  只 poll 这 1/K 的完成 —— RC 按序完成，poll 到第 K 个即证明前 K-1 个也完成，
  CQE/poll 开销摊薄到 1/K。

![选择性 signaling](../../docs/img/s3-1-selective-signaling.svg)

## 要点与陷阱

- 未 signaled 的 WR **仍占用 SQ 槽位**，直到其后某个 signaled WR 完成才被回收。
  因此在途上限 ≈ `SIGNAL_EVERY`，QP 的 `max_send_wr` 必须 ≥ 它（本例设 `SQ_DEPTH=256`）。
- 单边 WRITE 不消耗服务端 CPU，两轮差异**纯粹**来自客户端 signaling 策略。
- 直接用 `ibv_post_send` 手工设 `send_flags`，并用 `ibv_poll_cq` 轮询 `id->qp->send_cq`。
- 极端情况下若太久不 signaled，SQ 写满会导致 `ibv_post_send` 返回 `ENOMEM`——
  需周期性 signaled 回收，这正是 `SIGNAL_EVERY` 的意义。

## 构建与运行

```bash
make
./bin/server <RDMA网卡IP> 7471            # 终端1
./bin/client <RDMA网卡IP> 7471 [iters]    # 终端2，iters 默认 200000
```

预期：客户端打印 A / B 两轮的 us、Mops、ns/op 与加速比（B 通常快数倍）。
Soft-RoCE 上绝对数值偏慢，但 A/B 相对加速比依然清晰可见。

## 关联章节

`docs/stage3-performance.md` 3.1；完成机制见 `CLAUDE.md` 第 7 节。
