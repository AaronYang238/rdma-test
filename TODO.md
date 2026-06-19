# TODO — RDMA 进阶教程编写路线图

> **项目新定位**：从"面向初学者的 RDMA 最小示例"升级为
> **「由浅入深、面向高级工程师的 RDMA 系统教程」**。
> 本文件是教程编写的**总纲与施工清单**，按阶段推进；每完成一项勾选 `[x]`。

---

## 0. 编写约定（贯穿全程）

- [x] 每章一个独立、可编译运行的 `examples/<NN>-<topic>/`，配 `README.md` 与
      `Makefile`，**循序递进**（后章复用前章封装）。*（骨架就绪：01/02/03）*
- [x] 每节配一张 **SVG 原理图**（沿用 `CLAUDE.md` 既有约定，单独文件 + 链接导入）。
- [ ] 每个示例提供**可观测产物**：延迟/带宽数字、`ibv_*` 计数器、perftest 对比。
      *（02 已输出 RTT/单向延迟；计数器/perftest 待阶段三/八）*
- [x] 代码风格沿用现有 `die_rdma` / `check_zero` / `wait_*_comp` 模式，C11。
- [ ] 每章末尾给出「**原理 → API → 代码 → 性能 → 陷阱**」五段式小结。
- [x] 抽取公共脚手架到 `common/`（错误处理、CQ 轮询、计时、QP 默认能力）。
      *（`common/rdma_common.h` + `common/rules.mk`；MR 池待阶段六）*

---

## 阶段一 · 原理与硬件模型（深度铺垫）

> 目标：让高级工程师建立**准确的硬件心智模型**，而非 API 罗列。

- [x] 1.1 内核旁路与零拷贝的**真实代价**：MMIO doorbell、DMA、PCIe 往返、
      WC vs WB 内存。SVG：`docs/img/s1-1-datapath.svg`。
- [x] 1.2 Verbs 软硬件分层：libibverbs / provider(mlx5) / uverbs / 硬件。
      SVG：`docs/img/s1-2-verbs-layers.svg`。
- [x] 1.3 传输类型对比 **RC / UC / UD / XRC / DC**：可靠性、扩展性、报文上限、
      QP 数量代价。SVG：`docs/img/s1-3-transports.svg`。
- [x] 1.4 内存与一致性模型：MR/MTT/MPT、地址翻译、`IBV_ACCESS_*` 全集。
      SVG：`docs/img/s1-4-address-translation.svg`。
- [x] 1.5 完成语义与排序保证：RC 下 WRITE/READ/SEND 的顺序、fence、
      `IBV_SEND_FENCE`、PCIe relaxed ordering。SVG：`docs/img/s1-5-ordering.svg`。
      文档：`docs/stage1-hardware-model.md`。

---

## 阶段二 · 编程基础（在现有 demo 上夯实）

> 目标：把当前 `src/server.c` / `src/client.c` 提炼为教学基线。

- [x] 2.1 重构现有 demo 为 `examples/01-write-demo/`，作为基线参照。
- [x] 2.2 `02-send-recv/`：纯双边乒乓，讲 RQ 预投递、credit、流控直觉。
- [x] 2.3 `03-read/`：RDMA READ 与 WRITE 的对称性、延迟差异实测。
- [x] 2.4 `04-immediate/`：`WRITE_WITH_IMM`，单边写顺带通知对端（省 ACK 往返）。
- [x] 2.5 `05-atomic/`：`FETCH_ADD` / `CMP_SWAP`，分布式锁/计数器原理与对齐要求。

---

## 阶段三 · 性能工程（高级工程师核心关切）

> 目标：把延迟做到 μs 级、带宽打满线速，并解释每一项优化的机理。

- [x] 3.1 完成机制优化：**选择性 signaling**（每 N 个 signaled 一次）、
      `unsignaled` SQ 回收。SVG：`docs/img/s3-1-selective-signaling.svg`。
- [x] 3.2 **轮询 vs 事件**：busy-poll 低延迟 vs `ibv_get_cq_event` 省 CPU；
      混合策略（先 poll 后 arm）。SVG：`docs/img/s3-2-poll-vs-event.svg`。
- [x] 3.3 **Inline data** 与小消息优化；`max_inline_data` 调优。
      SVG：`docs/img/s3-3-inline.svg`。
- [x] 3.4 **批处理 / doorbell batching**、链式 WR（`wr.next`）、SGE 聚合。
      SVG：`docs/img/s3-4-batching.svg`。
