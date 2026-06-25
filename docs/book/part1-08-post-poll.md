# 第 8 章 · 完成机制：post 与 poll

前两章我们发了 SEND、做了 WRITE、玩了 READ。但每次代码里都悄悄跟着一句
`wait_send_comp` 或 `wait_recv_comp`，我们一直没正面解释它。这一章就来补上这块
拼图：**RDMA 操作是异步的，你得分两步走——先 post，再 poll。** 理解了它，你才算
真正理解了 RDMA 的「节奏」。

## 本章你将遇到的术语（预览）

- **异步（asynchronous）**：`post` 立即返回，硬件在后台干活，不阻塞你。
- **post**：把一个 WR（工作请求）放进队列，交给网卡。
- **poll**：从 CQ（完成队列）取出 CQE，确认某个操作真的完成了。
- **CQE / ibv_wc**：一张「操作完成回执」，里头有状态、操作码等信息。
- **IBV_SEND_SIGNALED / sq_sig_all**：决定一次 send 类操作要不要产生 CQE。
- **wc.status**：完成的结果码，`IBV_WC_SUCCESS` 才算成功。

## 场景 / 问题引入

来看一行你已经见过很多次的代码：

```c
rdma_post_send(id, NULL, buf, len, mr, IBV_SEND_SIGNALED);
```

这行调用返回了——**但数据真的发出去了吗？** 答案是：不一定。它只是把「请发送」
这个请求挂进了发送队列，网卡可能还没开始搬、正在搬、或刚搬完。如果你这时候就
复用 `buf`、或者以为对端已经收到了，就会踩坑。

那「到底什么时候算完成」要怎么知道？这就是 poll 要回答的问题。

## 直觉与类比

把 post/poll 想象成**点外卖**：

- 你下单（`post`）——App 立刻提示「下单成功」，但你的饭还没做好，更没送到。下单
  动作本身瞬间完成，不阻塞你继续刷手机。
- 厨房后台在做饭、骑手在送（**网卡硬件异步执行**）。
- 你时不时刷新订单状态，直到看到「已送达」（`poll` 到一个 CQE）。
- 「已送达」的回执上还写着结果：成功送到，还是「商家已取消」（`wc.status`）。

关键认知：**下单 ≠ 吃到饭**。`post` 返回 ≠ 操作完成。必须 poll 到那张回执，才能
确信。

![post 与 poll 完成机制](../img/07-post-poll.svg)

## 概念一：异步的两步——post 与 poll

RDMA 为了极致性能，把一次操作拆成两个解耦的阶段：

1. **post（投递）**：`rdma_post_send` / `post_recv` / `post_write` / `post_read`
   把一个 WR 放入队列，**立即返回**，由网卡硬件在后台执行。这一步几乎不耗 CPU，
   也不陷入内核——这正是 RDMA 内核旁路、低开销的体现。
2. **poll（轮询完成）**：从 CQ 里取出一个 CQE（完成事件），确认对应操作**真正
   完成**了，并检查它的结果。

之所以要拆开，是因为这样你可以**连续 post 很多个操作**（让网卡流水线满载），
之后再统一 poll 它们的完成——这是高吞吐的基础。乒乓示例里我们「发一个等一个」，
那是最朴素的形态；性能优化阶段会让 post 和 poll 充分解耦。

## 概念二：CQE 与本项目的便捷封装

操作完成后，网卡向 CQ 投递一个 **CQE**，在 verbs API 里就是一个 `struct ibv_wc`
（work completion）。本项目用 librdmacm 的便捷封装把「poll」这件事包了起来——
底层其实就是 `ibv_poll_cq`：

```c
static inline void wait_send_comp(struct rdma_cm_id *id, const char *what)
{
    struct ibv_wc wc;
    if (rdma_get_send_comp(id, &wc) <= 0) {   // 底层 ibv_poll_cq，阻塞取一个完成
        die_rdma(what);
    }
    if (wc.status != IBV_WC_SUCCESS) {        // ★ 必须检查状态！
        fprintf(stderr, "%s: send wc status=%s\n", what, ibv_wc_status_str(wc.status));
        exit(EXIT_FAILURE);
    }
}
```

逐行讲「为什么」：

- `rdma_get_send_comp(id, &wc)`：从**发送**完成队列取一个 CQE。返回值 `<= 0`
  表示出错或没取到。`rdma_get_recv_comp` 则对应**接收**完成。
- `&wc`：取回的那张回执。里面有 `status`、`opcode`、`byte_len`、`imm_data`（第
  9 章会用到）等字段。
- `wc.status != IBV_WC_SUCCESS`：**这步绝不能省**。下一节专门讲。

