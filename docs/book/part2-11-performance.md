# 第 11 章 · 性能工程

> 第 10 章把数据路径的真实代价摊开了：MMIO doorbell、PCIe 往返、CQE 的 DMA
> 写、NUMA 距离……每一项都在花时间。本章就是把那些「代价」逐个变成「优化
> 手段」——目标是把延迟做到 μs 级、把带宽打满线速，并且**每一项优化都说清
> 背后的硬件机理**，而不是「调一调试试看」。
>
> 前置阅读：第 10 章（硬件模型）；建议先跑过 `examples/02-send-recv/`
> 拿到自己的延迟基线，再看本章的优化能把它压到多低。

---

## 本章你将遇到的术语（预览）

每个术语先给一句话直觉，完整定义在正文展开；章末另有复习速查表。

| 术语 | 一句话直觉 |
|------|-----------|
| **SQ / RQ** | 一个 QP 的两条队列：发送队列、接收队列。 |
| **CQ / CQE** | 完成队列和它里面的条目——poll 它来确认操作干完了。 |
| **WR / WQE** | 你提交的「工作请求」和它在硬件队列里的存储形态。 |
| **SGE** | 一个 `{addr, length, lkey}` 三元组，一个 WR 可挂多个，自动拼接多段内存。 |
| **Doorbell** | 通知网卡「有新活」的那次 MMIO 写——批处理能把多次合成一次。 |
| **IMM** | 立即数，随 SEND/WRITE_WITH_IMM 捎带的 32 位通知。 |
| **Credit** | 流控信用：确保对端 RQ 里预投递了足够的接收 WR。 |
| **NUMA** | 非一致内存访问——QP/MR/线程最好都绑在同一节点。 |

---

## 引子：从「能跑」到「跑得快」

入门篇的示例每发一个 WR 就 poll 一次完成，清晰、好懂，但每次操作都背着一份
「生成 CQE + poll 同步」的固定开销。当你追求每秒上百万次操作时，这份固定
开销会变成主要瓶颈。本章的六节优化，本质都在做同一件事：**把第 10 章里那些
按次计费的硬件代价（CQE DMA 写、doorbell、PCIe 读、跨 NUMA 访问）摊薄、合并
或消除掉**。我们从收益最直接的「选择性 signaling」开始。

---

## 1. 选择性 signaling 与 SQ 回收

> 🛠 可运行示例：[examples/06-selective-signaling/](../../examples/06-selective-signaling/)
> ——同样 N 次 WRITE，对比「全 signaled」与「每 K 个 signaled」的吞吐加速比。

**问题**：每个 WR 都打 `IBV_SEND_SIGNALED`、每发一个就 poll 一个完成，逻辑最
直白。但既然 RC 的 SQ 是严格有序完成的，为什么还要为每一个 WR 都确认一遍？

**类比**：想象你在工厂流水线末端验货。「全 signaled」是每下线一件就停下来
签一次字；「选择性 signaling」是攒够 64 件、只签最后一件的字——因为流水线
是顺序出货的，最后一件签了，就证明前 63 件也都下线了。少签 63 次字，吞吐
自然上来了。

### 原理

每个 `IBV_SEND_SIGNALED` 的 WR 完成后，网卡都要通过 PCIe DMA 写一条 CQE。
高吞吐场景下，如果每个 WR 都打 SIGNALED，CQ 的 DMA 写和 CPU 的 `ibv_poll_cq`
本身就会占用大量 PCIe 带宽和 CPU 周期。

**选择性 signaling**：每 N 个 WR 只让最后一个带 `IBV_SEND_SIGNALED`，其余为
unsignaled。一次 `poll_cq` 就能确认前 N 个 WR 全部完成——因为 RC 的 SQ 是
严格有序的。CQE/poll 开销直接摊薄到 1/N。

![选择性 signaling](../img/s3-1-selective-signaling.svg)

### 代码模式

```c
#define SIGNAL_INTERVAL 16          /* 每 16 个 WR signaled 一次 */
static int outstanding = 0;         /* 未 poll 的 unsignaled WR 数 */

static void post_write_selective(struct rdma_cm_id *id, /* ... */)
{
    int flags = (++outstanding % SIGNAL_INTERVAL == 0)
                ? IBV_SEND_SIGNALED : 0;

    /* ibv_post_send ... flags ... */

    if (flags & IBV_SEND_SIGNALED) {
        wait_send_comp(id, "selective signal");
        outstanding = 0;
    }
}

/* 退出前 flush 剩余 unsignaled WR */
static void flush_outstanding(struct rdma_cm_id *id, int remain)
{
    if (remain > 0) {
        /* post 一个 no-op / 0 字节 WRITE 带 SIGNALED，然后 poll */
        /* 或直接 post 最后一个真实 WR 带 SIGNALED */
        wait_send_comp(id, "flush");
    }
}
```

