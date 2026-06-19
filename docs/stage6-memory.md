# 阶段六：高级内存管理

> 目标：理解 MR 注册代价、按需分页、动态授权与大页对性能的影响，写出内存高效的 RDMA 程序。

---

## 6.1 注册开销与缓存

`ibv_reg_mr` 并非零代价——它在内核态完成三件事：

1. **Pin 内存页**：`get_user_pages` 锁定物理页，防止 OS 换出或迁移。
2. **建立 MTT/MPT 条目**：在网卡地址翻译表中为每个物理页写入映射。
3. **IOMMU 映射**：在 IOMMU/SMMU 中建立 DMA 地址翻译（若开启）。

典型注册延迟 **1–10 µs/次**，在高频小对象场景（如 RPC 请求缓冲）会成为瓶颈。

![注册开销与注册缓存](img/s6-1-reg-cache.svg)

**注册缓存设计**：

```c
// 简化的 MR 缓存（key = addr+len+flags）
struct mr_cache_entry {
    void          *addr;
    size_t         len;
    int            access;
    struct ibv_mr *mr;
    int            refcnt;
    // LRU 链表节点...
};

struct ibv_mr *cached_reg_mr(struct ibv_pd *pd,
                              void *addr, size_t len, int access) {
    struct mr_cache_entry *e = cache_lookup(addr, len, access);
    if (e) { e->refcnt++; return e->mr; }          // 缓存命中 ~100ns
    struct ibv_mr *mr = ibv_reg_mr(pd, addr, len, access); // 缓存未命中 ~µs
    cache_insert(addr, len, access, mr);
    return mr;
}
```

**使用建议**：
- **MR 池**：预先注册固定大小的缓冲块，复用而非反复 reg/dereg。
- **注册缓存**：适合地址不固定但访问模式可预测的场景（如 jemalloc 集成）。
- LRU 驱逐时调用 `ibv_dereg_mr`；缓存容量受 `ulimit -l`（locked memory）限制。

---

## 6.2 ODP（On-Demand Paging）

ODP 允许注册**未 pin 的虚拟地址范围**，由 NIC + 内核协作在访问时按需 pin 页：

![ODP 按需分页](img/s6-2-odp.svg)

```c
// 注册时不 pin，仅建立虚拟地址到 MR 的映射
struct ibv_mr *mr = ibv_reg_mr(pd, addr, len,
    IBV_ACCESS_LOCAL_WRITE |
    IBV_ACCESS_REMOTE_WRITE |
    IBV_ACCESS_ON_DEMAND);       // ODP 标志

// 预取热点页（减少运行时缺页）
struct ibv_sge sge = { .addr = (uint64_t)hot_addr, .length = hot_len, .lkey = mr->lkey };
ibv_advise_mr(pd, IBV_ADVISE_MR_ADVICE_PREFETCH_WRITE, 0, &sge, 1);
```

**ODP 工作流程**：
1. 应用 post_send/post_write 引用 ODP MR 中未 pin 的地址。
2. NIC 检测到页表缺失，通过 `ucontext` 向内核发起 page fault 请求。
3. 内核 pin 该页，更新 NIC 的页表（`mlx5_ib_invalidate_range` 反向路径）。
4. NIC 重试原始 DMA 操作。

**适用场景**：
- 大稀疏地址空间（如 RDMA 上的分布式 KV 存储，value 按需访问）。
- 与 `malloc` 直接对接，无需预先注册（注册一次整个 heap 范围）。

**限制**：缺页延迟 10–100 µs；要求内核 ≥ 4.4 + NIC firmware 支持（Mellanox ConnectX-4 起）；不支持原子操作。

---

## 6.3 Memory Windows

Memory Windows（MW）允许在不重建 MR 的情况下，动态授予/撤销对 MR **子区域**的远端访问权：

![Memory Windows 子区域动态授权](img/s6-3-mw.svg)

**两种类型**：

