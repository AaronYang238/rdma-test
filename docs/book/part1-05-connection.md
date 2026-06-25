# 第 5 章 · 建立连接：rdma_cm

> 上一章我们把内存登记好、拿到了钥匙。但两台机器还各自为政，没有"接上头"。
> 这一章讲连接是怎么建起来的：**rdma_cm 如何把建连和创建 QP 封装成熟悉的
> 类 socket 流程**，以及一个让无数新手栽跟头的关键时序——**为什么接收方必须在
> 连接真正可用之前，就先把接收缓冲准备好。**

## 本章你将遇到的术语（预览）

- **rdma_cm（librdmacm）**：RDMA 的连接管理库，把建连 + 建 QP 包装成类 socket API。
- **rdma_cm_id**：连接的句柄，类似 socket fd，内部封装了 PD/QP/CQ。
- **rdma_create_ep**：创建带 QP 的端点，一步到位。
- **rdma_listen / rdma_get_request / rdma_accept**：服务端监听、取连接请求、接受。
- **rdma_connect**：客户端发起连接。
- **预投递接收（post_recv）**：在连接可用前就往 RQ 里放好接收任务单。

## 场景 / 问题引入

会用 TCP socket 的你，对建连流程很熟：服务端 `socket → bind → listen → accept`，
客户端 `socket → connect`。

RDMA 建连其实更麻烦：除了"接上头"，还要在两端创建并协商 QP、交换底层参数、把 QP
推进到能收发的状态……如果手工用底层 verbs 做，代码会非常长。

好在我们不必受这个苦。**rdma_cm 这套库，把"建立连接 + 创建并配置好 QP"打包成了
一套和 socket 长得几乎一样的 API。** 但它有一个和 socket 不同、且极易被忽略的
关键点，我们这一章重点讲透。

## 直觉与类比

rdma_cm 就像一个**贴心的搬家中介**：你只管说"我要和那栋楼接上线"，中介帮你把
拉专线、装好两端的收发文件筐（QP）、调试线路这些杂活全办了，最后给你一个"线路
句柄"（`rdma_cm_id`），你拿着它就能收发。

但中介有一条铁律提醒你：**通话接通的那一刻，对方可能立刻就开始往你这边发东西。
所以你必须在接通之前，就先把"收件筐"摆好**——否则对方第一句话发过来，你这边
没地方接，连接就出问题了。这条铁律就是下面要讲的"预投递接收"。

![rdma_cm 连接建立时序](../img/04-connection.svg)

## 概念一：rdma_cm_id 与 rdma_create_ep

`rdma_cm_id` 是连接的句柄，**类似 socket fd**。最妙的是它内部已经封装了第 3 章讲的
**PD + QP + CQ**——这就是为什么示例代码里看不到逐个创建这些对象的样板。

创建端点用 `rdma_create_ep`，它一步到位地建好带 QP 的端点：

```c
fill_qp_attr(&qp_attr);                                  /* 配置 QP 能力 */
check_zero(rdma_create_ep(&id, res, NULL, &qp_attr), "rdma_create_ep");
ctx.id = id;                                             /* 之后 ctx->id->pd 即本端 PD */
```

`res` 来自 `rdma_getaddrinfo`，它负责把"IP + 端口"解析成 RDMA 能用的地址信息，
类似 socket 编程里的 `getaddrinfo`。

## 概念二：服务端流程

服务端的流程和 TCP 几乎一一对应：

```c
rdma_getaddrinfo(...)                 /* 解析本地绑定地址（带 RAI_PASSIVE 标志） */
rdma_create_ep(&listen_id, ...)       /* 创建监听端点 */
rdma_listen(listen_id, 1)             /* 开始监听，类似 listen() */
rdma_get_request(listen_id, &conn_id) /* 取出一个连接请求，得到新的 conn_id */
/* ……此处必须先 post_recv（见概念四）…… */
rdma_accept(conn_id, NULL)            /* 接受连接，类似 accept() */
```

注意 `rdma_get_request` 返回一个**新的** `conn_id`，代表这条具体连接（监听用
`listen_id`，通信用 `conn_id`），这和 TCP 里 `accept()` 返回新 fd 是一个道理。

## 概念三：客户端流程

客户端更简单：

```c
rdma_getaddrinfo(...)        /* 解析服务端地址 */
rdma_create_ep(&id, ...)     /* 创建端点 */
/* ……此处必须先 post_recv（见概念四）…… */
rdma_connect(id, NULL)       /* 发起连接，类似 connect() */
```

到这里，连接建立的"形"你已经掌握了，和 socket 几乎无差别。真正的"神"在下一节。

