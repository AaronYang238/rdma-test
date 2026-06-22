# 阶段四：可扩展架构

> 目标：从两进程 demo 走向支撑万级连接的服务端结构。

---

## 4.1 SRQ（Shared Receive Queue）

在高连接数场景，每个 QP 独立维护一个接收队列（RQ），要求预先投递足够多的 WR
防止 RNR（Receiver Not Ready）错误。1000 个 QP × N 个预投递 WR，内存消耗线性
增长，且绝大多数 WR 同时处于"空等"状态。

**SRQ** 把多个 QP 的接收队列合并为一个共享池：

- 内存从 `QP数 × N` 降为固定的 `N`，无论有多少个 QP。
- 任意 QP 收到消息时，NIC 从 SRQ 取出一个 WR 使用，完成后在 CQ 产生 CQE，
  `wc.qp_num` 标明是哪个 QP 收到的。
- 预投递由应用统一管理：`ibv_post_srq_recv`，而非各 QP 自己的 `ibv_post_recv`。

![SRQ：多 QP 共享一个接收队列](img/s4-1-srq.svg)

```c
// 创建 SRQ
struct ibv_srq_init_attr srq_attr = {
    .attr = { .max_wr = 1024, .max_sge = 1 }
};
struct ibv_srq *srq = ibv_create_srq(pd, &srq_attr);

// 预投递到 SRQ（而非某个 QP）
struct ibv_sge sge = { .addr = (uint64_t)buf, .length = buf_len, .lkey = mr->lkey };
struct ibv_recv_wr wr = { .wr_id = (uint64_t)buf, .sg_list = &sge, .num_sge = 1 };
struct ibv_recv_wr *bad;
ibv_post_srq_recv(srq, &wr, &bad);

// 创建 QP 时绑定 SRQ
struct ibv_qp_init_attr qp_attr = { .srq = srq, ... };
struct ibv_qp *qp = ibv_create_qp(pd, &qp_attr);
```

**陷阱**：SRQ 耗尽时所有关联 QP 都无法接收消息，必须维持足够的预投递深度；
通过 `ibv_get_srq_num` 获取 SRQ 编号以便在对端识别；`ibv_modify_srq` 可动态
调整 `srq_limit` 触发低水位事件。

---

## 4.2 共享 CQ + 单线程事件循环

传统做法每个 QP 配一个 CQ，多 QP 场景需要等比增加轮询线程，CPU 开销随连接数
线性增长。**共享 CQ** 把所有 QP 的完成事件汇入同一个队列，一个线程完成分发：

- 创建时 `cq_size` 按最坏情况（所有 QP 同时完成）设置。
- CQE 携带 `wr_id`（应用自定义）和 `qp_num`，足以定位操作上下文。
- 适合"高连接数、中低 QPS"场景；若 QPS 极高，按核分片更优（见阶段三 3.5）。

![共享 CQ + 单线程事件循环](img/s4-2-shared-cq.svg)

```c
// 一个 CQ 服务 N 个 QP
struct ibv_cq *cq = ibv_create_cq(ctx, CQ_DEPTH, NULL, NULL, 0);

// 所有 QP 创建时指定同一个 send_cq / recv_cq
struct ibv_qp_init_attr attr = {
    .send_cq = cq,
    .recv_cq = cq,
    ...
};

// 事件循环
void event_loop(struct ibv_cq *cq) {
    struct ibv_wc wc[32];
    while (running) {
        int n = ibv_poll_cq(cq, 32, wc);
        for (int i = 0; i < n; i++) {
            if (wc[i].status != IBV_WC_SUCCESS) handle_error(&wc[i]);
            else dispatch(&wc[i]);   // 按 wr_id / qp_num 分发
        }
    }
}
```

**与事件通知结合**：使用 `ibv_req_notify_cq` + `ibv_get_cq_event` 代替纯忙轮询
可大幅降低空闲 CPU，适合连接众多但不全活跃的场景（参见阶段三 3.2 混合策略）。

---

## 4.3 连接管理规模化：手工 QP 状态机

`rdma_cm` 把 QP 状态迁移封装成类 socket API；当需要精细控制或大规模批量建连时，
常改用**带外 TCP 交换元数据 + 手工调用 `ibv_modify_qp`** 的方案。

QP 出厂状态为 **RESET**，必须按顺序迁移：

```
RESET → INIT → RTR（Ready To Receive）→ RTS（Ready To Send）
```

每步调用 `ibv_modify_qp` 并填写对应属性：

![手工 QP 状态机](img/s4-3-qp-state.svg)

```c
// 1. RESET → INIT
struct ibv_qp_attr attr = {
    .qp_state        = IBV_QPS_INIT,
    .pkey_index      = 0,
    .port_num        = port,
    .qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ,
};
ibv_modify_qp(qp, &attr,
    IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);

// 2. INIT → RTR（需对端 QPN / GID / PSN，通过带外 TCP 交换）
attr.qp_state              = IBV_QPS_RTR;
attr.path_mtu              = IBV_MTU_4096;
attr.dest_qp_num           = remote_qpn;
attr.rq_psn                = remote_psn;
attr.max_dest_rd_atomic    = 16;
attr.min_rnr_timer         = 12;
attr.ah_attr.is_global     = 1;
attr.ah_attr.grh.dgid      = remote_gid;
attr.ah_attr.grh.hop_limit = 64;
attr.ah_attr.dlid          = 0;  // RoCE 下不用 LID
attr.ah_attr.port_num      = port;
ibv_modify_qp(qp, &attr,
    IBV_QP_STATE | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
    IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER | IBV_QP_AV);

// 3. RTR → RTS
attr.qp_state      = IBV_QPS_RTS;
attr.timeout       = 14;   // ~67 ms
attr.retry_cnt     = 7;
attr.rnr_retry     = 7;    // 无限重试
attr.sq_psn        = local_psn;
attr.max_rd_atomic = 16;
ibv_modify_qp(qp, &attr,
    IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
    IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC);
```

