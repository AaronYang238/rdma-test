# 附录 B · 示例索引

> 本书每个核心概念都配有一个**独立可编译运行**的示例。这张索引把
> `examples/01` ～ `examples/08` 映射到书中章节，方便你「读到哪、跑到哪」。每个示例
> 都能在 Soft-RoCE 上跑通，**无需 RDMA 硬件**。

---

## 如何编译运行所有示例

所有示例共享 `common/` 脚手架，顶层一条 `make` 即可递归构建：

```bash
make            # 顶层递归构建所有示例（需 RDMA 开发库）
make list       # 列出可构建示例
make clean      # 清理产物
```

每个示例编译出 `bin/server` 与 `bin/client`，统一用法：

```bash
./bin/server <RDMA网卡IP> 7471    # 终端 1
./bin/client <RDMA网卡IP> 7471    # 终端 2
```

> 📌 没有 RDMA 网卡？用 Soft-RoCE 在普通以太网卡上模拟即可——**环境搭建见
> [第 2 章 · 30 分钟跑起来](part1-02-quickstart.md)**。Soft-RoCE 支持本书全部示例
> （SEND/RECV、WRITE、READ、ATOMIC、SRQ 等），是验证代码逻辑的理想环境（但延迟高，
> 不可用于性能基准）。

![构建与运行流程](../img/09-build-run.svg)

---

## 示例 → 章节映射

| 示例 | 演示什么 | 对应章节 | 一句话运行提示 |
|------|---------|---------|---------------|
| [01-write-demo](../../examples/01-write-demo/) | 控制面（SEND/RECV 交换 MR 元数据）+ 数据面（RDMA WRITE 推数据）的基线全流程 | [第 2 章](part1-02-quickstart.md)、[第 7 章](part1-07-write-read.md) | 入门第一个示例；服务端 WRITE 把字符串直接写进客户端内存 |
| [02-send-recv](../../examples/02-send-recv/) | 双边 SEND/RECV 乒乓往返 + 平均延迟基准 | [第 6 章](part1-06-send-recv.md) | 两端互发，体会预投递与双边语义 |
| [03-read](../../examples/03-read/) | RDMA READ 单边读，从对端内存把数据拉回本地 | [第 7 章](part1-07-write-read.md) | 与 WRITE 方向相反，对端 CPU 不参与 |
| [04-immediate](../../examples/04-immediate/) | `WRITE_WITH_IMM`：写数据 + 立即数通知合一 | [第 9 章](part1-09-imm-atomic.md) | 客户端读 `wc.imm_data` 当通知 |
| [05-atomic](../../examples/05-atomic/) | `FETCH_ADD` / `CMP_SWAP` 远端原子操作 | [第 9 章](part1-09-imm-atomic.md) | 对端内存上做原子加 / 比较交换 |
| [06-selective-signaling](../../examples/06-selective-signaling/) | 选择性 signaling 的吞吐对比实验（每 N 个才 signaled 一次） | [第 11 章](part2-11-performance.md) | 对比全 signaled vs 选择性 signaling 的吞吐差异 |
| [07-srq](../../examples/07-srq/) | 多 QP 共享一个 SRQ，把 RQ 内存从 O(连接数) 降为 O(1) | [第 12 章](part2-12-scalability.md) | 多个 QP 从同一个 SRQ 取接收 WR |
| [08-rpc](../../examples/08-rpc/) | 极简请求/响应 RPC（ADD/MUL）+ 平均往返延迟基准 | [第 15 章](part3-15-integration.md) | 客户端发 `{op,a,b}`，服务端算后回结果；两端 `iters` 须一致 |

---

## 配套关系一览

- **入门主线（第 1–9 章）**：示例 01–05 逐个对应核心原语——先建立控制面 + 数据面
  的完整图景（01），再单独吃透双边（02）、单边读（03）、立即数（04）、原子（05）。
- **进阶专题（第 10–14 章）**：示例 06–07 把性能工程（选择性 signaling）与可扩展
  架构（SRQ）的关键优化做成可动手对比的实验。
- **专家集成（第 15–17 章）**：示例 08 把 RDMA 原语拼成一个最小 RPC，正是第 15 章
  「与上层系统集成」的起点；更深的 mlx5dv/DEVX、DPU 等内容以伪代码呈现，无独立
  示例（依赖特定厂商硬件）。

> 每个示例目录下都有自己的 `README.md`，含「要点与陷阱」「构建与运行」「关联章节」
> 三段，建议读章节正文时对照动手。
