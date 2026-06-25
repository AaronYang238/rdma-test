# RDMA 由浅入深 —— 从小白到专家的系统教材

本仓库是一套 **可从头读到尾的 RDMA 中文教材 + 8 个可运行示例**：从一个最小示例
起步，逐级深入到硬件模型、性能工程、可扩展架构、可靠性、高级内存管理、系统集成、
调试，直到 mlx5dv/DEVX、DPU 卸载、现代拥塞控制等专家深水区。

> 📖 **教程正文入口：[`docs/book/00-前言与导读.md`](docs/book/00-前言与导读.md)**
> ——含全书目录与三条阅读路径（🌱 小白线 / 🚀 高级线 / 🔍 速查线）。
>
> **前置知识**：会写 C、用过 TCP socket、了解基本 Linux 命令即可，不需要任何
> RDMA 背景。

---

## 快速上手（30 秒环境 + 跑通第一个示例）

没有 RDMA 网卡也能学：用 Soft-RoCE（`rdma_rxe` 内核模块，纯软件模拟 RoCEv2）。

```bash
# 1. 装依赖（Debian/Ubuntu；RHEL 系见教材第 2 章）
sudo apt install -y build-essential libibverbs-dev librdmacm-dev rdma-core

# 2. 用 Soft-RoCE 搭环境（内核 ≥ 5.8 内置 rdma_rxe）
sudo modprobe rdma_rxe
sudo rdma link add rxe0 type rxe netdev eth0   # eth0 换成你的网卡名
ibv_devinfo                                     # 应看到 rxe0，state PORT_ACTIVE

# 3. 构建并运行示例 01（<IP> 填该网卡的 IP，不要用 127.0.0.1）
make
cd examples/01-write-demo
./bin/server <IP> 7471   # 终端1
./bin/client <IP> 7471   # 终端2
```

完整的环境搭建、运行说明与「读懂第一个输出」见教材
[第 2 章 · 30 分钟跑起来](docs/book/part1-02-quickstart.md)。

---

## 仓库结构

- `docs/book/`：**教材正文**（17 章 + 2 附录），从这里开始读。
- `examples/NN-*/`：由浅入深的独立可运行示例（`server.c` / `client.c` / `README.md` / `Makefile`）。
  示例 ↔ 章节映射见 [附录 B](docs/book/appendix-b-examples.md)。
- `common/`：示例共享脚手架（错误处理、CQ 轮询、计时）与编译规则。
- `docs/img/`：各章节与示例的 SVG 原理图（单独文件，正文以链接导入）。
- `docs/stage*.md`、`docs/glossary.md`：历史存根，指向 book 内对应章节（兼容旧链接）。
- `src/`：最初的单体 demo，已收编为 `examples/01-write-demo/`，保留作历史参照。
- `CLAUDE.md`：项目工作约定。`TODO.md`：编写历史与后续可扩展方向。

---

## 构建

```bash
make            # 顶层递归构建 examples/ 下所有示例
make list       # 列出可构建示例
```

每个示例生成到各自的 `examples/<名字>/bin/`。

---

## 注意事项

- 本仓库面向教学，示例优先清晰可读，未处理复杂错误恢复、重试、心跳、连接重建
  （这些在教材第 13 章「可靠性与生产化」中讲解）。
- `127.0.0.1` 回环地址通常不映射到 RDMA 设备，会出现
  `rdma_create_ep: No such device`；请使用 RDMA 网卡对应的 IP。
- Soft-RoCE 延迟较高（~50µs）、不能用于性能基准，但功能完整，是学习与验证逻辑的
  理想环境。