**关键约束**：
- 必须先到 **RTR** 才能 `ibv_post_recv`（否则无 RQ 接收上下文）。
- RTR 之后 → RTS 之后才能 `ibv_post_send`。
- QP 进入 **ERROR** 态后须 `ibv_modify_qp(RESET)` 重置，再重走上述流程。
- 带外元数据通常用普通 TCP socket 交换：`{ qpn, psn, gid, lid }`。

---

## 4.4 UD / DC：一对多与连接爆炸

### UD（Unreliable Datagram）

UD 传输类型下，**单个 QP 可向任意多个目标发送**，无需为每对关系建立 QP：

- 每条 SEND 消息附带 **AH（Address Handle）**，指定目标 GID/LID/SL。
- 仅支持 SEND（无 WRITE/READ）。
- 不保证顺序，不重传，消息可丢失（"不可靠"）。
- 消息大小不能超过一个 MTU（4096 字节典型值）。
- 接收方 `wc.src_qp` 可获知发送方 QPN。

```c
// 创建 UD QP
struct ibv_qp_init_attr attr = { .qp_type = IBV_QPT_UD, ... };

// 发送时指定 Address Handle
struct ibv_send_wr wr = {
    .opcode = IBV_WR_SEND,
    .wr.ud.ah       = ah,          // ibv_create_ah() 创建
    .wr.ud.remote_qpn  = dest_qpn,
    .wr.ud.remote_qkey = QKEY,
};
```

### DC（Dynamic Connected，Mellanox/NVIDIA 私有扩展）

DC 结合了 RC 的可靠语义（WRITE/READ）与 UD 的一对多扩展性：

- **DCT（DC Target）**：被动接受方，相当于"监听端"。
- **DCI（DC Initiator）**：主动发起方，每次操作动态连接到不同 DCT。
- NIC 内部维护连接缓存（类似 TLB），自动复用已有连接，对应用透明。
- 万节点 All-to-All 场景：N² 对连接，用 DC 只需 N 个 DCI 和 N 个 DCT。

![UD 一对多 vs DC 动态连接](img/s4-4-ud-dc.svg)

**取舍总结**：

| 维度 | UD | DC | RC |
|------|----|----|-----|
| 可靠性 | 无 | 有（RC 语义） | 有 |
| 操作 | SEND only | WRITE/READ/SEND | 全集 |
| 扩展性 | 最佳（1 QP→N） | 优秀（NIC 内动态） | 差（N² QP） |
| 可用性 | 全厂商 | 仅 Mellanox/NVIDIA | 全厂商 |
| 适用场景 | 控制面广播 | HPC All-to-All | 点对点数据面 |

> DC 是 Mellanox ConnectX 网卡私有功能（需 `IBV_EXP_*` 实验性 API 或 DOCA SDK）；
> 跨厂商或纯标准 RoCE 环境需以 **RC + XRC** 替代 DC。

---

## 小结：原理 → API → 代码 → 性能 → 陷阱

| 维度 | 要点 |
|------|------|
| **原理** | 高连接数的两大瓶颈：RQ 内存（SRQ 解决）、CQ 线程（共享 CQ 解决）；连接建立成本（手工状态机批量化）；QP 数爆炸（UD/DC 解决） |
| **API** | `ibv_create_srq` / `ibv_post_srq_recv`；共享 `send_cq/recv_cq`；`ibv_modify_qp` 三步迁移；UD AH + `IBV_QPT_UD` |
| **代码** | SRQ 与 QP 在同一 PD 下创建；CQ 大小按峰值并发 WR 估算；带外 TCP 交换 `{qpn, psn, gid}` |
| **性能** | SRQ 将 RQ 内存从 O(连接数) 降到 O(1)；共享 CQ 将轮询线程从 O(连接数) 降到 O(1)；DC 将 QP 数从 O(N²) 降到 O(N) |
| **陷阱** | SRQ 耗尽静默丢包；QP 未到 RTR 就 post_recv 报错；ERROR 态 QP 须 RESET 后重建；UD 消息含 40B GRH 头需在 RQ 预留额外空间 |

---

## 本阶段术语速查

> 完整术语表见 [`docs/glossary.md`](glossary.md)。

| 术语 | 含义 |
|------|------|
| **SRQ** | 共享接收队列，多 QP 共用一个 RQ，内存降为 O(1) |
| **CQ / CQE** | 完成队列；共享 CQ 用 `wr_id`/`qp_num` 区分来源 |
| **QP / QPN** | 队列对 / 其 24 位唯一编号，建连时交换 |
| **PSN** | 包序列号，建连双方交换初始值（随机起始）|
| **GID** | 128 位全局标识符，RoCE 下由 MAC/IP 派生 |
| **AH** | 地址句柄，UD SEND 须指定目标地址 |
| **RC / UC / UD** | 三种传输类型，扩展性 UD > DC > RC |
| **DC / DCT / DCI** | 动态连接 / 目标端 / 发起端，缓解 N² QP |
| **XRC** | 扩展可靠连接，纯标准 RoCE 下 DC 的替代方案 |
