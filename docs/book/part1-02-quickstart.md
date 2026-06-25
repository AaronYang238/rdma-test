# 第 2 章 · 30 分钟跑起来

> 上一章我们建立了直觉：RDMA 让网卡直接搬内存，绕过内核、绕过对端 CPU。
> 但光说不练总隔着一层。这一章的目标很务实：**哪怕你手上只有一台普通笔记本或
> 云主机，也能在 30 分钟内跑起本书的第一个 RDMA 程序，并亲眼看到效果。**

## 本章你将遇到的术语（预览）

- **Soft-RoCE / rdma_rxe**：一个纯软件实现的 RDMA 网卡，让没有 RDMA 硬件的机器
  也能跑 RDMA 程序（功能完整，只是慢）。
- **ibv_devinfo**：列出系统里有哪些 RDMA 设备、状态如何的命令行工具。
- **server / client**：示例 01 的两个程序，分别扮演数据的"发送方"和"开放内存的一方"。
- **ACK**：服务端写完数据后给客户端发的一个"我写好了"的小通知。

## 场景 / 问题引入

你读完第 1 章，跃跃欲试，结果在网上一搜：「RDMA 需要 InfiniBand 网卡 / RoCE 网卡，
一块好几千」。心一凉——难道学个原理还得先买硬件？

好消息是：**不需要**。Linux 内核自带一个叫 `rdma_rxe` 的模块，它用纯软件在普通
以太网卡上模拟出一块 RDMA 网卡（这套方案叫 **Soft-RoCE**）。它跑得不快、不能用来
做性能基准，但**功能完整**——本书所有示例都能在它上面跑通，用来学原理、验证逻辑
绰绰有余。

## 直觉与类比

Soft-RoCE 就像**学开车时的驾校教练车**：它装了双刹、跑不快、不能上赛道，但
方向盘、油门、挡位一应俱全，你在上面学到的操作，换到真车上完全通用。我们先用
"教练车"把流程跑熟，将来换上真 RDMA 网卡，代码一行都不用改。

![Soft-RoCE 架构：软件模拟 RDMA 网卡](../img/s7-4-softroce.svg)

## 第一步：30 秒搭好 Soft-RoCE 环境

下面三条命令就能把"教练车"开出来（需要 root 权限，内核 ≥ 5.8 通常已内置模块）：

```bash
# 1. 加载 Soft-RoCE 内核模块
sudo modprobe rdma_rxe

# 2. 把一块以太网口"包装"成 RDMA 设备（用 `ip link` 查看真实网卡名，替换 eth0）
sudo rdma link add rxe0 type rxe netdev eth0

# 3. 验证：应能看到 rxe0，且端口 state 为 PORT_ACTIVE
ibv_devinfo
rdma link show
```

如果 `ibv_devinfo` 列出了 `rxe0` 并且状态是 `PORT_ACTIVE`，恭喜，你已经有一块
（虚拟的）RDMA 网卡了。后面命令里要填的 `<RDMA网卡IP>`，就是这块以太网口的 IP
（用 `ip addr show eth0` 查看）。

## 第二步：安装依赖

我们的示例用到 `libibverbs`（verbs 库）和 `librdmacm`（连接管理库），编译还需要
gcc / make：

```bash
# Anolis / RHEL / CentOS 系
dnf install -y gcc make pkgconf-pkg-config \
    rdma-core-devel libibverbs-devel librdmacm-devel

# Debian / Ubuntu 系
apt install -y build-essential libibverbs-dev librdmacm-dev rdma-core
```

## 第三步：构建并运行示例 01

仓库顶层 `make` 会递归构建所有示例，每个示例的产物落在各自的 `bin/` 下：

```bash
cd examples/01-write-demo && make    # 生成 bin/server 与 bin/client
```

构建与运行的整体流程如下图：

![构建与运行流程](../img/09-build-run.svg)

然后开**两个终端**，分别跑服务端和客户端（`<RDMA网卡IP>` 填上一步那块网卡的 IP）：

