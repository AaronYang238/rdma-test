# CLAUDE.md — 项目指引

> 本文件是 **Claude Code 的项目工作约定**。RDMA 的教学正文已整理成一本
> 「由浅入深、从小白到专家」的教材，**入口见
> [`docs/book/00-前言与导读.md`](docs/book/00-前言与导读.md)**。

---

## 1. 项目定位

本仓库是一套 **RDMA 系统教材 + 可运行示例**：

- **教材正文**：`docs/book/`，分三部分 17 章 + 2 附录，连续编号、可线性阅读
  （入门 ch1–9 / 进阶 ch10–14 / 专家 ch15–17）。总入口与阅读路径见
  [`docs/book/00-前言与导读.md`](docs/book/00-前言与导读.md)。
- **可运行示例**：`examples/01`–`08`，全部可在 Soft-RoCE（无需 RDMA 硬件）上跑通。
  示例与章节的映射见 [`docs/book/appendix-b-examples.md`](docs/book/appendix-b-examples.md)。
- **原理图**：`docs/img/` 下的 SVG，每节至少一张。
- `docs/stage*.md`、`docs/glossary.md` 为**历史存根**，仅指向 book 内对应章节，
  兼容旧链接；正文以 `docs/book/` 为准。
- `src/` 是最初的单体 demo，已收编为 `examples/01-write-demo/`，保留作历史参照。

---

## 2. 构建、运行与调试

```bash
make            # 顶层递归构建 examples/ 下所有示例
make list       # 列出可构建示例

cd examples/01-write-demo && make
./bin/server <RDMA网卡IP> 7471   # 终端1
./bin/client <RDMA网卡IP> 7471   # 终端2
```

**常见问题**：
- `rdma_create_ep: No such device`：用了 `127.0.0.1`；请改用 RDMA 网卡 IP。
- 无物理 RDMA 网卡：用 Soft-RoCE（`rdma_rxe` 内核模块）模拟，30 秒搭建步骤见
  [第 2 章 · 30 分钟跑起来](docs/book/part1-02-quickstart.md)。
- 依赖（Debian/Ubuntu）：`apt install -y build-essential libibverbs-dev librdmacm-dev rdma-core`；
  （Anolis/RHEL/CentOS）：`dnf install -y gcc make rdma-core-devel libibverbs-devel librdmacm-devel`。

---

## 3. 给 Claude 的工作约定

- **代码风格**：C11，4 空格缩进，沿用现有 `die_rdma` / `check_zero` 错误处理与
  `wait_send_comp` / `wait_recv_comp` 模式（见 `common/rdma_common.h`）；新增 verbs
  调用务必检查返回值与 `wc.status`。
- **目录**：示例源码在 `examples/NN-*/`，公共脚手架在 `common/`，产物在各自
  `bin/`（已被 `.gitignore` 忽略）。
- **改动后**：`make` 必须能通过；涉及收发逻辑时同步更新 server 与 client 两端。
- **教学定位**：本仓库面向「小白 → 专家」的渐进学习，**文风优先清晰可读、循序
  渐进**（先用后定义、问题引入 + 类比，避免术语先于解释、避免速查表堆砌）。
- **文档**：行为变化时同步更新对应的 `docs/book/` 章节；新增章节保持「**每节配
  一张 SVG 图**」的约定。教材每章遵循统一七段式结构：术语预览 → 问题引入 →
  直觉类比 → 概念逐个展开 → 代码与示例 → 常见误区 → 小结 + 术语速查。
- **配图（强制约束）**：教材的**每一节都必须配至少一张 SVG 示意图**，刻画原理、
  数据路径、状态机或时序。**SVG 一律单独保存为文件**（统一放在 `docs/img/` 下，
  按 `NN-topic.svg` 命名），正文中**不内置 `<svg>`**，改用 Markdown 图片链接导入。
  `docs/book/` 下的章节引用图片用 `../img/NN-topic.svg` 相对路径。**新增或重写
  任何章节/小节时，缺图视为未完成**，不得只有文字。图需与正文术语一致，并随内容
  变化同步更新对应 `.svg` 文件。
- **分支**：在指定的开发分支上提交，非必要不创建 PR。
