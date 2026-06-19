# 阶段五：可靠性与生产化

> 本文对应 [`TODO.md`](../TODO.md) 阶段五的内容。覆盖 WC 错误处理、重传参数调优、
> 拥塞控制（PFC/ECN/DCQCN）与资源生命周期管理。

---

## 5.1 错误完成处理

![WC 错误处理](img/s5-1-wc-error.svg)

### IBV_WC_* 错误码速查表

| 错误码 | 含义 | 常见原因 |
|--------|------|----------|
| `IBV_WC_RNR_RETRY_EXC_ERR` | RNR 重试次数耗尽 | 对端 RQ 空（未及时 post_recv） |
| `IBV_WC_RETRY_EXC_ERR` | 通用重试次数耗尽 | 链路故障、对端宕机、超时 |
| `IBV_WC_LOC_LEN_ERR` | 本地长度错误 | SGE 长度超过 MR 大小 |
| `IBV_WC_REM_ACCESS_ERR` | 远端访问权限错误 | rkey 无效或权限不足（无 REMOTE_WRITE/READ） |
| `IBV_WC_WR_FLUSH_ERR` | WR 被冲刷 | QP 已处于 ERROR 状态时投递的 WR |

### QP 进入 ERROR 状态

任何操作完成时若 `wc.status != IBV_WC_SUCCESS`，该 QP 即进入 **ERROR** 状态。
此后队列中所有未完成的 WR 都会以 `WR_FLUSH_ERR` 完成——这是"冲刷"行为，并非
新错误。务必区分：**真正的错误**是第一个非 SUCCESS 的 CQE，后续 FLUSH 只是
清场动作。

```c
struct ibv_wc wc;
while (ibv_poll_cq(cq, 1, &wc) > 0) {
    if (wc.status != IBV_WC_SUCCESS) {
        if (wc.status == IBV_WC_WR_FLUSH_ERR) {
            /* QP 已出错，正在冲刷队列，忽略或统计 */
            continue;
        }
        fprintf(stderr, "WC error: %s (vendor_err=0x%x)\n",
                ibv_wc_status_str(wc.status), wc.vendor_err);
        /* 触发 QP 恢复流程 */
    }
}
```

### QP 恢复：RESET → INIT → RTR → RTS

QP 一旦进入 ERROR 状态，必须经过完整状态机重置才能复用：

```c
/* 1. 重置到 RESET */
struct ibv_qp_attr attr = { .qp_state = IBV_QPS_RESET };
ibv_modify_qp(qp, &attr, IBV_QP_STATE);

/* 2. RESET → INIT */
attr.qp_state   = IBV_QPS_INIT;
attr.pkey_index = 0;
attr.port_num   = port;
attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;
ibv_modify_qp(qp, &attr,
    IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);

/* 3. INIT → RTR（需重新交换 QPN/GID 等信息） */
/* 4. RTR → RTS */
/* ... 与初始化时相同 ... */
```

> **生产建议**：将 QP 生命周期封装成状态机；错误发生时先排空 CQ（直到没有更多
> CQE），再做 RESET，避免遗留 CQE 污染下一轮操作。

---

## 5.2 重传与超时

![重传与超时参数](img/s5-2-retry.svg)

RC QP 内置重传机制，通过三个关键参数控制：

### retry_cnt — 通用重传计数

```c
attr.retry_cnt = 7;   /* 0–7，7 = 无限重传 */
```

当发送方等不到 ACK（超时），会触发重传，最多重传 `retry_cnt` 次。超过后报
`RETRY_EXC_ERR`。重传间隔由 `timeout` 决定，**每次翻倍**（指数退避）。

- **数据中心**：设 `retry_cnt=7`（无限），依赖 PFC 避免丢包，通常不会真正触发。
- **WAN / 有损网络**：设较小值（3–5），及早发现故障快速上报。

### rnr_retry — RNR 重传计数

```c
attr.rnr_retry = 7;   /* 0–7，7 = 无限 */
```

**RNR（Receiver Not Ready）**：对端 RQ 没有预投递的 RECV WR。RC 协议允许发送方
在收到 RNR NAK 后等待一段时间（由**对端** `min_rnr_timer` 决定）再重试，最多
`rnr_retry` 次后报 `RNR_RETRY_EXC_ERR`。

> 避免 RNR 的根本方法：保持 RQ 始终有足够的 RECV WR。SRQ（共享接收队列）可以
> 减轻多 QP 场景下的 RECV 管理负担。

### timeout — 本地 ACK 超时

```c
attr.timeout = 14;  /* 单位：4.096µs × 2^timeout */
                    /* timeout=14 ≈ 67ms */
```

公式：`T = 4.096µs × 2^timeout`

| timeout 值 | 超时时间 |
|-----------|---------|
| 0 | 无限（禁用） |
| 8 | ~1ms |
| 14 | ~67ms |
| 17 | ~536ms |
| 21 | ~8.5s |

**数据中心推荐**：`timeout=14`，`retry_cnt=7`，`rnr_retry=7`。

---

## 5.3 拥塞控制

![DCQCN 拥塞控制](img/s5-3-dcqcn.svg)

RDMA over Converged Ethernet（RoCEv2）依赖**无损以太网**。拥塞控制分两层：

### PFC（Priority Flow Control）— 无损传输基础

