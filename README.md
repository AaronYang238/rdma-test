# RDMA Demo for Beginners

这个项目提供一个面向初学者的 RDMA 最小示例，包含两个程序：

- `rdma_server`：等待连接，接收客户端内存信息，执行一次 `RDMA Write` 写入客户端内存。
- `rdma_client`：建立连接，注册可远端写入内存，发送本地 MR 信息，接收 ACK 并打印被远端写入后的内容。

> 🚧 **项目定位升级中**：本仓库正从「面向初学者的 RDMA 最小示例」演进为
> **「由浅入深、面向高级工程师的 RDMA 系统教程」**——覆盖硬件模型、性能工程、
> 可扩展架构、可靠性与生产化、高级内存管理及上层系统集成。下方当前 demo 将被
> 收编为教程的第一个示例（`examples/01-write-demo/`）。编写路线图与施工清单见
> **[TODO.md](./TODO.md)**。

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
```

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
