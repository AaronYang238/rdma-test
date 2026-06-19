# CLAUDE.md — RDMA 原理与实战教程

> 本文件既是 Claude Code 的项目指引，也是一份完整的 **RDMA 教程**。
> 以本仓库的 `src/server.c`、`src/client.c`、`src/common.h` 为实例，逐层讲解
> RDMA 的**主要原理**、**编程使用**与**代码实现**。
>
> 约定：**每一节都附带一张 SVG 示意图**（内联 SVG，可在支持 SVG 的 Markdown
> 预览器 / 编辑器中直接渲染）。
>
> 🎯 **项目定位**：本仓库正由「初学者最小示例」升级为**「由浅入深、面向高级
> 工程师的 RDMA 系统教程」**。本文件第 1–8 节是入门主线；进阶篇（硬件模型、
> 性能工程、可扩展架构、可靠性、高级内存管理、系统集成、调试）的编写路线图与
> 施工清单见 **[TODO.md](./TODO.md)**，落地后会在此回填导航。

---

## 目录

1. [RDMA 是什么：内核旁路与零拷贝](#1-rdma-是什么内核旁路与零拷贝)
2. [核心对象：PD / MR / QP / CQ / WR](#2-核心对象pd--mr--qp--cq--wr)
3. [内存注册：lkey 与 rkey](#3-内存注册lkey-与-rkey)
4. [连接建立：rdma_cm 控制流](#4-连接建立rdma_cm-控制流)
5. [双边操作：SEND / RECV](#5-双边操作send--recv)
6. [单边操作：WRITE / READ](#6-单边操作write--read)
7. [完成机制：post 与 poll](#7-完成机制post-与-poll)
8. [本项目端到端时序](#8-本项目端到端时序)
9. [构建、运行与调试](#9-构建运行与调试)
10. [给 Claude 的工作约定](#10-给-claude-的工作约定)

---

## 1. RDMA 是什么：内核旁路与零拷贝

**RDMA（Remote Direct Memory Access）** 让一台机器的网卡直接读写另一台机器的
内存，**绕过对端 CPU、绕过内核协议栈、避免数据多次拷贝**。

传统 TCP/IP 每次收发都要经过：用户态 → 内核 socket 缓冲 → 协议栈 → 网卡，并触发
系统调用与中断。RDMA 则在连接建立后，由 **网卡（RNIC）直接 DMA 访问已注册内存**，
数据路径完全不打扰 CPU 与内核。

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="720" height="300" font-family="sans-serif" font-size="13">
  <rect width="720" height="300" fill="#fbfbfd"/>
  <!-- TCP side -->
  <text x="170" y="24" text-anchor="middle" font-weight="bold">传统 TCP/IP</text>
  <rect x="60" y="40" width="220" height="34" fill="#e3f2fd" stroke="#1976d2"/>
  <text x="170" y="62" text-anchor="middle">应用 (用户态 buffer)</text>
  <rect x="60" y="86" width="220" height="34" fill="#fff3e0" stroke="#ef6c00"/>
  <text x="170" y="108" text-anchor="middle">系统调用 / socket 拷贝</text>
  <rect x="60" y="132" width="220" height="34" fill="#fff3e0" stroke="#ef6c00"/>
  <text x="170" y="154" text-anchor="middle">内核 TCP/IP 协议栈</text>
  <rect x="60" y="178" width="220" height="34" fill="#ede7f6" stroke="#5e35b1"/>
  <text x="170" y="200" text-anchor="middle">网卡 (NIC)</text>
  <line x1="170" y1="74" x2="170" y2="86" stroke="#555" marker-end="url(#a)"/>
  <line x1="170" y1="120" x2="170" y2="132" stroke="#555" marker-end="url(#a)"/>
  <line x1="170" y1="166" x2="170" y2="178" stroke="#555" marker-end="url(#a)"/>
  <text x="170" y="240" text-anchor="middle" fill="#c62828">CPU 全程参与 · 多次拷贝 · 中断</text>

  <!-- RDMA side -->
  <text x="540" y="24" text-anchor="middle" font-weight="bold">RDMA</text>
  <rect x="430" y="40" width="220" height="34" fill="#e3f2fd" stroke="#1976d2"/>
  <text x="540" y="62" text-anchor="middle">应用 (已注册 MR)</text>
  <rect x="430" y="178" width="220" height="34" fill="#ede7f6" stroke="#5e35b1"/>
  <text x="540" y="200" text-anchor="middle">网卡 (RNIC) — DMA</text>
  <path d="M540 74 C 700 100, 700 150, 540 178" fill="none" stroke="#2e7d32" stroke-width="2" marker-end="url(#a)"/>
  <text x="615" y="130" fill="#2e7d32">旁路内核</text>
  <rect x="430" y="92" width="220" height="68" fill="none" stroke="#bbb" stroke-dasharray="4"/>
  <text x="540" y="130" text-anchor="middle" fill="#999">内核/拷贝被跳过</text>
  <text x="540" y="240" text-anchor="middle" fill="#2e7d32">Kernel Bypass · Zero Copy · 低延迟</text>
  <defs><marker id="a" markerWidth="8" markerHeight="8" refX="6" refY="3" orient="auto"><path d="M0,0 L6,3 L0,6 z" fill="#555"/></marker></defs>
</svg>
```

**三种数据传输语义**（本教程逐一讲解）：

| 语义 | 谁参与 | 典型用途 |
|------|--------|----------|
| SEND / RECV（双边） | 收发双方 CPU 都参与 | 控制面：握手、交换元数据 |
| WRITE（单边） | 仅发起方 CPU | 数据面：把数据推入对端内存 |
| READ（单边） | 仅发起方 CPU | 数据面：从对端内存拉数据 |

---

## 2. 核心对象：PD / MR / QP / CQ / WR

RDMA 编程围绕几个核心对象。本仓库通过 `rdma_create_ep`（`server.c:119`、
`client.c:120`）一次性创建了 `rdma_cm_id`，它内部封装了 **PD + QP + CQ**。

- **PD（Protection Domain）**：保护域，MR 与 QP 的归属边界。
- **MR（Memory Region）**：注册过的内存，网卡才能访问，见 `register_memory()`。
- **QP（Queue Pair）**：发送队列 SQ + 接收队列 RQ。本项目用 `IBV_QPT_RC`
  （可靠连接，见 `setup_qp_attr()` `server.c:21`）。
- **CQ（Completion Queue）**：操作完成后投递 CQE（完成事件）。
- **WR / WQE（Work Request / Work Queue Element）**：一次操作请求，`post_*`
  把 WR 放进队列。

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="720" height="330" font-family="sans-serif" font-size="13">
  <rect width="720" height="330" fill="#fbfbfd"/>
  <rect x="20" y="20" width="680" height="290" fill="none" stroke="#90a4ae" stroke-dasharray="5"/>
  <text x="36" y="42" fill="#546e7a" font-weight="bold">PD (Protection Domain)</text>

  <!-- MR -->
  <rect x="40" y="60" width="180" height="80" fill="#e8f5e9" stroke="#2e7d32"/>
  <text x="130" y="86" text-anchor="middle" font-weight="bold">MR</text>
  <text x="130" y="106" text-anchor="middle">注册内存</text>
  <text x="130" y="124" text-anchor="middle" font-size="11">lkey / rkey</text>

  <!-- QP -->
  <rect x="270" y="60" width="200" height="160" fill="#e3f2fd" stroke="#1565c0"/>
  <text x="370" y="84" text-anchor="middle" font-weight="bold">QP (Queue Pair)</text>
  <rect x="290" y="100" width="160" height="44" fill="#fff" stroke="#1565c0"/>
  <text x="370" y="127" text-anchor="middle">SQ — Send Queue</text>
  <rect x="290" y="156" width="160" height="44" fill="#fff" stroke="#1565c0"/>
  <text x="370" y="183" text-anchor="middle">RQ — Recv Queue</text>

  <!-- CQ -->
  <rect x="520" y="60" width="160" height="160" fill="#fff3e0" stroke="#ef6c00"/>
  <text x="600" y="84" text-anchor="middle" font-weight="bold">CQ</text>
  <rect x="540" y="100" width="120" height="26" fill="#fff" stroke="#ef6c00"/>
  <text x="600" y="118" text-anchor="middle" font-size="11">CQE (完成事件)</text>
  <rect x="540" y="134" width="120" height="26" fill="#fff" stroke="#ef6c00"/>
  <text x="600" y="152" text-anchor="middle" font-size="11">CQE</text>

  <!-- WR arrows -->
  <text x="130" y="180" text-anchor="middle" font-size="11" fill="#555">post_send/write/read → SQ</text>
  <text x="130" y="200" text-anchor="middle" font-size="11" fill="#555">post_recv → RQ</text>
  <line x1="220" y1="122" x2="290" y2="122" stroke="#2e7d32" stroke-dasharray="3"/>
  <text x="245" y="115" font-size="10" fill="#2e7d32">引用</text>
  <line x1="450" y1="140" x2="520" y2="140" stroke="#ef6c00" marker-end="url(#b)"/>
  <text x="485" y="133" font-size="10" fill="#ef6c00">完成</text>
  <text x="370" y="250" text-anchor="middle" fill="#555">poll_cq / get_*_comp 从 CQ 取完成 →</text>
  <defs><marker id="b" markerWidth="8" markerHeight="8" refX="6" refY="3" orient="auto"><path d="M0,0 L6,3 L0,6 z" fill="#ef6c00"/></marker></defs>
</svg>
```

---

## 3. 内存注册：lkey 与 rkey

网卡只能访问**注册过的内存**。注册做两件事：把内存页 **pin 住**（防止换出），
并在网卡 MTT/MPT 表中建立映射，返回一个 `ibv_mr`，其中：

- `lkey`（local key）：**本端**在 SQ/RQ 的 WR 中引用该内存。
- `rkey`（remote key）：**对端**做 RDMA READ/WRITE 时用来寻址 + 鉴权。

本项目代码（`client.c:70`）：

```c
ctx->data_mr = ibv_reg_mr(ctx->id->pd, ctx->remote_writable_data,
                          sizeof(ctx->remote_writable_data),
                          IBV_ACCESS_LOCAL_WRITE |
                          IBV_ACCESS_REMOTE_WRITE |   // 允许对端写
                          IBV_ACCESS_REMOTE_READ);    // 允许对端读
```

权限标志决定对端能干什么：服务端只需 `IBV_ACCESS_LOCAL_WRITE`
（`server.c:72`，它只做本地源缓冲）；客户端开放 `REMOTE_WRITE` 才允许服务端
RDMA Write 进来。`rdma_reg_msgs` 是 `ibv_reg_mr` 的便捷封装，自动加
`LOCAL_WRITE`，用于 SEND/RECV 控制消息。

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="720" height="270" font-family="sans-serif" font-size="13">
  <rect width="720" height="270" fill="#fbfbfd"/>
  <rect x="40" y="50" width="200" height="150" fill="#e8f5e9" stroke="#2e7d32"/>
  <text x="140" y="40" text-anchor="middle" font-weight="bold">应用虚拟内存 buffer</text>
  <text x="140" y="90" text-anchor="middle">remote_writable_data[]</text>
  <line x1="240" y1="125" x2="340" y2="125" stroke="#555" marker-end="url(#c)"/>
  <text x="290" y="116" text-anchor="middle" font-size="11">ibv_reg_mr</text>

  <rect x="340" y="50" width="200" height="150" fill="#ede7f6" stroke="#5e35b1"/>
  <text x="440" y="40" text-anchor="middle" font-weight="bold">网卡映射表 (MPT/MTT)</text>
  <text x="440" y="85" text-anchor="middle">pin 物理页</text>
  <text x="440" y="110" text-anchor="middle">虚拟→物理映射</text>
  <rect x="360" y="130" width="160" height="26" fill="#fff" stroke="#5e35b1"/>
  <text x="440" y="148" text-anchor="middle">lkey (本端用)</text>
  <rect x="360" y="162" width="160" height="26" fill="#fff" stroke="#5e35b1"/>
  <text x="440" y="180" text-anchor="middle">rkey (给对端)</text>

  <path d="M520 175 C 620 175, 620 110, 600 90" fill="none" stroke="#c62828" stroke-width="2" marker-end="url(#c)"/>
  <text x="630" y="140" text-anchor="middle" font-size="11" fill="#c62828">rkey + addr</text>
  <text x="630" y="158" text-anchor="middle" font-size="11" fill="#c62828">经 SEND</text>
  <text x="630" y="176" text-anchor="middle" font-size="11" fill="#c62828">发给对端</text>
  <defs><marker id="c" markerWidth="8" markerHeight="8" refX="6" refY="3" orient="auto"><path d="M0,0 L6,3 L0,6 z" fill="#555"/></marker></defs>
</svg>
```

`addr + rkey + size` 正是本项目 `struct control_message`（`common.h:18`）要
传递的内容。

---

## 4. 连接建立：rdma_cm 控制流

本项目用 **librdmacm（rdma_cm）** 简化连接管理，它把"建立 TCP 风格连接 + 创建
QP"封装成类 socket 的 API。

- 服务端：`rdma_getaddrinfo` → `rdma_create_ep` → `rdma_listen` →
  `rdma_get_request` → `rdma_accept`（`server.c:115-132`）。
- 客户端：`rdma_getaddrinfo` → `rdma_create_ep` → `rdma_connect`
  （`client.c:116-128`）。

**关键时序细节**：接收方必须在连接真正可用前 `rdma_post_recv` 预投递接收缓冲，
否则对端 SEND 到达时无处安放。本项目正是这样做的：服务端在 `rdma_accept` 之前
post_recv（`server.c:129`），客户端在 `rdma_connect` 之前 post_recv
（`client.c:125`）。

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="720" height="350" font-family="sans-serif" font-size="12">
  <rect width="720" height="350" fill="#fbfbfd"/>
  <text x="180" y="24" text-anchor="middle" font-weight="bold">Client</text>
  <text x="540" y="24" text-anchor="middle" font-weight="bold">Server</text>
  <line x1="180" y1="34" x2="180" y2="330" stroke="#1565c0" stroke-width="2"/>
  <line x1="540" y1="34" x2="540" y2="330" stroke="#5e35b1" stroke-width="2"/>

  <text x="540" y="58" text-anchor="middle" font-size="11">rdma_listen / get_request</text>
  <text x="540" y="80" text-anchor="middle" font-size="11" fill="#2e7d32">post_recv (预投递)</text>
  <text x="180" y="80" text-anchor="middle" font-size="11" fill="#2e7d32">post_recv (预投递)</text>

  <line x1="180" y1="110" x2="540" y2="110" stroke="#1565c0" marker-end="url(#d)"/>
  <text x="360" y="103" text-anchor="middle">rdma_connect →</text>
  <line x1="540" y1="150" x2="180" y2="150" stroke="#5e35b1" marker-end="url(#d)"/>
  <text x="360" y="143" text-anchor="middle">← rdma_accept</text>

  <rect x="300" y="170" width="120" height="30" fill="#e8f5e9" stroke="#2e7d32"/>
  <text x="360" y="190" text-anchor="middle" font-size="11">连接 ESTABLISHED</text>

  <text x="360" y="230" text-anchor="middle" fill="#555">此后进入数据阶段：</text>
  <text x="360" y="252" text-anchor="middle" fill="#555">SEND/RECV 交换 MR 元数据</text>
  <text x="360" y="274" text-anchor="middle" fill="#555">RDMA WRITE 推送数据</text>
  <text x="360" y="306" text-anchor="middle" font-size="11" fill="#c62828">rdma_disconnect / destroy_ep</text>
  <defs><marker id="d" markerWidth="8" markerHeight="8" refX="6" refY="3" orient="auto"><path d="M0,0 L6,3 L0,6 z" fill="#555"/></marker></defs>
</svg>
```

---

## 5. 双边操作：SEND / RECV

**双边（two-sided）**：发送方 `post_send`，接收方必须已 `post_recv`，**两端 CPU
都参与**，两端各产生一个 CQE。适合传输小而关键的控制信息。

本项目用 SEND/RECV 交换 MR 元数据与 ACK：

```c
// 客户端把自己的 addr/rkey/size 发给服务端 (client.c:131-143)
ctx.send_ctrl.addr = (uint64_t)(uintptr_t)ctx.remote_writable_data;
ctx.send_ctrl.rkey = ctx.data_mr->rkey;
ctx.send_ctrl.size = sizeof(ctx.remote_writable_data);
rdma_post_send(id, NULL, &ctx.send_ctrl, sizeof(ctx.send_ctrl),
               ctx.send_ctrl_mr, IBV_SEND_SIGNALED);

// 服务端在 RQ 取出该消息 (server.c:135)
wait_recv_comp(conn_id, "...");   // 得到 recv_ctrl.addr / rkey
```

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="720" height="250" font-family="sans-serif" font-size="12">
  <rect width="720" height="250" fill="#fbfbfd"/>
  <text x="160" y="24" text-anchor="middle" font-weight="bold">发送方</text>
  <text x="560" y="24" text-anchor="middle" font-weight="bold">接收方</text>

  <rect x="60" y="50" width="200" height="40" fill="#e3f2fd" stroke="#1565c0"/>
  <text x="160" y="75" text-anchor="middle">post_send(buf, lkey)</text>
  <rect x="460" y="50" width="200" height="40" fill="#e8f5e9" stroke="#2e7d32"/>
  <text x="560" y="75" text-anchor="middle">post_recv(buf, lkey) [先]</text>

  <line x1="260" y1="110" x2="460" y2="110" stroke="#1565c0" stroke-width="2" marker-end="url(#e)"/>
  <text x="360" y="102" text-anchor="middle" fill="#1565c0">网络传输 payload</text>
  <text x="360" y="128" text-anchor="middle" font-size="11" fill="#555">数据写入接收方 RQ 预置 buffer</text>

  <rect x="60" y="160" width="200" height="36" fill="#fff3e0" stroke="#ef6c00"/>
  <text x="160" y="183" text-anchor="middle" font-size="11">send CQE</text>
  <rect x="460" y="160" width="200" height="36" fill="#fff3e0" stroke="#ef6c00"/>
  <text x="560" y="183" text-anchor="middle" font-size="11">recv CQE</text>
  <text x="360" y="225" text-anchor="middle" fill="#c62828">两端 CPU 都被通知（双边）</text>
  <defs><marker id="e" markerWidth="9" markerHeight="9" refX="7" refY="3" orient="auto"><path d="M0,0 L7,3 L0,6 z" fill="#1565c0"/></marker></defs>
</svg>
```

---

## 6. 单边操作：WRITE / READ

**单边（one-sided）**：发起方提供 **对端的 addr + rkey**，网卡直接 DMA 访问对端
内存，**对端 CPU 完全不参与**，对端不产生 CQE。适合高吞吐数据面。

本项目服务端用 RDMA Write 把数据直接写进客户端内存（`server.c:139`）：

```c
rdma_post_write(conn_id, NULL,
                ctx.local_data, strlen(ctx.local_data)+1,
                ctx.data_mr, IBV_SEND_SIGNALED,
                ctx.recv_ctrl.addr,   // 对端地址（客户端通过 SEND 告知）
                ctx.recv_ctrl.rkey);  // 对端 rkey
```

**READ** 方向相反（本项目未用，原理对称）：`rdma_post_read(id, ..., local_buf,
local_mr, flags, remote_addr, remote_rkey)`，从对端内存把数据拉到本地。

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="720" height="280" font-family="sans-serif" font-size="12">
  <rect width="720" height="280" fill="#fbfbfd"/>
  <!-- WRITE -->
  <text x="160" y="22" text-anchor="middle" font-weight="bold" fill="#1565c0">RDMA WRITE</text>
  <rect x="60" y="36" width="180" height="40" fill="#e3f2fd" stroke="#1565c0"/>
  <text x="150" y="61" text-anchor="middle" font-size="11">发起方 post_write</text>
  <rect x="480" y="36" width="180" height="40" fill="#eceff1" stroke="#90a4ae"/>
  <text x="570" y="55" text-anchor="middle" font-size="11">对端内存 (REMOTE_WRITE)</text>
  <text x="570" y="70" text-anchor="middle" font-size="10" fill="#999">CPU 无感</text>
  <line x1="240" y1="56" x2="480" y2="56" stroke="#1565c0" stroke-width="2" marker-end="url(#f)"/>
  <text x="360" y="48" text-anchor="middle" fill="#1565c0" font-size="11">数据 + remote_addr + rkey →</text>

  <!-- READ -->
  <text x="160" y="142" text-anchor="middle" font-weight="bold" fill="#2e7d32">RDMA READ</text>
  <rect x="60" y="156" width="180" height="40" fill="#e8f5e9" stroke="#2e7d32"/>
  <text x="150" y="181" text-anchor="middle" font-size="11">发起方 post_read</text>
  <rect x="480" y="156" width="180" height="40" fill="#eceff1" stroke="#90a4ae"/>
  <text x="570" y="175" text-anchor="middle" font-size="11">对端内存 (REMOTE_READ)</text>
  <text x="570" y="190" text-anchor="middle" font-size="10" fill="#999">CPU 无感</text>
  <line x1="480" y1="176" x2="240" y2="176" stroke="#2e7d32" stroke-width="2" marker-end="url(#g)"/>
  <text x="360" y="168" text-anchor="middle" fill="#2e7d32" font-size="11">← 拉回数据</text>

  <text x="360" y="245" text-anchor="middle" fill="#c62828">仅发起方收到 CQE；对端零参与（单边）</text>
  <defs>
    <marker id="f" markerWidth="9" markerHeight="9" refX="7" refY="3" orient="auto"><path d="M0,0 L7,3 L0,6 z" fill="#1565c0"/></marker>
    <marker id="g" markerWidth="9" markerHeight="9" refX="7" refY="3" orient="auto"><path d="M0,0 L7,3 L0,6 z" fill="#2e7d32"/></marker>
  </defs>
</svg>
```

---

## 7. 完成机制：post 与 poll

RDMA 是**异步**的，分两步：

1. **post**（投递）：`rdma_post_send / post_recv / post_write / post_read` 把 WR
   放入队列，**立即返回**，硬件后台执行。
2. **poll**（轮询完成）：从 CQ 取出 CQE 确认操作真正完成。本项目用便捷封装
   `rdma_get_send_comp` / `rdma_get_recv_comp`（底层即 `ibv_poll_cq`），见
   `wait_send_comp()` / `wait_recv_comp()`（`server.c:24-48`）。

`IBV_SEND_SIGNALED`（以及 `sq_sig_all=1`，`server.c:20`）保证每个 send 类操作都
产生 CQE，才能被 poll 到。务必检查 `wc.status == IBV_WC_SUCCESS`。

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="720" height="260" font-family="sans-serif" font-size="12">
  <rect width="720" height="260" fill="#fbfbfd"/>
  <rect x="40" y="40" width="160" height="40" fill="#e3f2fd" stroke="#1565c0"/>
  <text x="120" y="65" text-anchor="middle">1. post_*()</text>
  <text x="120" y="100" text-anchor="middle" font-size="10" fill="#2e7d32">立即返回(非阻塞)</text>

  <rect x="260" y="40" width="160" height="40" fill="#ede7f6" stroke="#5e35b1"/>
  <text x="340" y="65" text-anchor="middle">2. 网卡执行 WR</text>
  <text x="340" y="100" text-anchor="middle" font-size="10" fill="#555">DMA / 网络传输</text>

  <rect x="480" y="40" width="160" height="40" fill="#fff3e0" stroke="#ef6c00"/>
  <text x="560" y="65" text-anchor="middle">3. CQE 入 CQ</text>

  <line x1="200" y1="60" x2="260" y2="60" stroke="#555" marker-end="url(#h)"/>
  <line x1="420" y1="60" x2="480" y2="60" stroke="#555" marker-end="url(#h)"/>

  <rect x="260" y="150" width="200" height="44" fill="#e8f5e9" stroke="#2e7d32"/>
  <text x="360" y="170" text-anchor="middle">4. poll_cq / get_*_comp</text>
  <text x="360" y="187" text-anchor="middle" font-size="11">检查 wc.status</text>
  <line x1="560" y1="80" x2="430" y2="150" stroke="#ef6c00" marker-end="url(#h)"/>
  <text x="360" y="228" text-anchor="middle" fill="#c62828">SUCCESS 才算真正完成；否则报错退出</text>
  <defs><marker id="h" markerWidth="8" markerHeight="8" refX="6" refY="3" orient="auto"><path d="M0,0 L6,3 L0,6 z" fill="#555"/></marker></defs>
</svg>
```

---

## 8. 本项目端到端时序

把上述拼起来，就是 `server.c` 与 `client.c` 的完整协作：先用 **SEND/RECV** 交换
MR 元数据（控制面），再用 **RDMA WRITE** 推送数据（数据面），最后 SEND 一个 ACK。
这正是真实 RDMA 系统的经典范式。

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="720" height="420" font-family="sans-serif" font-size="11">
  <rect width="720" height="420" fill="#fbfbfd"/>
  <text x="170" y="22" text-anchor="middle" font-weight="bold">Client</text>
  <text x="550" y="22" text-anchor="middle" font-weight="bold">Server</text>
  <line x1="170" y1="32" x2="170" y2="400" stroke="#1565c0" stroke-width="2"/>
  <line x1="550" y1="32" x2="550" y2="400" stroke="#5e35b1" stroke-width="2"/>

  <text x="170" y="56" text-anchor="middle" fill="#2e7d32">post_recv(ack)</text>
  <text x="550" y="56" text-anchor="middle" fill="#2e7d32">post_recv(ctrl)</text>

  <line x1="170" y1="80" x2="550" y2="80" stroke="#1565c0" marker-end="url(#i)"/>
  <text x="360" y="73" text-anchor="middle">connect / accept 建链</text>

  <line x1="170" y1="120" x2="550" y2="120" stroke="#1565c0" marker-end="url(#i)"/>
  <text x="360" y="113" text-anchor="middle">① SEND: addr + rkey + size  (control_message)</text>
  <text x="360" y="138" text-anchor="middle" font-size="10" fill="#555">服务端 recv 完成，拿到客户端 MR 信息</text>

  <line x1="550" y1="170" x2="170" y2="170" stroke="#c62828" stroke-width="2" marker-end="url(#j)"/>
  <text x="360" y="163" text-anchor="middle" fill="#c62828">② RDMA WRITE: local_data → 客户端内存</text>
  <text x="360" y="188" text-anchor="middle" font-size="10" fill="#999">客户端 CPU 无感知（单边）</text>

  <line x1="550" y1="220" x2="170" y2="220" stroke="#5e35b1" marker-end="url(#j)"/>
  <text x="360" y="213" text-anchor="middle">③ SEND: ACK ("RDMA write completed")</text>

  <rect x="60" y="250" width="220" height="46" fill="#e8f5e9" stroke="#2e7d32"/>
  <text x="170" y="270" text-anchor="middle" font-size="10">客户端打印被覆盖的 buffer</text>
  <text x="170" y="286" text-anchor="middle" font-size="10">＝ 服务端写入的字符串</text>

  <line x1="170" y1="330" x2="550" y2="330" stroke="#999" stroke-dasharray="4" marker-end="url(#i)"/>
  <text x="360" y="323" text-anchor="middle" fill="#999">disconnect / dereg_mr / destroy_ep</text>
  <text x="360" y="375" text-anchor="middle" fill="#555">控制面(SEND/RECV) + 数据面(WRITE) 的经典组合</text>
  <defs>
    <marker id="i" markerWidth="9" markerHeight="9" refX="7" refY="3" orient="auto"><path d="M0,0 L7,3 L0,6 z" fill="#1565c0"/></marker>
    <marker id="j" markerWidth="9" markerHeight="9" refX="7" refY="3" orient="auto"><path d="M0,0 L7,3 L0,6 z" fill="#c62828"/></marker>
  </defs>
</svg>
```

---

## 9. 构建、运行与调试

```bash
make                       # 生成 bin/rdma_server 与 bin/rdma_client
./bin/rdma_server <RDMA网卡IP> 7471   # 终端1
./bin/rdma_client <RDMA网卡IP> 7471   # 终端2
```

构建/运行图：

```svg
<svg xmlns="http://www.w3.org/2000/svg" width="720" height="220" font-family="sans-serif" font-size="12">
  <rect width="720" height="220" fill="#fbfbfd"/>
  <rect x="30" y="80" width="150" height="50" fill="#e3f2fd" stroke="#1565c0"/>
  <text x="105" y="100" text-anchor="middle">src/*.c</text>
  <text x="105" y="118" text-anchor="middle" font-size="10">common.h</text>
  <line x1="180" y1="105" x2="250" y2="105" stroke="#555" marker-end="url(#k)"/>
  <text x="215" y="98" text-anchor="middle" font-size="10">make</text>
  <rect x="250" y="80" width="170" height="50" fill="#ede7f6" stroke="#5e35b1"/>
  <text x="335" y="100" text-anchor="middle">gcc + librdmacm</text>
  <text x="335" y="118" text-anchor="middle" font-size="10">libibverbs</text>
  <line x1="420" y1="105" x2="490" y2="105" stroke="#555" marker-end="url(#k)"/>
  <rect x="490" y="55" width="200" height="44" fill="#e8f5e9" stroke="#2e7d32"/>
  <text x="590" y="82" text-anchor="middle">bin/rdma_server</text>
  <rect x="490" y="112" width="200" height="44" fill="#e8f5e9" stroke="#2e7d32"/>
  <text x="590" y="139" text-anchor="middle">bin/rdma_client</text>
  <line x1="590" y1="156" x2="590" y2="185" stroke="#c62828" marker-end="url(#k)"/>
  <text x="590" y="205" text-anchor="middle" font-size="11" fill="#c62828">需 RDMA 网卡 IP（非 127.0.0.1）</text>
  <defs><marker id="k" markerWidth="8" markerHeight="8" refX="6" refY="3" orient="auto"><path d="M0,0 L6,3 L0,6 z" fill="#555"/></marker></defs>
</svg>
```

**常见问题**：
- `rdma_create_ep: No such device`：用了 `127.0.0.1`；请改用 RDMA 网卡 IP。
- 无物理 RDMA 网卡：可用 Soft-RoCE（`rdma_rxe` 内核模块）模拟。
- 依赖（Anolis/RHEL/CentOS）：`dnf install -y gcc make rdma-core-devel
  libibverbs-devel librdmacm-devel`。

---

## 10. 给 Claude 的工作约定

- **代码风格**：C11，4 空格缩进，沿用现有 `die_rdma` / `check_zero` 错误处理与
  `wait_send_comp` / `wait_recv_comp` 模式；新增 verbs 调用务必检查返回值与
  `wc.status`。
- **目录**：源码在 `src/`，产物在 `bin/`（已被 `.gitignore` 忽略）。
- **改动后**：`make` 必须能通过；涉及收发逻辑时同步更新 server 与 client 两端。
- **教学定位**：本仓库面向初学者，优先清晰可读，而非生产级健壮性（重试、心跳、
  连接重建、CQ 事件模式、内存池等属于扩展方向）。
- **文档**：行为变化时同步更新本文件与 `README.md`；新增章节请保持"**每节配一张
  SVG 图**"的约定。
- **配图（强制约束）**：本文件及教程的**每一节、每一个阶段（含 `TODO.md` 阶段一~
  阶段八的每个章节）都必须配至少一张 SVG 示意图**——内联 `<svg>`，用于刻画原理、
  数据路径、状态机或时序。**新增或重写任何阶段/小节时，缺图视为未完成**，不得只有
  文字。图需与正文术语一致，并随内容变化同步更新。
- **分支**：在指定的开发分支上提交，非必要不创建 PR。