### 关键约束（陷阱）

- 教学默认 `sq_sig_all = 1`（`common/rdma_common.h:fill_qp_attr`）——生产环境
  改为 `0`，手动控制 `IBV_SEND_SIGNALED`。
- 未 signaled 的 WR **仍占用 SQ 槽位**，直到其后某个 signaled WR 完成才被回收。
  因此在途上限 ≈ `SIGNAL_INTERVAL`，QP 的 `max_send_wr` 必须 **≥** 它，否则 SQ
  在 poll 之前就满了，`ibv_post_send` 会返回 `ENOMEM`。这正是要**周期性
  signaled 回收**的原因（示例 06 设 `SQ_DEPTH=256`、`SIGNAL_EVERY=64`）。
- unsignaled WR 出错时**不会**产生错误 CQE；需靠后续 signaled WR 的 `wc.status`
  间接发现（此时 QP 已进入 ERROR 态，所有后续 WR 以 flush error 完成）。
- 退出前别忘了 **flush 剩余 unsignaled WR**，否则最后不足一个区间的 WR 可能
  还没确认完成程序就退了。

示例 06 的实测：单边 WRITE 不消耗服务端 CPU，A/B 两轮差异**纯粹**来自客户端的
signaling 策略，B 轮通常快数倍；即便在 Soft-RoCE 上绝对值偏慢，相对加速比依然
清晰可见。

---

## 2. 轮询 vs 事件

**问题**：怎么知道一个操作完成了？最简单是写个死循环不停 poll，但那会把一个
CPU 核烧到 100%。可如果改成「睡觉等通知」，延迟又会变差。到底该怎么选？

**类比**：等快递有两种姿势。**busy-poll** 是站在门口一直盯着——最快知道，但
你什么别的事都干不了（独占 CPU）。**事件驱动**是回屋睡觉、等门铃响——省力，
但从按铃到你起身开门多了一截延迟。**混合策略**则是先在门口盯一会儿，没动静
再回屋睡——兼顾两者。

### 三种模式对比

![完成通知模式对比](../img/s3-2-poll-vs-event.svg)

#### Busy-Poll（最低延迟）

```c
struct ibv_wc wc;
while (ibv_poll_cq(cq, 1, &wc) == 0)
    ;   /* 空转 */
```

- **优点**：消除事件通知延迟，P50 可达亚微秒级。
- **缺点**：独占一个 CPU 核，功耗高，不适合 QPS 低的场景。

#### 事件驱动（省 CPU）

```c
/* 注册 completion channel */
struct ibv_comp_channel *cc = ibv_create_comp_channel(ctx);
ibv_req_notify_cq(cq, 0);          /* arm：下一个 CQE 触发事件 */

/* 线程阻塞等待 */
struct ibv_cq *ev_cq; void *ev_ctx;
ibv_get_cq_event(cc, &ev_cq, &ev_ctx);
ibv_ack_cq_events(ev_cq, 1);       /* 必须 ack，否则引用计数泄漏 */
ibv_req_notify_cq(ev_cq, 0);       /* 重新 arm */
ibv_poll_cq(ev_cq, batch, wcs);    /* 取出所有 CQE */
```

这里 **arm** 指 `ibv_req_notify_cq`——它的语义是「下一个 CQE 到来时给我发一个
事件」。注意两个易错点：`ibv_ack_cq_events` **必须调用**，否则 CQ 的引用计数
泄漏、销毁时卡住；每次取完事件要**重新 arm**，因为 arm 是一次性的。

- **优点**：线程可睡眠，适合高连接数 / 低 QPS。
- **缺点**：事件路径多一次内核交互，延迟增加 1–5 μs。

#### 混合策略（生产首选）

```c
/* 先 busy-poll N 次，无 CQE 则切换为事件模式 */
int spin = 0;
while (ibv_poll_cq(cq, 1, &wc) == 0) {
    if (++spin > SPIN_THRESHOLD) {
        ibv_req_notify_cq(cq, 0);
        /* 再 poll 一次防 race */
        if (ibv_poll_cq(cq, 1, &wc) == 0) {
            ibv_get_cq_event(cc, &ev_cq, &ev_ctx);
            ibv_ack_cq_events(ev_cq, 1);
        }
        spin = 0;
    }
}
```

