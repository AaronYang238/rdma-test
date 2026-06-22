# RDMA 系统教程（由浅入深，小白 → 高级工程师）

本仓库是一套**由浅入深的 RDMA 系统教程**：从一个最小可运行示例起步，逐级深入到
硬件模型、性能工程、可扩展架构、可靠性、高级内存管理、系统集成与调试。配套
**5 个可运行示例 + 8 个理论阶段 + 40+ 张 SVG 原理图 + 术语表**。

起步示例（`examples/01-write-demo/`）包含两个程序：

- `server`：等待连接，接收客户端内存信息，执行一次 `RDMA Write` 写入客户端内存。
- `client`：建立连接，注册可远端写入内存，发送本地 MR 信息，接收 ACK 并打印被远端写入后的内容。

### 📚 学习路径（建议按序）

> **前置知识**：会写 C、用过 TCP socket、了解基本 Linux 命令即可，不需要任何 RDMA 背景。

1. **环境准备** → 本 README §3、§3.5（没有硬件就用 Soft-RoCE）。
2. **原理入门** → [`CLAUDE.md`](./CLAUDE.md) 第 1–8 节（内核旁路、PD/MR/QP/CQ、SEND/RECV、WRITE/READ、post/poll）。
3. **动手实践** → [`examples/`](./examples/) 01→05（WRITE / 双边乒乓 / READ / IMM / ATOMIC）。
4. **进阶专题** → `docs/` 各阶段：[硬件模型](docs/stage1-hardware-model.md) → [性能工程](docs/stage3-performance.md) → [可扩展架构](docs/stage4-scalability.md) → [可靠性](docs/stage5-reliability.md) → [内存管理](docs/stage6-memory.md) → [系统集成](docs/stage7-integration.md) → [调试](docs/stage8-debugging.md)。
5. **随手查** → [术语表 `docs/glossary.md`](docs/glossary.md)。

> `TODO.md` 是面向贡献者的编写路线图与施工清单；普通读者按上面的学习路径即可。

> 📘 **想系统学习 RDMA？** 请阅读配套教程 **[CLAUDE.md](./CLAUDE.md)**：它以本仓库
> 代码为实例，逐节讲解 RDMA 的**主要原理**（内核旁路 / 零拷贝、PD/MR/QP/CQ、
> lkey/rkey）、**编程使用**（post/poll、SEND/RECV 双边、WRITE/READ 单边）与
> **代码实现**，并为**每一节配有一张 SVG 示意图**。下方 README 提供 API 速查与
> 运行说明。

## 1. 你会看到的核心 RDMA API

### 连接管理（rdma_cm）

- `rdma_getaddrinfo`：解析地址与端口。
- `rdma_create_ep`：创建带 QP 的端点（简化了 event channel + 手工 QP 创建流程）。
- `rdma_listen` / `rdma_get_request` / `rdma_accept`：服务端监听、接入、接受连接。
- `rdma_connect`：客户端发起连接。
- `rdma_disconnect`：主动断开连接。
- `rdma_destroy_ep`：销毁端点。

### 内存管理与注册（verbs）

- `rdma_reg_msgs`：注册消息收发缓冲区（SEND/RECV 用）。
- `ibv_reg_mr`：注册数据缓冲区，拿到 `lkey/rkey`。本例客户端设置：
  - `IBV_ACCESS_LOCAL_WRITE`
  - `IBV_ACCESS_REMOTE_WRITE`
  - `IBV_ACCESS_REMOTE_READ`

### 数据通道与完成队列

- `rdma_post_recv`：预投递接收 WQE。
- `rdma_post_send`：发送控制消息（MR 元数据与 ACK）。
- `rdma_post_write`：执行一边操作（one-sided）把服务端数据直接写入客户端内存。
- `rdma_get_send_comp` / `rdma_get_recv_comp`：等待 send/recv 完成。

## 2. 为什么要用两种通信

本示例刻意混合两种模式，帮助理解 RDMA 常见实践：