接收方向同理用 `wait_recv_comp`（底层 `rdma_get_recv_comp`）。对照第 7 章的结论
再强化一次：单边 WRITE/READ 只在**发起方**产生 send CQE，所以发起方用
`wait_send_comp`；被访问方根本没有 CQE 可 poll。

## 概念三：signaled——决定一次操作要不要产生 CQE

不是每个 post 都会自动产生 CQE。是否产生，由两个开关控制：

- **每条 WR 的 `IBV_SEND_SIGNALED` 标志**：给这一条操作单独开启「完成时产生
  CQE」。
- **QP 的 `sq_sig_all` 属性**：建 QP 时设为 1，则该 QP 上**每个** send 类操作都
  自动产生 CQE，不必逐条加标志。

本项目的脚手架两个都用了——教学默认「事事有回执」，最直观：

```c
qp_attr->sq_sig_all = 1;       /* 教学默认：每个 send 都产生 CQE */
qp_attr->qp_type = IBV_QPT_RC; /* 可靠连接 */
```

而每次 post 又显式带上 `IBV_SEND_SIGNALED`。为什么强调这个？因为**只有产生了
CQE 的操作才能被 poll 到**。如果你既没设 `sq_sig_all`、又没加
`IBV_SEND_SIGNALED`，那个操作完成时**不产生 CQE**——你在那儿 poll 会一直等不到，
程序卡死。

反过来，「事事 signaled」在高吞吐场景是浪费：CQE 越多，poll 的开销越大。所以
进阶的**选择性 signaling**（每 N 个操作才 signal 一个）是重要的性能优化——这是
第二部分的内容，这里先埋个伏笔。

## 概念四：永远检查 wc.status

poll 到一个 CQE，**不等于操作成功了**——它只表示「有结果了」。结果是好是坏，看
`wc.status`：

- `IBV_WC_SUCCESS`：成功。
- 其它值（如 `IBV_WC_RNR_RETRY_EXC_ERR`、`IBV_WC_LOC_LEN_ERR`、
  `IBV_WC_REM_ACCESS_ERR` 等）：各种失败，可用 `ibv_wc_status_str(wc.status)`
  打印成人类可读字符串。

更要命的是：**RC（可靠连接）QP 一旦某个操作出错，整个 QP 会进入 error 态**，后续
所有操作都会失败。所以错误必须当场发现、当场处理。本项目的约定就是检查到非
SUCCESS 立即报错退出——简单粗暴，但教学上一目了然，也符合「verbs 调用务必检查
返回值与 wc.status」的工程纪律。

## 常见误区

- **「post 返回了就等于发出去/完成了」**：错。post 只是投递，必须 poll 才知道
  真正完成。在 poll 到完成前**不要复用或释放 send 的源 buffer**。
- **「poll 到 CQE 就是成功」**：错。必须再看 `wc.status == IBV_WC_SUCCESS`。
- **「忘了 signal 却在那儿 poll」**：操作不产生 CQE，poll 永远等不到，程序假死。
- **「一个操作产生多个 CQE」**：一个 signaled 操作只产生一个 CQE；别重复 poll。
- **「send 的 CQE 代表对端应用已处理」**：只代表本端网卡已交付（RC 下对端网卡已
  确认）。对端应用是否处理，需应用层 ACK。

## 小结

- RDMA 是异步的两步：**post**（投递 WR，立即返回，硬件后台执行）+ **poll**（从
  CQ 取 CQE 确认完成）。
- 本项目用 `rdma_get_send_comp` / `rdma_get_recv_comp`（底层 `ibv_poll_cq`）来
  poll。
- `IBV_SEND_SIGNALED` 与 `sq_sig_all` 决定操作是否产生 CQE——**没 CQE 就 poll
  不到**。
- poll 到之后**必须检查 `wc.status == IBV_WC_SUCCESS`**；RC QP 出错会整体进入
  error 态。

至此，入门主线（核心对象、内存注册、建链、三大语义、完成机制）已经全部贯通。
下一章是入门通往进阶的**桥梁**：我们会看两个更精巧的操作——带立即数的写
（省一次 ACK 往返）和原子操作（分布式锁的地基），并由此引出第二部分的硬件模型
与性能工程。

## 术语速查

| 术语 | 含义 |
|------|------|
| post | 把 WR 投递进队列，立即返回，硬件后台执行 |
| poll | 从 CQ 取出 CQE，确认操作完成 |
| CQE / ibv_wc | 一次操作的完成回执，含 status、opcode 等 |
| ibv_poll_cq | poll CQ 的底层 verbs 调用 |
| rdma_get_send_comp / recv_comp | librdmacm 对 poll 的便捷封装 |
| IBV_SEND_SIGNALED | 让单条操作完成时产生 CQE |
| sq_sig_all | QP 属性，令每个 send 都自动产生 CQE |
| wc.status | 完成结果码，IBV_WC_SUCCESS 才算成功 |