### 一定要讲透的陷阱：arm 之后必须再 poll 一次

为什么 arm 完不能直接去睡（`ibv_get_cq_event`），而要中间再 poll 一次？

因为存在这样一个**竞态窗口**：CQE 恰好在你调用 `ibv_req_notify_cq`（arm）**之后**、
调用 `ibv_get_cq_event`（睡眠）**之前**就到达了。arm 只对「arm 之后到来的
CQE」负责，这条「卡在缝里」的 CQE 不会再触发新事件——于是你的线程在
`ibv_get_cq_event` 上**永久睡眠**，CQE 永远没人取。

补救办法就是 arm 之后**立刻再 poll 一次**：如果那条 CQE 已经躺在 CQ 里，这一
poll 直接把它取走，根本不去睡；只有确认 CQ 真空了，才安全地进入事件等待。
这是事件驱动模式里最经典、也最容易漏掉的一步。

---

## 3. Inline data 与小消息优化

**问题**：发一条 64 字节的控制消息，网卡为什么还要专门发起一次 PCIe DMA 去
另一块 buffer 取这点数据？能不能把这么小的数据「随 WQE 一起捎过去」？

**类比**：寄一张小纸条，普通做法是「单子上写明纸条放在哪个柜子」，快递员还得
专门跑一趟柜子取（一次 DMA 往返）。inline 则是「直接把纸条贴在单子上」——
快递员读单子就拿到了内容，省掉那趟取件。

### 原理

普通 `ibv_post_send` 在 WQE 中存储 `(addr, lkey, length)`，网卡执行时再发起
PCIe DMA 去读数据 buffer。对于小消息，这次额外的 DMA 往返成本显著。

`IBV_SEND_INLINE` 把数据直接**内嵌进 WQE**，网卡读 WQE 即得数据，**省去一次
PCIe 往返**，通常可降低延迟 20–50 ns。

![Inline data 省去 DMA 读](../img/s3-3-inline.svg)

### 使用方法

```c
/* 创建 QP 时声明最大 inline 容量（受硬件限制） */
qp_attr.cap.max_inline_data = 236;   /* mlx5 典型上限 */

/* post_send 时加 IBV_SEND_INLINE 标志（不需要 lkey） */
struct ibv_send_wr wr = { ... };
wr.send_flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE;
```

注意 `rdma_post_send` 这个便捷封装没有直接的 inline 参数，需要回退到
`ibv_post_send` 手工构造 WR。

### 调优建议（陷阱）

```bash
# 查询设备支持的最大 inline 大小
ibv_devinfo -v | grep max_inline
```

- 超过 `max_inline_data` 的 WR 设置 `IBV_SEND_INLINE` 会返回 `EINVAL`——所以
  上限要先查清楚再用。
- 控制面消息（64–128 B，本仓库 `control_message` = 80 B）是 inline 的理想候选。
- 大数据面传输（>1 KB）收益极小，不必 inline——数据本身的 DMA 才是大头，
  省那一次往返微不足道。

---

## 4. 批处理、链式 WR 与 SGE 聚合

**问题**：第 1 节摊薄了 CQE 开销，但每次 `ibv_post_send` 还各自敲一次 doorbell
（一次 MMIO 写，约 50–100 ns）。要发 1000 个 WR，难道就得敲 1000 次门铃？

**类比**：寄 10 个包裹，与其按 10 次呼叫按钮叫快递员来 10 趟，不如把 10 个
包裹串成一摞、按一次按钮让他一次取走。**链式 WR** 就是「把多个 WR 串成一摞，
一次 doorbell 全投递」；**SGE 聚合**则是「一个包裹里塞进散落各处的几样东西，
快递员打包时自动拼好」。

### 批处理与链式 WR

每次 `ibv_post_send` 都会触发一次 doorbell MMIO 写（WC flush），代价约
50–100 ns。将多个 `ibv_send_wr` 通过 `wr.next` 链成链表，**一次 `ibv_post_send`
调用**即可投递整个链，只需一次 doorbell——doorbell 开销直接除以 batch 大小。

![批处理与链式 WR](../img/s3-4-batching.svg)