- 双边（two-sided）`SEND/RECV`：用于小控制面数据（如地址、rkey、状态）。
- 单边（one-sided）`RDMA Write`：用于高吞吐数据面，减少远端 CPU 参与。

实际系统里常见模式是：

1. 先走 SEND/RECV 做握手（交换 MR、协议版本、任务描述）。
2. 再走 RDMA Read/Write 进行高性能数据传输。

## 3. 安装依赖（Anolis/RHEL/CentOS 系）

```bash
dnf install -y gcc make pkgconf-pkg-config rdma-core-devel libibverbs-devel librdmacm-devel
# Debian/Ubuntu：apt install -y build-essential libibverbs-dev librdmacm-dev rdma-core
```

## 3.5 没有 RDMA 网卡？先用 Soft-RoCE 30 秒搭好环境（强烈建议先做）

本教程**所有示例**都能在 Soft-RoCE（`rdma_rxe` 内核模块，纯软件模拟 RoCEv2）上
跑通，无需任何 RDMA 硬件。普通笔记本 / 云主机 / 虚拟机均可：

```bash
# 1. 加载模块（内核 ≥ 5.8 内置）
sudo modprobe rdma_rxe

# 2. 绑定到一张以太网口（用 `ip link` 查看真实网卡名，替换 eth0）
sudo rdma link add rxe0 type rxe netdev eth0

# 3. 验证：应看到 rxe0，且 state 为 PORT_ACTIVE
ibv_devinfo
rdma link show
```

之后下面的运行命令里，`<RDMA网卡IP>` 直接填该以太网口的 IP（`ip addr show eth0`）。
Soft-RoCE 延迟较高（~50µs）、不能用于性能基准，但功能完整，是学习与验证逻辑的
理想环境。深入说明（含 Docker/容器、限制）见 [`docs/stage7-integration.md`](docs/stage7-integration.md) §7.4。

## 4. 构建

```bash
make            # 顶层递归构建 examples/ 下所有示例
make list       # 列出可构建示例
```

每个示例生成到各自的 `examples/<名字>/bin/`。

## 5. 本机回环演示（以示例 01 为例）

```bash
cd examples/01-write-demo && make
./bin/server <RDMA网卡IP> 7471   # 终端1
./bin/client <RDMA网卡IP> 7471   # 终端2
```

预期看到：

- 服务端打印收到客户端 MR 信息，并显示 write + ack 完成。
- 客户端打印本地缓冲区被服务端覆盖后的字符串。

其余示例（双边乒乓延迟、RDMA Read 等）见 [`examples/`](./examples/) 索引。

## 6. 代码结构

- `common/rdma_common.h`：共享结构、错误处理、CQ 轮询与计时脚手架。
- `common/rules.mk`：示例共用的编译规则。
- `examples/NN-*/`：由浅入深的独立示例（server.c / client.c / README.md / Makefile）。
- `docs/img/`：各章节与示例的 SVG 图（单独文件，正文以链接导入）。
- `src/`：最初的单体 demo，已收编为 `examples/01-write-demo/`，保留作历史参照。
- `TODO.md`：进阶教程编写路线图。`CLAUDE.md`：原理教程正文。

## 7. 注意事项

- 本示例用于教学，未处理复杂错误恢复、重试、心跳、连接重建。
- `127.0.0.1` 回环地址通常不映射到 RDMA 设备，可能出现 `rdma_create_ep: No such device`；请使用 RDMA 网卡对应的 IP。
- 在没有 RDMA NIC 的机器上，可能需要 Soft-RoCE（`rdma_rxe`）或特定内核网络配置，运行时能否成功取决于系统能力。
- 生产环境还需要考虑：
  - CQ 轮询与事件模式切换
  - 多 QP 并发
  - 流控与拥塞控制
  - MR 生命周期与内存池
  - 安全边界（限制 rkey 暴露范围）

## 8. 进一步扩展建议

1. 增加 `RDMA Read` 路径，对比 read/write 延迟。
2. 增加 ping-pong 压测（QPS、P99、带宽）。
3. 用环形缓冲 + doorbell batching 做吞吐优化。
4. 将控制面改成 protobuf/flatbuffers。
