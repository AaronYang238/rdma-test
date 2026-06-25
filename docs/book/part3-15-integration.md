# 第 15 章 · 与上层系统集成

> 到这里，你已经掌握了 RDMA 的全部原语：双边的 SEND/RECV、单边的 WRITE/READ、
> 立即数与原子操作，以及性能、可扩展、可靠性、内存管理这些工程维度。但原语本身
> 不是终点——真实世界里，RDMA 总是作为某个更大系统的「传输底座」存在：一套 RPC
> 框架、一条 GPU 训练流水线、一个分布式存储集群。本章带你看清楚：这些上层系统是
> **怎么把 RDMA 原语拼成一个可用产品的**。

---

## 本章你将遇到的术语（预览）

| 术语 | 一句话直觉 |
|------|-----------|
| **IMM（立即数）** | 写数据的同时捎带一个 32 位「便条」，让对端不用额外往返就知道发生了什么 |
| **SGE** | 描述一段内存的三元组 `{addr, length, lkey}`，拼请求/响应缓冲用 |
| **RC / UD** | 传输类型；RPC 要可靠所以用 RC，广播类控制可用 UD |
| **RoCE** | 在以太网上跑 RDMA，RoCEv2 封装在 UDP 4791 |
| **GID** | 全局标识符，类比 IPv6，跨节点寻址用 |
| **NUMA** | GPUDirect 要求网卡与 GPU 在同一 PCIe 拓扑/NUMA 节点，否则带宽腰斩 |
| **MR / rkey** | GPU 显存也能 `ibv_reg_mr` 注册（需 peer-memory 内核模块） |

---

## 引子：原语不是系统

想象你刚学会了「砌砖」这门手艺——会和水泥、会对齐、会留缝。但会砌砖不等于会盖
房子。盖房子要的是图纸：地基怎么打、承重墙在哪、水电怎么走。

RDMA 也一样。`ibv_post_send` 是一块砖；而一个 RPC 框架、一套 NVMe-oF 存储、一个
NCCL 集合通信库，才是房子。本章的三个小节分别回答三个「怎么盖」的问题：

1. 怎么用 RDMA 原语搭出一个**低延迟 RPC**？（§15.1）
2. 怎么让远端网卡直接读写**本地 GPU 显存**，把 CPU 彻底踢出数据路径？（§15.2）
3. 业界已经用 RDMA 盖了哪些「成品房子」，它们各自怎么用原语？（§15.3）

> 📌 没有 RDMA 硬件也不要紧——本章所有方案都能在 Soft-RoCE 上跑通功能验证。
> **环境搭建见[第 2 章](part1-02-quickstart.md)**，这里不再重复。

---

## 15.1 极简 RPC over RDMA

**问题引入**：RPC（远程过程调用）是分布式系统的命脉——客户端调一个函数，参数和
返回值在网络上往返一趟。传统 TCP RPC 每次往返要两次系统调用 + 走一遍内核协议栈，
RTT 普遍在 10–50 µs。在一个每秒要发百万次调用的系统里，这点延迟会被放大成灾难。
RDMA 能把它压到 1–3 µs，问题是：**怎么用原语拼出一个 RPC？**

**直觉/类比**：把一次 RPC 想成「往对方信箱里塞一封信，再戳一下门铃告诉他有信」。
TCP 的做法是把「信」和「门铃」捆在一起走协议栈；RDMA 的做法是把两件事**拆开**：

- **数据（信）**：用 RDMA WRITE（单边，对端 CPU 完全不参与）直接写进对端的环形缓冲。
- **通知（门铃）**：用 `WRITE_WITH_IMM`，在写数据的同一个操作里让对端产生一个
  recv CQE，`imm_data` 里捎上槽位索引——**省掉一次单独的 SEND 往返**。

> 💡 **IMM（立即数）** 首次出现：它是一个随 WRITE/SEND 携带的 32 位整数，对端在
> CQE 的 `wc.imm_data` 字段能读到。这里我们用它当「槽位号」，告诉对端「请求落在
> 第几格」。

![极简 RPC over RDMA](../img/s7-1-rpc.svg)

> 🛠 可运行示例：[examples/08-rpc/](../../examples/08-rpc/)
> ——一个最小的请求/响应 RPC：客户端发 `{op, seq, a, b}`，服务端算 ADD/MUL 后回
> `{seq, status, result}`，并压测平均往返延迟。该示例用**双边 SEND/RECV** 承载
> （结构最清晰，服务端 CPU 参与每次调用）；本节进一步讲解单边 `WRITE_WITH_IMM`
> 环形缓冲优化，可把 RTT 压到 ~1 µs。

**环形缓冲设计**：