```c
struct ibv_send_wr wr[BATCH], *bad;
for (int i = 0; i < BATCH - 1; i++) {
    build_wr(&wr[i], ...);
    wr[i].send_flags = 0;           /* unsignaled */
    wr[i].next = &wr[i + 1];
}
build_wr(&wr[BATCH-1], ...);
wr[BATCH-1].send_flags = IBV_SEND_SIGNALED;
wr[BATCH-1].next = NULL;

ibv_post_send(qp, &wr[0], &bad);   /* 一次调用，一次 doorbell */
wait_send_comp(...);
```

注意这里和第 1 节的选择性 signaling **天然配合**：链中间的 WR 全部
unsignaled，只有链尾带 `IBV_SEND_SIGNALED`，且链尾 `next = NULL`。两个细节
缺一不可——忘了置 `NULL` 会让网卡顺着野指针往下读。

### SGE 聚合（Scatter-Gather）

单个 WR 可携带 `sge[]` 数组，指向多段**不连续内存**，网卡在 DMA 时自动拼接。
`max_send_sge` 决定每个 WR 最多几个 SGE（通常 ≥ 16）。

```c
struct ibv_sge sge[2] = {
    { .addr = (uint64_t)hdr,  .length = hdr_len,  .lkey = hdr_mr->lkey  },
    { .addr = (uint64_t)body, .length = body_len, .lkey = body_mr->lkey },
};
wr.sg_list = sge;
wr.num_sge = 2;
```

这避免了为「拼包」而做的内存拷贝（比如把 header 和 body 复制到一段连续
buffer 再发），是 zero-copy RPC 框架的常用技巧。

---

## 5. 多 QP / 多核扩展与 NUMA 亲和

**问题**：前面的优化都在压单 QP 的每次操作开销。可单 QP 本质是串行的——前一个
WR 没完成就排队。要榨干一张 100Gb 网卡，靠一个 QP、一个核够吗？

**类比**：单 QP 像一条单车道——再优化红绿灯，车流上限也就那样。要提吞吐得
**修多条并行车道**（多 QP + 多核），而且每条车道最好都修在「离网卡近的那个
城区」（同一 NUMA 节点），否则车每次过桥（跨 NUMA）都要多花几十纳秒过路费。

### 单 QP 的瓶颈

单 QP 本质上是串行的——前序 WR 完成才能继续。高吞吐需要多 QP 并行，
每个工作线程独享一组 QP + CQ，彻底消除锁竞争。

![多 QP / 多核与 NUMA 亲和](../img/s3-5-multi-qp.svg)

### NUMA 亲和三原则

回忆第 10 章：跨 NUMA 的 doorbell 写会额外增加 30–50 ns，是 P99 抖动的常见
根源。要消除它，三件事都要对齐到 RNIC 所在的那个 NUMA 节点：

1. **找到 RNIC 所在 NUMA 节点**：`cat /sys/bus/pci/devices/<BDF>/numa_node`
2. **数据 buffer 在同节点分配**：`numa_alloc_onnode()` 或
   `numactl --membind=<node>`
3. **工作线程绑定到同节点 CPU**：`taskset -c <core>` 或 `pthread_setaffinity_np`

### 多 QP 设计模式

```
模式 A：每线程独享 QP + CQ
    线程 0 → QP₀ + CQ₀   (无锁，最快)
    线程 1 → QP₁ + CQ₁

模式 B：共享 CQ + 各自 QP
    线程 0/1 → QP₀/QP₁ → 共享 CQ
    poll_cq 需加锁（或用原子 CAS）

模式 C（阶段四）：SRQ + 共享 CQ
    大量连接共享一个 RQ，节省内存
```

选型直觉：**生产推荐模式 A**（无锁、最快）；当连接数 > CPU 核数、再给每个
连接独享 QP+CQ 太浪费时，才考虑模式 B/C（用一点锁竞争或共享换取内存与
扩展性）。模式 C 的 SRQ 留到阶段四 / 后续章节细讲。

---

## 6. 基准方法学

**问题**：你做了一堆优化，怎么证明它真的快了、又快到了哪？「我跑了一次感觉
变快了」不是工程结论。

**类比**：测性能像称体重——只称一次说明不了什么，要在**固定时间、固定条件**
下多次测量看分布。而且你得先有一把**校准过的秤**（perftest）当参照，才知道
自己的程序离硬件极限还有多远。

### perftest 校准流程

`perftest` 是社区标准的 RDMA 基准工具，把它的数字当作「这台硬件能达到的
上限参照」：