- [x] 3.5 **多 QP / 多核扩展**：QP 与 CPU 核绑定、CQ 共享 vs 分离、NUMA 亲和。
      SVG：`docs/img/s3-5-multi-qp.svg`。
- [x] 3.6 基准方法学：perftest 校准、P50/P99/带宽表格模板。
      SVG：`docs/img/s3-6-benchmark.svg`。文档：`docs/stage3-performance.md`。

---

## 阶段四 · 可扩展架构

> 目标：从两进程 demo 走向支撑万级连接的服务端结构。

- [ ] 4.1 **SRQ（Shared Receive Queue）**：降低海量连接的 RQ 内存。
- [ ] 4.2 **共享 CQ + 单线程事件循环**：一个 CQ 服务多 QP。
- [ ] 4.3 连接管理规模化：`rdma_cm` 事件通道 vs 手工 QP 状态机
      (`INIT→RTR→RTS`)；带外 TCP 交换 GID/QPN/PSN 的经典做法。
      SVG：手工 QP 状态迁移图。
- [ ] 4.4 **UD / DC** 处理一对多与连接爆炸；DCT 的取舍。

---

## 阶段五 · 可靠性与生产化

> 目标：教会如何在真实故障下不丢数据、不悬挂。

- [ ] 5.1 错误完成处理：`IBV_WC_*` 错误码、QP 进入 ERROR 态后的恢复/重建。
- [ ] 5.2 重传与超时：`retry_cnt` / `rnr_retry` / `timeout` 调参与含义。
- [ ] 5.3 **拥塞控制**：PFC、ECN、DCQCN 概览与可观测计数器。SVG：DCQCN 反馈环。
- [ ] 5.4 资源生命周期与泄漏排查：MR/QP/CQ 销毁顺序、`fork()` 与 `madvise` 陷阱。

---

## 阶段六 · 高级内存管理

- [ ] 6.1 **注册开销与缓存**：reg/dereg 成本、注册缓存（registration cache）设计。
- [ ] 6.2 **ODP（On-Demand Paging）**：免 pin 内存、缺页处理与代价。
- [ ] 6.3 **Memory Windows**：动态细粒度授权 rkey，缩小攻击面。
- [ ] 6.4 **HugePage / 大 MR / 连续内存**对 MTT 命中率与 TLB 的影响。

---

## 阶段七 · 与上层系统集成（实战落地）

- [ ] 7.1 在 RDMA 上手写一个**极简 RPC**：请求/响应缓冲环 + 单边写 + IMM 通知。
- [ ] 7.2 **GPUDirect RDMA** 概念与 `IBV_ACCESS_*` + peer-memory（仅原理 + 伪代码）。
- [ ] 7.3 生态总览：NVMe-oF、NCCL/集合通信、SPDK、UCX —— 各自如何用上述原语。
- [ ] 7.4 **Soft-RoCE(`rdma_rxe`) 完整实验环境**搭建脚本，无硬件也能跑全部示例。

---

## 阶段八 · 调试与可观测性（工具箱）

- [ ] 8.1 `ibv_devinfo` / `rdma` (iproute2) / `ibstat` 读懂设备能力。
- [ ] 8.2 计数器：`/sys/class/infiniband/*/ports/*/counters`、`hw_counters`。
- [ ] 8.3 抓包与追踪：`ibdump`、`tcpdump`(RoCE)、`perf`、provider 调试日志。
- [ ] 8.4 常见故障决策树：建链失败 / RNR / 完成错误 / 性能不达标。SVG：排障流程图。

---

## 文档与仓库改造任务

- [x] R.1 改写 `README.md` 顶部定位：由「初学者最小示例」→「进阶系统教程」。
- [x] R.2 `CLAUDE.md` 增补「教程导航」与各 `examples/` 索引，保持每节一张 SVG。
- [x] R.3 新建 `examples/` 目录结构与共享 `common/` 脚手架。
- [x] R.4 顶层 `Makefile` 支持递归构建所有 `examples/`。
- [ ] R.5 增加 `docs/` 放术语表、参考文献（IB Spec、RoCEv2、各 vendor 手册）。
- [ ] R.6 CI（可选）：在 Soft-RoCE 环境冒烟测试每个示例可建链。

---

## 推进顺序建议

1. 先做 **R.1–R.4 + 阶段二**：把现有 demo 收编为 `examples/01`，立起骨架。
2. 再做 **阶段一**理论篇与 **阶段三**性能篇（高级工程师价值最高）。
3. 之后按 **四 → 五 → 六 → 七 → 八** 逐步深入。
4. 每完成一阶段，回填 `CLAUDE.md` 导航与本 TODO 勾选。
