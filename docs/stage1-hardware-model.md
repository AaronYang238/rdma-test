# 阶段一 · 原理与硬件模型

> 本文是 RDMA 进阶教程的**理论篇**，对应 `TODO.md` 阶段一 1.1–1.5。
> 面向已掌握基本 verbs API（参见 `CLAUDE.md` 入门篇与 `examples/01-05`）、
> 想建立**准确硬件心智模型**的高级工程师。
> 每节均附 SVG 图（`docs/img/s1-*.svg`）。

---

## 目录

1. [数据路径的真实代价：MMIO · DMA · WC vs WB](#1-数据路径的真实代价)
2. [Verbs 软硬件分层：libibverbs / provider / uverbs / 硬件](#2-verbs-软硬件分层)
3. [传输类型对比：RC / UC / UD / XRC / DC](#3-传输类型对比)
4. [内存与地址翻译：MR / MPT / MTT / IBV_ACCESS_*](#4-内存与地址翻译)
5. [完成语义与排序保证：fence 与 PCIe ordering](#5-完成语义与排序保证)

---

## 1. 数据路径的真实代价

RDMA 的"内核旁路"并不是魔法——它把系统调用的开销换成了 **MMIO + PCIe 事务**
的开销。理解这条路径，才能知道延迟的下限在哪里、该在哪里优化。

### 一次 post_send 的硬件事务序列

```
① CPU 填写 WQE（Work Queue Element）到 SQ 所在内存（Write-Back 普通内存）
② CPU 写 doorbell 寄存器（MMIO，Write-Combining 内存）→ 网卡得到通知
③ 网卡通过 PCIe DMA 读 WQE（可能顺带读 inline 以外的数据 buffer）
④ 网卡完成传输后，PCIe DMA 写 CQE 到 CQ 内存
⑤ CPU poll_cq，读取 CQE 确认完成
```

![一次 post_send 的硬件事务路径](../docs/img/s1-1-datapath.svg)

### Write-Combining（WC）vs Write-Back（WB）

| 内存类型 | 用途 | 特点 |
|----------|------|------|
| WB（普通） | SQ / CQ / 数据 buffer | CPU cache 一致，可被 DMA 读到最新值 |
| WC（MMIO doorbell 区） | 写 doorbell 通知网卡 | CPU 写合并后再刷到 PCIe 总线；**不可读** |

doorbell 区映射到 `/proc/<pid>/maps` 里 `[uverbs]` 开头的 mmap 区域。每次
`rdma_post_send` 最终触发一次对该区域的 MMIO 写，加上来回 PCIe 事务，构成
RDMA 延迟的**硬件下限**，通常在百纳秒量级（取决于 PCIe 代数、NUMA 距离）。

### 常见延迟来源分析

```
总延迟 ≈ WQE 写缓存（WB）+ doorbell 刷出（WC）
       + PCIe 读 WQE 往返
       + 数据 DMA（WRITE/READ）
       + PCIe 写 CQE 往返
       + 线缆/交换机传播
```

跨 NUMA 的 doorbell 写（CPU 与网卡不在同一 NUMA 节点）会使延迟劣化
30–50 ns，是 **CPU-NIC 亲和性**要求的硬件根因。

---

## 2. Verbs 软硬件分层

RDMA 软件栈分三层，**数据面与控制面走不同路径**，混淆二者是常见的误解根源。

![Verbs 软硬件分层与两条路径](../docs/img/s1-2-verbs-layers.svg)

### 控制面（慢路径）

```
应用 → librdmacm / libibverbs → ioctl(uverbs) → 内核 ib_core → 驱动 → 硬件
```

创建 / 销毁 QP、MR、CQ、PD 都走这条路。每次均有系统调用，但发生频率极低
（连接建立时），不在关键路径上。

### 数据面（快路径）

```
应用 → libibverbs → provider（libmlx5 等）→ WQE 写用户态 mmap → MMIO doorbell
                                                                      ↓
                                                              RNIC 硬件直接处理
```

`ibv_post_send` / `ibv_poll_cq` 完全在用户态执行，**0 系统调用、0 内核态切换**。
provider 库（如 `libmlx5`）直接调用网卡的用户态驱动接口写 WQE 并敲 doorbell。

### 关键文件与符号

```
/dev/infiniband/uverbs0        # 控制面字符设备（ioctl）
/sys/class/infiniband/mlx5_0/ # 设备能力、计数器
ldd bin/server | grep mlx5    # 确认 provider 是否链接
```

---

## 3. 传输类型对比

不同 QP 类型在**可靠性**、**连接性**、**支持的操作**和**扩展代价**上差异显著，
选型直接影响架构。

![传输类型对比](../docs/img/s1-3-transports.svg)

### 四种主要类型

| 类型 | 可靠性 | 连接 | 支持操作 | 扩展性 |
|------|--------|------|----------|--------|
| **RC**（Reliable Connected） | 有序可靠，硬件重传 | 一对一 | SEND/RECV/WRITE/READ/ATOMIC | 差：N² QP |
| **UC**（Unreliable Connected） | 无重传，包可丢 | 一对一 | SEND/RECV/WRITE | 较差 |
| **UD**（Unreliable Datagram） | 无重传，包可丢 | 一对多 | 仅 SEND/RECV | 最佳：1 QP |
| **XRC**（eXtended RC） | 同 RC | 共享 RQ | SEND/RECV/WRITE/READ | 缓解 N² |

### DC（Dynamic Connected，Mellanox 扩展）

DC 是 Mellanox/NVIDIA 网卡特有的类型，兼顾 RC 的可靠性与 UD 的扩展性。
连接在网卡内**动态建立与复用**，不需要应用层维护全量 QP，特别适合
万节点以上的 All-to-All 通信（如 NCCL、存储网络）。

### 何时选什么

- **教学 / 通用**：RC，语义最全、最易理解（本仓库所有示例）。
- **低延迟广播 / 路由发现**：UD。
- **大规模存储 / AI 集群**：DC 或 RoCE v2 + ECMP。
- **QP 数量超出硬件限制**：XRC 或 SRQ（见阶段四）。

---

## 4. 内存与地址翻译

RDMA 的安全与性能都建立在**内存注册**机制上。注册不只是"告诉网卡有这块内存"，
而是构建了一条从 rkey 到物理地址的**鉴权翻译链**。

![RNIC 地址翻译与鉴权链（MPT / MTT）](../docs/img/s1-4-address-translation.svg)

### MPT（Memory Protection Table）

每个 MR 在 RNIC 内对应一条 MPT 表项，索引键为 `lkey` 或 `rkey`。
远端 WRITE/READ 请求到达时，网卡先查 MPT：

1. **PD 校验**：请求来自的 QP 与 MR 是否属于同一 PD。
2. **权限校验**：操作类型是否在 `IBV_ACCESS_*` 权限集内。
3. **范围校验**：`addr + length` 是否落在注册范围内。

任何一项失败，产生完成错误（`IBV_WC_REM_ACCESS_ERR` 等），不会访问内存。

### MTT（Memory Translation Table）

MPT 通过 `MTT_base` 指向一组物理页映射（等价于内核的页表）。
`ibv_reg_mr` 的开销主要在于：

- `get_user_pages()` pin 住所有物理页（防止被 swap 换出）。
- 在 RNIC 的 MTT 中建立 VA → PA 映射。

这就是 `reg_mr` 较慢（μs–ms 级）的原因，也是为什么生产环境要用**注册缓存**
（registration cache）而非每次收发都 reg/dereg（见阶段六）。

### IBV_ACCESS_* 权限矩阵

```c
IBV_ACCESS_LOCAL_WRITE    // 本端 RQ（post_recv）或 READ 目标必须有此权限
IBV_ACCESS_REMOTE_WRITE   // 允许对端 RDMA WRITE
IBV_ACCESS_REMOTE_READ    // 允许对端 RDMA READ
IBV_ACCESS_REMOTE_ATOMIC  // 允许对端原子操作（FETCH_ADD / CMP_SWAP）
IBV_ACCESS_ON_DEMAND      // ODP：延迟 pin（见阶段六）
```

规则：**对端能做什么完全由 MR 注册时的权限决定**，rkey 是钥匙，MPT 是锁。
最小权限原则：只开放必要的位，降低 rkey 泄露的爆炸半径。

---

## 5. 完成语义与排序保证

RC QP 保证报文有序到达，但"有序到达"与"对端内存可见"、"对端 CPU 可读"之间
仍有微妙差别，是 RDMA 程序出现数据竞争的常见根源。

![RC 下 SQ 内操作的排序与 fence](../docs/img/s1-5-ordering.svg)

### RC 内的顺序保证

```
同一 QP：WR 按投递顺序在 SQ 中排队，网卡按序执行。
```

具体来说（IB Spec Vol.1 §10.7.3）：

- **SEND 可用作数据栅栏**：RC 下，同一 QP 先 WRITE 后 SEND，对端**保证**先收到
  WRITE 的数据，再收到 SEND——这正是示例 01/04 用 SEND/ACK 当通知的理论依据。
- **READ 不保证与之前 WRITE 的顺序**：`post_write` 之后立刻 `post_read`，READ
  可能读到 WRITE 未落地前的旧值（网卡流水线乱序）。
- **跨 QP 无顺序保证**：多个 QP 之间没有全局顺序，需应用层同步。

### IBV_SEND_FENCE

```c
wr.send_flags = IBV_SEND_SIGNALED | IBV_SEND_FENCE;
```

带 FENCE 标志的 WR 会等待**本 QP 内所有先前的 READ / ATOMIC 完成**后，
才开始执行自身。典型用法：FETCH_ADD → FENCE → WRITE（用旧值写入结果）。

### PCIe Relaxed Ordering 的影响

PCIe 默认启用 Relaxed Ordering（RO），允许 PCIe 事务重排，以提高总线利用率。
这对 RDMA 的影响：

- NIC 的 DMA 写（CQE / RDMA WRITE 数据）可能在对端以乱序落地。
- Mellanox 网卡在 RC 下默认**关闭 RO**，保证 WRITE 语义；但 UD 场景下可能存在。
- 自研或异构场景需检查 `ethtool -I <dev>` 和 PCIe 配置，或显式用 FENCE。

### 生产排查要点

```
wc.status == IBV_WC_SUCCESS   ≠ 对端数据已完全可见（不同 CPU core 的 cache）
                               → 需要 fence 或应用层同步原语
wc.status != IBV_WC_SUCCESS   → QP 进入 ERROR 态，后续 WR 全部以 flush error 完成
                               → 必须重建 QP（阶段五详述）
```

---

## 小结：五段式（原理 → API → 代码 → 性能 → 陷阱）

| 节 | 原理 | 核心 API | 代码位置 | 性能影响 | 常见陷阱 |
|----|------|---------|---------|---------|---------|
| 1.1 | MMIO + DMA 是真实路径 | `ibv_post_send` doorbell | `common/rdma_common.h` `wait_send_comp` | NUMA 亲和 ±50 ns | 误认为"零延迟" |
| 1.2 | 数据面 0 syscall | provider mmap | 透明，应用不感知 | syscall = 微秒级；WQE 写 = 纳秒级 | 混淆控制面/数据面开销 |
| 1.3 | RC/UD/DC 取舍 | `qp_type` in `ibv_qp_init_attr` | `fill_qp_attr()` `common/rdma_common.h:43` | UD 1 QP vs RC N² QP | 大规模下坚持用 RC 导致 QP 爆炸 |
| 1.4 | MPT/MTT 鉴权链 | `ibv_reg_mr` `IBV_ACCESS_*` | `examples/*/server.c` `client.c` | reg 慢（ms）；MTT 命中影响 DMA | 权限过宽暴露 rkey |
| 1.5 | RC 有序但需 fence | `IBV_SEND_FENCE` `wc.status` | `wait_send_comp` / `wait_recv_comp` | fence 串行化 SQ | 误认为 WRITE 后 READ 仍看新值 |