```bash
# 安装
dnf install -y perftest

# RDMA WRITE 带宽基线（双机）
ib_write_bw -d mlx5_0 -i 1              # server
ib_write_bw -d mlx5_0 -i 1 <server-ip> # client

# RDMA WRITE 延迟基线
ib_write_lat -d mlx5_0 -i 1
ib_write_lat -d mlx5_0 -i 1 <server-ip>

# SEND 延迟
ib_send_lat ...

# 关键参数
ib_write_bw ... -s 65536 -n 10000      # 消息大小 64 KB，10000 次迭代
ib_write_lat ... --output=percentiles   # 输出百分位数
```

![基准方法学与指标模板](../img/s3-6-benchmark.svg)

### 自研程序对比 checklist

把 `examples/02-send-recv` 的 RTT 数字与 `ib_send_lat` 对比，差距大时逐项排查
（每一项都对应本章或第 10 章讲过的某个机理）：

| 检查项 | 命令 / 方法 |
|--------|------------|
| 是否跨 NUMA | `numactl --hardware` + `lstopo` |
| SQ 深度是否够 | `ibv_devinfo \| grep max_qp_wr` |
| 中断亲和性 | `/proc/irq/*/smp_affinity` |
| CPU 调速器 | `cpupower frequency-info` → 改 performance |
| 重传计数器 | `/sys/class/infiniband/mlx5_0/ports/1/counters/port_rcv_errors` |
| PCIe 带宽饱和 | `pcm-pcie` 或 `nvidia-smi dmon` |

### 延迟解读：别只看平均值

```
P50  ≈ 理想路径延迟（无抖动）
P99  ≈ 抖动上限
P99 > 3 × P50  → 存在明显抖动，检查：
    - CQ 轮询不及时（缺 busy-poll 或批量 poll 不够大）
    - 重传（rnr_retry 触发、congestion）
    - NUMA 跨节点访问
    - OS jitter（NMI、timer、kthread）
```

一句话：报告性能要给 **P50 / P99 / 带宽** 三件套，单次测量或只报平均值都
是不可复现、会骗人的。

---

## 小结：五段式（原理 → API → 代码 → 性能 → 陷阱）

| 节 | 原理 | 核心 API / 参数 | 代码示例 | 性能收益 | 常见陷阱 |
|----|------|----------------|---------|---------|---------|
| 11.1 | 减少 CQE DMA 写次数 | `IBV_SEND_SIGNALED` · `sq_sig_all=0` | `fill_qp_attr` · `examples/06` | QPS 提升 N 倍 | flush 剩余 unsignaled WR |
| 11.2 | CPU 占用 vs 延迟折中 | `ibv_req_notify_cq` · `ibv_get_cq_event` | — | 延迟 vs 省电 | arm 后忘记再 poll 一次 |
| 11.3 | 省去小消息 DMA 读 | `IBV_SEND_INLINE` · `max_inline_data` | — | −20–50 ns | 超 limit 返回 EINVAL |
| 11.4 | 减少 doorbell 次数 | `wr.next` 链式 · `ibv_post_send(list)` | — | doorbell 开销 ÷ batch | 链尾 `next=NULL` 且 SIGNALED |
| 11.5 | 消除锁 + NUMA 延迟 | `taskset` · `numa_alloc_onnode` | — | P99 抖动减少 30–50 ns | buffer 跨 NUMA 分配 |
| 11.6 | 建立可重复的度量基线 | `ib_write_bw/lat` · `--output=percentiles` | `02-send-recv` | — | 单次测量 vs 统计百分位 |

---

## 术语速查

> 完整术语表见 [`docs/glossary.md`](../glossary.md)。

| 术语 | 含义 |
|------|------|
| **SQ / RQ** | 发送队列 / 接收队列，组成一个 QP |
| **CQ / CQE** | 完成队列 / 完成队列条目，poll_cq 取出结果 |
| **WR / WQE** | 工作请求 / 其在硬件队列中的存储形式 |
| **SGE** | Scatter/Gather 元素 `{addr, length, lkey}`，一个 WR 可含多个 |
| **Doorbell** | MMIO 通知 NIC 新 WQE；批处理可合并多次 doorbell |
| **IMM** | 立即数，随 SEND/WRITE_WITH_IMM 携带的 32 位通知 |
| **Credit** | 流控信用，确保对端 RQ 有足够预投递 WR |
| **NUMA** | 非一致内存访问，QP/MR/线程应绑定同一 NUMA 节点 |
