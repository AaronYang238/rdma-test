# examples/ —— 由浅入深的 RDMA 示例集

每个子目录是一个**独立可编译运行**的示例，配 `README.md` 与（位于 `docs/img/` 的）
SVG 图，后面的示例复用 `common/` 脚手架并在前面的基础上递进。

| 示例 | 主题 | 核心 verbs | 状态 |
|------|------|------------|------|
| [01-write-demo](01-write-demo/) | 控制面 + RDMA WRITE 数据面（基线） | `post_send/recv`, `post_write` | ✅ |
| [02-send-recv](02-send-recv/) | 双边乒乓 + 延迟基准 | `post_send/recv` | ✅ |
| [03-read](03-read/) | RDMA READ 单边读 | `post_read` | ✅ |
| [04-immediate](04-immediate/) | `WRITE_WITH_IMM` 写+通知合一 | `ibv_post_send` (IMM) | ✅ |
| [05-atomic](05-atomic/) | `FETCH_ADD` / `CMP_SWAP` 原子操作 | `ibv_post_send` (atomic) | ✅ |
| [06-selective-signaling](06-selective-signaling/) | 选择性 signaling 吞吐对比（第 11 章） | `ibv_post_send`, `ibv_poll_cq` | ✅ |
| [07-srq](07-srq/) | SRQ 多 QP 共享接收队列（第 12 章） | `ibv_create_srq`, `rdma_create_qp` | ✅ |
| [08-rpc](08-rpc/) | 极简请求/响应 RPC + 延迟基准（第 15 章） | `post_send/recv` | ✅ |

> 01–05 配套教材第一部分入门（第 1–9 章）；06–08 把进阶/专家章节（第 11/12/15 章）的
> 关键优化做成可动手运行的对比实验。

构建全部：

```bash
make            # 顶层递归构建所有示例（需 RDMA 开发库）
make list       # 列出可构建示例
make clean
```

> 各示例需 RDMA 网卡或 Soft-RoCE 环境运行；依赖见根目录 `README.md`。
> 完整编写路线图见根目录 `TODO.md`。
