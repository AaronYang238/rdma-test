# examples/ —— 由浅入深的 RDMA 示例集

每个子目录是一个**独立可编译运行**的示例，配 `README.md` 与（位于 `docs/img/` 的）
SVG 图，后面的示例复用 `common/` 脚手架并在前面的基础上递进。

| 示例 | 主题 | 核心 verbs | 状态 |
|------|------|------------|------|
| [01-write-demo](01-write-demo/) | 控制面 + RDMA WRITE 数据面（基线） | `post_send/recv`, `post_write` | ✅ |
| [02-send-recv](02-send-recv/) | 双边乒乓 + 延迟基准 | `post_send/recv` | ✅ |
| [03-read](03-read/) | RDMA READ 单边读 | `post_read` | ✅ |
| 04-immediate | `WRITE_WITH_IMM` / `SEND_WITH_IMM` | （规划中） | ⏳ |
| 05-atomic | `FETCH_ADD` / `CMP_SWAP` | （规划中） | ⏳ |

构建全部：

```bash
make            # 顶层递归构建所有示例（需 RDMA 开发库）
make list       # 列出可构建示例
make clean
```

> 各示例需 RDMA 网卡或 Soft-RoCE 环境运行；依赖见根目录 `README.md`。
> 完整编写路线图见根目录 `TODO.md`。