PFC 是 IEEE 802.1Qbb 标准，允许交换机向上游发送 **PAUSE 帧**，暂停特定优先级
的流量，从而避免丢包。

**HOL（Head-of-Line）阻塞问题**：PFC 是 per-priority 的，但同一优先级内不同流
相互影响——一条慢流可以暂停整个优先级队列，阻塞其他无关流。

### ECN（Explicit Congestion Notification）— 主动通知

交换机检测到队列深度超过阈值时，在数据包 IP 头打上 **CE（Congestion
Experienced）**标记。接收方识别到 CE 标记后，回送一个 **CNP（Congestion
Notification Packet）**给发送方。

### DCQCN 算法

DCQCN（Data Center Quantized Congestion Notification）是 RoCEv2 标准拥塞控制
算法：

1. **速率降低**：收到 CNP → 乘性减小发送速率（类似 TCP AIMD 的 MD 部分）。
2. **慢启动恢复**：一段时间无 CNP → 加性增大速率，逐步探测可用带宽。
3. **快速恢复**：连续无 CNP 超过阈值 → 切换到快速恢复模式，更激进地增加速率。

### 可观测计数器

```bash
# 查看硬件拥塞计数器
ls /sys/class/infiniband/mlx5_0/ports/1/hw_counters/

# 关键指标
cat /sys/class/infiniband/mlx5_0/ports/1/hw_counters/np_cnp_sent      # CNP 发送数
cat /sys/class/infiniband/mlx5_0/ports/1/hw_counters/rp_cnp_handled   # CNP 处理数
cat /sys/class/infiniband/mlx5_0/ports/1/hw_counters/out_of_buffer     # PFC 触发次数

# 使用 perfquery 查看端口计数器
perfquery -x <lid>
```

> **调优建议**：在交换机侧启用 ECN + DCQCN，PFC 作为兜底（仅开 RDMA 所在
> priority）。避免全局 PFC，防止 HOL 蔓延到非 RDMA 流量。

---

## 5.4 资源生命周期与泄漏排查

![资源生命周期](img/s5-4-lifecycle.svg)

### 正确的销毁顺序

RDMA 资源之间存在依赖关系，**必须按照与创建相反的顺序销毁**：

```
销毁顺序（从子到父）：
  MR       → 先于 PD 销毁
  QP       → 先于 CQ、PD 销毁
  SRQ      → 先于 PD 销毁
  CQ       → 先于 ibv_context 销毁
  PD       → 先于 ibv_context 销毁
  ibv_context → 最后关闭（ibv_close_device）
```

```c
/* 正确的清理顺序示例 */
if (ctx->mr)  { ibv_dereg_mr(ctx->mr);    ctx->mr  = NULL; }
if (ctx->qp)  { ibv_destroy_qp(ctx->qp);  ctx->qp  = NULL; }
if (ctx->srq) { ibv_destroy_srq(ctx->srq); ctx->srq = NULL; }
if (ctx->cq)  { ibv_destroy_cq(ctx->cq);  ctx->cq  = NULL; }
if (ctx->pd)  { ibv_dealloc_pd(ctx->pd);  ctx->pd  = NULL; }
if (ctx->ctx) { ibv_close_device(ctx->ctx); ctx->ctx = NULL; }
```

违反顺序会得到 `EBUSY` 错误（子资源仍存活时无法销毁父资源）。

### fork() 陷阱

```c
/* 必须在 ibv_open_device 之前调用 */
ibv_fork_init();

pid_t pid = fork();
if (pid == 0) {
    /* 子进程：FD 已继承，但 pin 住的物理页映射已失效！
     * 不调用 ibv_fork_init() 的后果：子进程访问已注册内存 → 段错误或数据损坏 */
}
```

`ibv_fork_init()` 通过 `mmap(MAP_ANONYMOUS|MAP_PRIVATE)` 重映射注册内存，使
`fork()` 的 copy-on-write 机制对 RDMA 内存安全。也可设置环境变量：

```bash
export RDMAV_FORK_SAFE=1   # 等价于程序内调用 ibv_fork_init()
```

### madvise(MADV_DONTNEED) 静默 unpin

```c
/* 危险！静默解除内存 pin */
madvise(mr->addr, mr->length, MADV_DONTNEED);
/* 内核可能回收物理页，但 ibv_mr 仍然"有效"
 * 后续 RDMA 操作将导致 REM_ACCESS_ERR 或数据损坏 */
```

已注册的 MR 内存**禁止**调用 `madvise(MADV_DONTNEED)` 或 `madvise(MADV_FREE)`。
若需要释放内存，必须先 `ibv_dereg_mr()`。

### 泄漏排查工具

```bash
# 1. valgrind（检测用户态内存泄漏）
valgrind --leak-check=full ./rdma_server 192.168.1.1 7471

# 2. 查看内核侧资源（每个进程）
ls /proc/<pid>/fd/ | wc -l          # FD 数量（rdma_cm 每个 endpoint 占 1 个）

# 3. rdma-core 调试环境变量
export IBV_SHOW_WARNINGS=1           # 打印 libibverbs 警告
export RDMAV_FORK_SAFE=1

# 4. 查看系统级 MR/QP 统计
cat /sys/class/infiniband/mlx5_0/ports/1/counters/port_rcv_errors
rdma res show mr                     # 列出系统所有 MR（需 iproute2-rdma）
rdma res show qp                     # 列出系统所有 QP
```