| 类型 | 绑定方式 | 原子性 | 适用场景 |
|------|----------|--------|----------|
| Type 1 | `ibv_bind_mw`（独立操作） | 否 | 简单子区域授权 |
| Type 2 | 嵌入 RDMA Write/Read WR | 是（与数据操作原子） | 高安全要求场景 |

```c
// 分配 MW（Type 1）
struct ibv_mw *mw = ibv_alloc_mw(pd, IBV_MW_TYPE_1);

// 绑定到 MR 的子区域
struct ibv_mw_bind bind_info = {
    .bind_info = {
        .mr      = mr,
        .addr    = (uint64_t)sub_region_start,
        .length  = sub_region_len,
        .mw_access_flags = IBV_ACCESS_REMOTE_WRITE,
    },
};
ibv_bind_mw(qp, mw, &bind_info);
// mw->rkey 是新生成的受限 rkey，发给客户端

// 撤销：直接释放 MW，rkey 立即失效，MR 无需改动
ibv_dealloc_mw(mw);
```

**安全用途**：定期轮换 MW（生成新 rkey），旧 rkey 自动失效，防止重放攻击。
MR 本身的 rkey 永远不需要暴露给客户端。

---

## 6.4 HugePage / 大 MR / 连续内存

MTT 条目数 = MR 大小 / 页大小。4K 页的大 MR 产生海量 MTT，加剧 NIC TLB 压力：

![HugePage 对 MTT 与 TLB 的影响](img/s6-4-hugepage.svg)

```
1 GB MR，4K 页：262,144 条 MTT
1 GB MR，2MB 页：512 条 MTT      ← 减少 512 倍
```

**分配 HugePage**：

```c
// 方式一：mmap + MAP_HUGETLB（2MB）
void *buf = mmap(NULL, size,
                 PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
                 -1, 0);
mlock(buf, size);  // 防止 THP 迁移

// 方式二：/dev/hugepages 预分配（需 root 或 hugepages 系统配置）
// echo 512 > /proc/sys/vm/nr_hugepages
int fd = open("/dev/hugepages/rdma_buf", O_CREAT | O_RDWR, 0600);
void *buf = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);

// NUMA 亲和分配（减少跨 NUMA 访问）
#include <numa.h>
void *buf = numa_alloc_onnode(size, numa_node_of_cpu(sched_getcpu()));
```

**透明大页（THP）注意事项**：
- THP 由内核自动合并/拆分，`khugepaged` 可能在 MR 注册后迁移页面。
- RDMA 与 THP 共用时，**务必 `mlock`** 防止页迁移，或使用显式 `MAP_HUGETLB`
  完全绕过 THP 机制。
- 检查 THP 状态：`cat /sys/kernel/mm/transparent_hugepage/enabled`。

**`ibv_reg_mr` 自动识别大页**：当传入地址对齐到 2MB 且操作系统已映射为大页时，
libibverbs/provider 会自动用大页 MTT 条目，无需额外 API 调用。

---

## 小结：原理 → API → 代码 → 性能 → 陷阱

| 维度 | 要点 |
|------|------|
| **原理** | reg_mr 代价来自 pin+MTT+IOMMU；ODP 延迟 pin；MW 动态授权子区域；大页减少 MTT 条目 |
| **API** | `ibv_reg_mr(IBV_ACCESS_ON_DEMAND)`；`ibv_advise_mr`；`ibv_alloc_mw`/`ibv_bind_mw`/`ibv_dealloc_mw`；`mmap(MAP_HUGETLB)` |
| **代码** | MR 池/缓存复用；ODP 用 ibv_advise_mr 预取；MW 子区域精确授权；NUMA-local alloc |
| **性能** | 大页将 MTT 条目减少 512×；MR 池消除 reg/dereg 开销；ODP 消除预注册但引入缺页延迟 |
| **陷阱** | THP 页迁移破坏 pin（需 mlock）；MW Type 2 需 NIC 支持；ODP 不支持原子操作；缓存 LRU 需控制 locked memory 上限 |