```c
#define SLOT_SIZE  4096
#define RING_SLOTS 256

struct rpc_ring {
    char     slots[RING_SLOTS][SLOT_SIZE];
    uint32_t head;   // 消费者读指针（本端维护，不通过 RDMA 暴露）
};

// 客户端发送请求：写数据 + IMM 通知槽位号
void send_request(struct ibv_qp *qp, struct ibv_mr *local_mr,
                  uint64_t remote_addr, uint32_t remote_rkey,
                  int slot, void *req, size_t len) {
    struct ibv_send_wr wr = {
        .opcode       = IBV_WR_RDMA_WRITE_WITH_IMM,
        .imm_data     = htonl(slot),
        .send_flags   = IBV_SEND_SIGNALED,
        .sg_list      = &(struct ibv_sge){
            .addr   = (uint64_t)req,
            .length = len,
            .lkey   = local_mr->lkey,
        },
        .num_sge      = 1,
        .wr.rdma.remote_addr = remote_addr + slot * SLOT_SIZE,
        .wr.rdma.rkey        = remote_rkey,
    };
    struct ibv_send_wr *bad;
    ibv_post_send(qp, &wr, &bad);
}

// 服务端事件循环：poll recv CQE 得到槽位号，处理请求
void server_loop(struct ibv_cq *cq, struct rpc_ring *ring) {
    struct ibv_wc wc;
    while (ibv_poll_cq(cq, 1, &wc) > 0) {
        if (wc.opcode == IBV_WC_RECV_RDMA_WITH_IMM) {
            int slot = ntohl(wc.imm_data);
            handle_request(ring->slots[slot]);
            // 重新投递 recv WR
            repost_recv(cq, slot);
        }
    }
}
```

> ⚠️ 复用[第 6 章](part1-06-send-recv.md)的铁律：**响应的 recv 必须在对应请求发出
> 前预投递**。08-rpc 示例里，客户端在 connect 前投递第一个 recv，之后每收到一个
> 响应就补投递下一个，形成深度 1 的接收流水线，避免 RNR 错误。

**性能对比**：

| 方案 | 典型 RTT | CPU 开销 |
|------|---------|---------|
| TCP RPC（内核） | 10–50 µs | 高（系统调用 + 中断） |
| RDMA RPC（busy-poll） | 1–3 µs | 中（轮询 CQ） |
| RDMA RPC（event 模式） | 3–10 µs | 低（睡眠等待） |

可以看到，busy-poll 用 CPU 换延迟，event 模式（见[第 11 章](part2-11-performance.md)
轮询 vs 事件）用延迟换 CPU——这是一对永恒的取舍。

---

## 15.2 GPUDirect RDMA

**问题引入**：在 AI 训练里，梯度数据本来就躺在 GPU 显存里。如果还要先把它从 GPU
拷回系统内存、再让网卡发出去，对端收到后又拷进它的 GPU——这一来一回的拷贝既吃
带宽又吃 CPU。能不能让网卡**直接读写 GPU 显存**？

**直觉/类比**：传统路径像「快递员只能到小区门口，住户得自己下楼取件再扛上楼」；
GPUDirect 则是「快递直接送货上门」——网卡通过 PCIe 点对点（P2P）直接 DMA 到 GPU
显存，**完全绕过系统内存和 CPU 拷贝**。

![GPUDirect RDMA 数据路径](../img/s7-2-gpudirect.svg)

**传统路径**（有 CPU 参与）：
```
远端 NIC → PCIe → 本端 NIC → 系统 RAM → CPU memcpy → GPU 显存
```

**GPUDirect 路径**（零拷贝）：
```
远端 NIC → PCIe → 本端 NIC → PCIe P2P → GPU 显存
```

关键在于：**GPU 显存也可以被 `ibv_reg_mr` 注册成 MR**——只要内核里有 peer-memory
模块，网卡就能拿到 GPU 显存的 rkey，对端 RDMA WRITE 时直接落进显存。

**使用方式（伪代码）**：

```c
// 1. 分配 GPU 内存
void *gpu_buf;
cudaMalloc(&gpu_buf, buf_size);

// 2. 注册为 RDMA MR（需 nvidia_peermem 或 nv_peer_mem 内核模块）
struct ibv_mr *gpu_mr = ibv_reg_mr(pd, gpu_buf, buf_size,
    IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);

// 3. 将 gpu_mr->rkey + gpu_buf 地址发给对端
// 4. 对端直接 RDMA WRITE 到 GPU 显存
rdma_post_write(id, NULL, src_buf, len, src_mr, IBV_SEND_SIGNALED,
                (uint64_t)gpu_buf, gpu_mr->rkey);
```

注意第 4 步的 `rdma_post_write` 和你在[第 7 章](part1-07-write-read.md)里写过的
一模一样——**应用代码几乎不变，变的只是那块内存的物理归属**。

**前提条件**：
- 加载 `nvidia_peermem`（新驱动）或 `nv_peer_mem`（旧驱动）。
- NIC 与 GPU 需在同一 PCIe root complex 下，或通过支持 P2P 的 PCIe switch 连接。
- 在 `nvidia-smi topo -m` 中确认 GPU 与 NIC 的拓扑关系（NV# 表示支持）。
- 并非所有 NIC/GPU 组合支持；A100/H100 + ConnectX-6/7 是典型支持配置。

> ⚠️ 这就是 **NUMA / PCIe 拓扑** 在本章的实际意义：如果网卡和 GPU 挂在不同的
> PCIe root complex 下，P2P 走不通或要绕远路，带宽会断崖式下跌。先看
> `nvidia-smi topo -m` 再谈优化。