```bash
./bin/server <RDMA网卡IP> 7471   # 终端 1：先启动，进入监听
./bin/client <RDMA网卡IP> 7471   # 终端 2：发起连接
```

> 🛠 动手跑：[examples/01-write-demo/](../../examples/01-write-demo/)

## 第四步：读懂第一个输出

跑通后，服务端大致会打印：

```
[server] listening on <ip>:7471
[server] accepted connection
[server] remote MR: addr=0x..., rkey=..., size=1024
[server] write done, sent ack, disconnecting
```

客户端大致会打印：

```
[client] connected to <ip>:7471
[client] server note: RDMA write completed
[client] local buffer after remote write: "Hello from server via RDMA Write at pid=..."
```

**最值得玩味的是客户端最后一行**。客户端的本地缓冲区原本写的是
`"This text should be overwritten by server RDMA Write"`，但它**从头到尾没有调用
任何"接收数据"的函数**，那段内存却被悄悄改成了服务端的字符串。这正是上一章说的
**单边 WRITE**：服务端的网卡直接把数据 DMA 进了客户端内存，客户端 CPU 全程无感。

## 简化直觉版：刚才到底发生了什么

先不深入细节，建立一个整体画面就好。这次握手 + 传输大致分三步：

1. **交换名片（SEND/RECV，双边）**：客户端把"我的可写内存在哪、钥匙是什么、多大"
   通过 SEND 告诉服务端。这是控制面。
2. **直接写入（RDMA WRITE，单边）**：服务端拿着客户端给的地址和钥匙，让网卡把
   数据直接写进客户端内存。客户端 CPU 不参与。这是数据面。
3. **打个招呼（ACK）**：服务端再 SEND 一个小通知，告诉客户端"写好了"。客户端
   这才去看自己的缓冲区，发现已经被改写了。

整个端到端时序如下图。**现在你只需要记住这个"先握手、再搬数据、最后通知"的
三段式**，每一段背后的对象和 API，我们会在第 3～7 章逐一拆开讲：

![本示例端到端时序](../img/08-end-to-end.svg)

## 常见误区

- **用 `127.0.0.1` 跑，报 `rdma_create_ep: No such device`**。回环地址通常不映射
  到任何 RDMA 设备。请用你 Soft-RoCE 绑定的那块以太网口的真实 IP。
- **以为没有 RDMA 硬件就跑不了**。Soft-RoCE 就是为此而生，按第一步配置即可。
- **`rdma link add` 里的网卡名填错**。`eth0` 只是示例，务必用 `ip link` 查到你
  机器上真实存在的网卡名替换。
- **只开了一个终端**。server 和 client 是两个独立程序，要分别在两个终端运行，
  且先启动 server 再启动 client。

## 小结

- 没有 RDMA 硬件？用 **Soft-RoCE（rdma_rxe）** 三条命令搭出"教练车"，功能完整。
- 装好 `libibverbs` / `librdmacm` 依赖，`make` 构建，两个终端分别跑 server / client。
- 示例 01 的看点：客户端没"收数据"，内存却被改了——这就是单边 WRITE 的魔法。
- 一次完整交互是**三段式**：SEND/RECV 握手 → RDMA WRITE 搬数据 → SEND ACK 通知。

你已经亲眼见过 RDMA 工作了。但程序里那些 `pd`、`mr`、`rkey`、`post_send` 到底是
什么？**下一章，我们把 RDMA 编程绕不开的几个核心对象逐个认识一遍。**

## 术语速查

| 术语 | 含义 |
|------|------|
| Soft-RoCE / rdma_rxe | 纯软件模拟的 RDMA 网卡，无硬件也能跑 RDMA 程序 |
| ibv_devinfo | 列出系统 RDMA 设备及其状态的命令行工具 |
| PORT_ACTIVE | RDMA 端口处于可用状态的标志 |
| ACK | 服务端写完数据后发给客户端的"完成"通知 |
| make | 构建工具，本仓库用它编译各示例 |