## 概念四（关键）：为什么必须在连接可用前预投递接收

这是本章、也是很多 RDMA 新手最容易困惑、最容易出错的地方，请慢慢读。

回忆第 3 章：QP 的接收队列 RQ 里，必须**事先放好"接收任务单"（post_recv）**，
对端 SEND 过来的数据才有地方安放。RDMA 的接收**不像** TCP——TCP 即使你还没
`read()`，内核也会先把数据缓存在 socket 缓冲区里。**RDMA 没有这层内核缓冲：
对端 SEND 到达时，如果你的 RQ 里没有预备好的接收缓冲，这条消息就无处安放，会出错。**

所以铁律是：**接收方必须在连接"真正可用"之前，就先 `post_recv`。**

- "真正可用"对服务端来说是 `rdma_accept` 之后，对客户端来说是 `rdma_connect`
  完成之后——一旦可用，对端随时可能 SEND 过来。
- 因此 post_recv 必须**早于**这个时间点。

看示例 01 怎么做的。**服务端**在 `rdma_accept` **之前** 就预投递了接收：

```c
/* server.c —— 预投递接收，必须在 accept 前 */
rdma_post_recv(conn_id, NULL, &ctx.recv_ctrl, sizeof(ctx.recv_ctrl), ctx.recv_ctrl_mr);
rdma_accept(conn_id, NULL);
```

**客户端**同样，在 `rdma_connect` **之前** 就预投递了接收 ACK 的缓冲：

```c
/* client.c —— 预投递接收 ACK，必须在 connect 前 */
rdma_post_recv(id, NULL, &ctx.recv_ctrl, sizeof(ctx.recv_ctrl), ctx.recv_ctrl_mr);
rdma_connect(id, NULL);
```

记住这个顺序：**先摆好收件筐（post_recv），再接通电话（accept / connect）。**
顺序反了，对端的第一条消息就可能扑空。

> 🛠 动手跑：[examples/01-write-demo/](../../examples/01-write-demo/)

## 常见误区

- **「RDMA 和 TCP 一样，数据没收会被内核缓存」**。错。RDMA 没有内核 socket 缓冲，
  接收前必须先 `post_recv` 备好缓冲，否则消息无处安放。
- **「post_recv 放在 accept / connect 之后也行」**。危险。连接一旦可用，对端可能
  立刻 SEND，此时 RQ 还空着就会出错。务必**先 post_recv 再 accept/connect**。
- **「监听用的 id 和通信用的 id 是同一个」**。服务端 `rdma_get_request` 会返回
  **新的** `conn_id`，监听 `listen_id` 和连接 `conn_id` 要分清。
- **「单边 WRITE/READ 也要对端 post_recv」**。不需要。post_recv 只为**双边 SEND**
  准备接收位置；单边操作对端 CPU 不参与，自然不需要它备接收缓冲。

## 小结

- **rdma_cm** 把建连 + 创建/配置 QP 封装成类 socket 流程，`rdma_cm_id` 类似 fd，
  内部封装了 PD/QP/CQ。
- 服务端：`getaddrinfo → create_ep → listen → get_request → accept`；
  客户端：`getaddrinfo → create_ep → connect`。
- **铁律**：接收方必须在连接真正可用前 `post_recv` 预投递接收缓冲。RDMA 没有内核
  缓冲，对端 SEND 到达时 RQ 必须已有接收位置。本书示例正是 accept/connect 之前
  就 post_recv。

至此，第一部分入门篇的"骨架"已经搭齐：你理解了 RDMA 为何存在、跑通了第一个示例、
认识了核心对象、弄清了内存注册、掌握了连接建立。接下来，我们将真正进入数据收发的
细节——**双边的 SEND/RECV、单边的 WRITE/READ，以及 post 与 poll 的完成机制**，
把示例 01 的每一步都吃透。

## 术语速查

| 术语 | 含义 |
|------|------|
| rdma_cm（librdmacm） | RDMA 连接管理库，类 socket 的建连 + 建 QP 封装 |
| rdma_cm_id | 连接句柄（类似 socket fd），内部封装 PD/QP/CQ |
| rdma_getaddrinfo | 解析地址 + 端口为 RDMA 地址信息 |
| rdma_create_ep | 创建带 QP 的端点 |
| rdma_listen / rdma_get_request / rdma_accept | 服务端监听 / 取连接请求 / 接受连接 |
| rdma_connect | 客户端发起连接 |
| 预投递接收（post_recv） | 在连接可用前往 RQ 放好接收任务单，否则对端 SEND 无处安放 |
