# 示例 05 · ATOMIC（FETCH_ADD / CMP_SWAP）

> 📖 对应教材：[第 9 章 · 立即数与原子操作](../../docs/book/part1-09-imm-atomic.md)

RDMA 原子操作让网卡直接对**对端内存中的一个 64 位字**做不可分割的"读-改-写"，
对端 CPU 全程不参与。是分布式锁、无锁计数器、序列号分配的基础。

本示例客户端依次：

1. `FETCH_AND_ADD(+5)`：`*remote += 5`，返回旧值（100 → 105）。
2. `COMPARE_AND_SWAP(expect=105, swap=999)`：若 `*remote == 105` 则置 999，返回旧值。

两者都把**操作前的旧值**写回客户端本地 8 字节 buffer，并在发送队列产生 CQE。

![原子操作时序](../../docs/img/ex05-atomic.svg)

## 要点与陷阱

- 目标地址必须 **8 字节对齐**（本例用 `__attribute__((aligned(8)))`）。
- 目标 MR 必须开放 **`IBV_ACCESS_REMOTE_ATOMIC`**。
- 原子操作只能用 `ibv_post_send` 手工构造，字段在 `wr.wr.atomic`：
  - `compare_add`：FETCH_ADD 的加数 / CMP_SWAP 的比较值；
  - `swap`：仅 CMP_SWAP 使用。
- 原子性保证的**粒度/范围**取决于设备能力（`atomic_cap`，IB 级 vs 全局），跨主机
  与本地 CPU 原子指令混用时尤需注意。

## 构建与运行

```bash
make
./bin/server <RDMA网卡IP> 7471   # 终端1
./bin/client <RDMA网卡IP> 7471   # 终端2
```

预期：客户端打印两次旧值；服务端打印 `final counter=999` 且强调自身 CPU 未参与。

## 关联章节

详见教材[第 9 章 · 立即数与原子操作](../../docs/book/part1-09-imm-atomic.md)。