---

## 15.3 生态总览

**问题引入**：学完原语，你难免会问：「真有人在生产里这么用吗？」答案是——几乎所有
追求极致 I/O 的系统都建在 RDMA Verbs 之上，只是它们把 Verbs 藏在了更友好的 API
之下。认识这些「成品房子」，能帮你判断什么时候该自己砌砖、什么时候该直接拎包入住。

**直觉/类比**：Verbs 之于这些框架，就像 POSIX socket 之于 Nginx、Redis、Kafka——
底层是同一组系统调用，上层却长出了存储、通信、计算三个方向的参天大树。

![RDMA 生态总览](../img/s7-3-ecosystem.svg)

### NVMe-oF（NVMe over Fabrics）

块存储协议，通过 RDMA 传输 NVMe 命令与数据，让远端 SSD「用起来像本地盘」：

```bash
# 加载内核模块（Target 端）
modprobe nvmet && modprobe nvmet-rdma
# 配置 namespace 并监听（通过 configfs）
# Initiator 端
modprobe nvme-rdma
nvme connect -t rdma -a <target_ip> -s 4420 -n nqn.xxx
```

- 延迟接近本地 NVMe（~10 µs vs ~100 µs TCP/IP NVMe-oF）。
- 内核态实现；用户态方案见下面的 SPDK。

### NCCL（NVIDIA Collective Communication Library）

AI 训练中的集合通信库，AllReduce / AllGather 底层使用 RDMA：

- IB 传输主要用 RC QP + RDMA WRITE / WRITE_WITH_IMM 搬运数据（不依赖 RDMA
  原子）；ring/tree 算法的同步靠 flag/计数轮询而非硬件原子操作。
- `NCCL_IB_HCA` 环境变量指定使用哪张 RDMA 网卡。
- 与 §15.2 的 GPUDirect RDMA 结合，实现 GPU-to-GPU 零拷贝通信。

### UCX（Unified Communication X）

传输无关的通信框架，被 OpenMPI、PyTorch Distributed、Apache Spark 采用：

```bash
ucx_info -d        # 查看可用传输（rc_verbs, ud_verbs, shared_mem 等）
ucx_perftest -t ucp_put_bw -m rdma  # 带宽测试
```

UCX 自动选择最优传输：同节点用共享内存，跨节点用 RC/UD，降级到 TCP——把
「选哪种语义」这个你在前面手动操心的问题自动化了。

### SPDK（Storage Performance Dev Kit）

用户态 NVMe-oF Target，结合 DPDK 风格的轮询和 RDMA：

- 绕过内核，延迟 < 10 µs。
- `spdk_tgt` 配合 `bdev_nvme` + `nvmf_rdma` transport 使用。
- 适合超低延迟存储场景（AI 训练 Checkpoint、数据库）。

---

## 小结：原理 → API → 代码 → 性能 → 陷阱

| 维度 | 要点 |
|------|------|
| **原理** | RDMA RPC = WRITE（数据）+ IMM（通知）合一；GPUDirect 靠 PCIe P2P 绕开 CPU；各上层框架各有侧重（存储/通信/计算） |
| **API** | `IBV_WR_RDMA_WRITE_WITH_IMM`；`ibv_reg_mr(gpu_buf)`（需 peer-mem 模块）；NVMe-oF/UCX/SPDK 把 Verbs 封装成更高层 API |
| **代码** | RPC 环形缓冲槽位用 imm_data 传递；GPUDirect 仅伪代码参考，应用层代码几乎不变 |
| **性能** | RDMA RPC 比 TCP 快 5–10×；GPUDirect 消除 GPU←→CPU 拷贝（带宽 × 2）；NVMe-oF/SPDK 延迟逼近本地盘 |
| **陷阱** | GPUDirect 需确认 PCIe 拓扑（`nvidia-smi topo`）；NCCL 调参需匹配 QP 数与 GPU 数；RPC 别忘了预投递 recv 防 RNR |

---

## 术语速查

| 术语 | 含义 |
|------|------|
| **IMM** | 立即数，WRITE_WITH_IMM 实现 RPC 响应 + 通知合一 |
| **SGE** | Scatter/Gather 元素，构造请求/响应缓冲 |
| **RC / UD** | 传输类型，RPC 用 RC，广播控制可用 UD |
| **RoCE** | RDMA over Converged Ethernet，RoCEv2 用 UDP 4791 |
| **GID** | 全局标识符，跨节点寻址 |
| **NUMA** | GPUDirect 要求 NIC 与 GPU 同 PCIe 拓扑 / NUMA |
| **MR / rkey** | GPU 显存也可 `ibv_reg_mr` 注册（需 peer-memory）|

> 完整术语表见[附录 A · 术语表与参考文献](appendix-a-glossary.md)。

---

> 下一章我们换个角色：从「搭系统的人」变成「修系统的人」——
> [第 16 章 · 调试与可观测性](part3-16-debugging.md)，建立一套完整的排障工具箱。
