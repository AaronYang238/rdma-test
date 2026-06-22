# 阶段六：高级内存管理

> 本章深入讲解四类高级内存管理技术：**注册缓存**（消除重复 reg/dereg 开销）、
> **ODP（On-Demand Paging）**（按需分页，无需预先 pin）、**Memory Windows**
> （大 MR 的细粒度子区域授权）以及**大页与连续内存**（减少 MTT 条目、
> 提升 NIC TLB 命中率）。掌握这四项，可在保持正确性的前提下大幅降低
> 内存子系统开销，是从"能跑"走向"高性能"的必经之路。

---

## 6.1 注册开销与缓存

![注册缓存](img/s6-1-reg-cache.svg)

### ibv_reg_mr 的内部代价

`ibv_reg_mr` 并非简单的"记录一个指针"，它要完成以下工作：

1. **mlock / pin 页面**：通过 `get_user_pages_fast` 将缓冲区涉及的所有物理页
   锁定（不可换出、不可压缩），避免 NIC DMA 时页面失效。
2. **构建 MTT（Memory Translation Table）条目**：网卡的地址翻译硬件需要
   `虚拟地址 → 物理地址` 的映射表，`ibv_reg_mr` 逐页遍历，填写网卡内部的
   MTT/MPT（Memory Protection Table）。对于 1 GB、4K 页的 MR，需写入 262,144 项。
3. **IOMMU DMA 映射**：若系统启用 IOMMU，还需在 IOVA 空间建立映射，确保
   DMA 地址的安全性与隔离性。

典型耗时：小 MR（< 1 MB）约 **1–5 µs**；大 MR（1 GB）可达 **数十毫秒**，
因为构建 MTT 的时间与页数成线性关系。`ibv_dereg_mr` 同样昂贵：需刷新网卡
TLB/缓存、拆除 IOMMU 映射、解除页面 pin。

### 注册缓存设计

核心思路：**把 `ibv_mr*` 缓存起来，相同地址/长度/权限的 MR 不重复注册**。

```c
/* 注册缓存核心数据结构 */
typedef struct {
    void          *addr;
    size_t         len;
    int            access_flags;
} reg_cache_key_t;

typedef struct reg_cache_entry {
    reg_cache_key_t  key;
    struct ibv_mr   *mr;
    int              refcount;
    /* LRU 链表指针 */
    struct reg_cache_entry *lru_prev, *lru_next;
    /* 哈希桶链表 */
    struct reg_cache_entry *hash_next;
} reg_cache_entry_t;

#define CACHE_BUCKETS 256
typedef struct {
    reg_cache_entry_t *buckets[CACHE_BUCKETS];
    reg_cache_entry_t *lru_head, *lru_tail; /* 最近使用 → 最久未用 */
    int                count;
    int                max_entries;
    struct ibv_pd     *pd;
} reg_cache_t;

/* 查找或注册 MR */
struct ibv_mr *reg_cache_get(reg_cache_t *cache,
                             void *addr, size_t len, int flags)
{
    uint32_t h = hash(addr, len, flags) % CACHE_BUCKETS;
    reg_cache_entry_t *e = cache->buckets[h];
    while (e) {
        if (e->key.addr == addr && e->key.len == len &&
            e->key.access_flags == flags) {
            e->refcount++;
            lru_move_to_front(cache, e); /* 更新 LRU 顺序 */
            return e->mr;               /* 命中：~100 ns */
        }
        e = e->hash_next;
    }
    /* 未命中：真正注册 */
    if (cache->count >= cache->max_entries)
        reg_cache_evict_lru(cache);     /* LRU 淘汰最久未用的 MR */
    struct ibv_mr *mr = ibv_reg_mr(cache->pd, addr, len, flags);
    if (!mr) return NULL;
    reg_cache_insert(cache, addr, len, flags, mr); /* 存入缓存 */
    return mr;                          /* 未命中：1–10 µs */
}

/* 释放时只减引用计数 */
void reg_cache_put(reg_cache_t *cache, struct ibv_mr *mr)
{
    reg_cache_entry_t *e = reg_cache_find_by_mr(cache, mr);
    if (--e->refcount == 0 && cache->count > cache->max_entries / 2)
        reg_cache_remove_and_dereg(cache, e); /* 真正 dereg */
}
```

### 适用场景与陷阱

- **短生命周期分配**：请求处理完毕后立刻 free 再 malloc 的场景，地址可能复用，
  缓存 key 碰撞需格外小心（同地址不同长度，或权限标志改变）。
- **内存池配合使用**：固定大小的内存池天然适合注册缓存，地址稳定、权限不变。
- **主要陷阱**：`addr` 相同但 `len` 或 `access_flags` 不同时必须视为不同 key；
  缓存满时 LRU 淘汰会调用 `ibv_dereg_mr`，需确保被淘汰 MR 的 refcount 为 0。

---

## 6.2 ODP（按需分页）

![按需分页](img/s6-2-odp.svg)

### 原理：免预先 pin，NIC 触发缺页

ODP（On-Demand Paging）通过在 `ibv_reg_mr` 时传入
`IBV_ACCESS_ON_DEMAND` 标志，告知内核驱动**注册时不 pin 任何页面**。
页面何时被 NIC 真正访问，才在那时 pin 并映射。

**页面故障处理流程**：

1. NIC 发起 DMA 访问，目标物理地址在网卡页表中无映射。
2. NIC 内部触发"页面故障"信号，通过 PCIe 中断通知内核 RDMA 驱动
   （Mellanox 驱动为 `mlx5_ib_pfault`）。
3. 内核 `ibv_umem_odp` 子系统接管：通过 MMU notifier 找到对应 VMA，
   调用 `get_user_pages` 固定该物理页，更新网卡页表（类似 `mmu_notifier_invalidate_range`
   的逆操作）。
4. 内核通知 NIC 重试，NIC 重新发起 DMA，此时映射已存在，操作正常完成。

整个流程首次访问延迟约 **10–100 µs/页**，因此 ODP 适合**稀疏访问**场景，
不适合延迟敏感的热路径。

### 代码示例

```c
/* ODP 注册：页面不会被立即 pin */
struct ibv_mr *mr = ibv_reg_mr(pd, buf, buf_len,
    IBV_ACCESS_ON_DEMAND  |
    IBV_ACCESS_LOCAL_WRITE |
    IBV_ACCESS_REMOTE_WRITE);
if (!mr) { perror("ibv_reg_mr ODP"); exit(1); }

/* 预取：提前告知驱动将要访问的范围，触发批量 pin，减少后续缺页 */
struct ibv_sge sg = {
    .addr   = (uint64_t)buf,
    .length = prefetch_len,
    .lkey   = mr->lkey,
};
/* ibv_advise_mr 相当于对 ODP MR 的 madvise(WILLNEED) */
ibv_advise_mr(pd, IBV_ADVISE_MR_ADVICE_PREFETCH_WRITE,
              IB_UVERBS_ADVISE_MR_FLAG_FLUSH, &sg, 1);
```

### 优缺点对比

| 维度 | 传统 MR（全量 pin） | ODP MR（按需分页） |
|------|--------------------|--------------------|
| 注册耗时 | 1–10 µs（小）到数十 ms（大） | 极低（无 pin） |
| 首次访问延迟 | 正常 DMA 延迟 | +10–100 µs/页（缺页中断） |
| 内存用量 | 全部页面预先锁定 | 仅访问页面被锁定 |
| 稀疏大缓冲区 | 浪费（无论访问与否全部 pin） | 理想（只 pin 实际访问页） |
| 硬件要求 | 所有 RNIC | 需 ConnectX-4 及以上，且驱动支持 |

---

## 6.3 Memory Windows（内存窗口）

![内存窗口](img/s6-3-mw.svg)

### 问题背景

在多客户端场景中，服务端有一块大 MR，需要将不同子区域分别授权给不同客户端。
直接把整个 MR 的 `rkey` 给所有客户端，存在两个问题：

1. **权限过宽**：任一客户端都能访问整块 MR。
2. **无法撤销**：想收回某客户端的访问权，必须销毁并重建 MR（代价极大）。

Memory Window 正是解决这一问题的机制：在大 MR 上创建轻量级子区域窗口，
各窗口有独立的 `rkey`，可以按需绑定和撤销。

### Type 1 vs Type 2

**Type 1 MW**（`IBV_MW_TYPE_1`）：通过 `ibv_bind_mw` 在指定 QP 上显式绑定。

```c
/* 1. 分配 MW */
struct ibv_mw *mw = ibv_alloc_mw(pd, IBV_MW_TYPE_1);
if (!mw) { perror("ibv_alloc_mw"); exit(1); }

/* 2. 绑定到子区域 */
struct ibv_mw_bind bind_info = {
    .bind_info = {
        .mr           = mr,                    /* 底层 MR */
        .addr         = (uint64_t)sub_region_addr,
        .length       = sub_region_len,
        .mw_access_flags = IBV_ACCESS_REMOTE_WRITE,
    },
    .wr_id    = 42,
    .send_flags = IBV_SEND_SIGNALED,
};
int ret = ibv_bind_mw(qp, mw, &bind_info);
/* 绑定完成后 mw->rkey 更新为新 rkey，可传给客户端 */

/* 3. 使用完毕后撤销（无需销毁 MR） */
struct ibv_send_wr inv_wr = {
    .opcode     = IBV_WR_LOCAL_INV,
    .wr_id      = 43,
    .ex.invalidate_rkey = mw->rkey,
    .send_flags = IBV_SEND_SIGNALED,
};
struct ibv_send_wr *bad_wr;
ibv_post_send(qp, &inv_wr, &bad_wr);
```

**Type 2 MW**（`IBV_MW_TYPE_2`）：绑定作为 RDMA WRITE WR 的一部分原子执行，
避免额外往返。绑定和写入在网卡侧原子完成，适合对延迟极敏感的场景。

### rkey 轮换（安全技术）

每次授权后，撤销旧 MW 并重新 `ibv_bind_mw`，客户端得到新 rkey。
旧 rkey 失效，防止**重放攻击**（rkey replay attack）：
即使攻击者截获了之前的 rkey，也无法再次使用。

---

## 6.4 大页、大 MR 与连续内存

![大页与MTT](img/s6-4-hugepage.svg)

### MTT 条目数学

网卡地址翻译单位是物理页。对于 1 GB MR：

| 页大小 | 页数 | MTT 条目数 | NIC TLB 覆盖（512 项）|
|--------|------|------------|----------------------|
| 4 KB   | 262,144 | 262,144 | 512 × 4 KB = 2 MB    |
| 2 MB   | 512     | 512     | 512 × 2 MB = 1 GB    |
| 1 GB   | 1       | 1       | 全部                 |

NIC 内部 TLB（典型 512 项）在 4K 页模式下只能覆盖 2 MB；
超出覆盖范围的 DMA 访问会触发 TLB miss，导致网卡查询 MTT，
增加额外延迟并占用 PCIe 带宽。

### 分配方式

**显式大页分配（推荐）**：

```c
#include <sys/mman.h>
#include <numaif.h>
#include <numa.h>

/* 分配 2MB 大页，MAP_HUGETLB 要求系统预先配置大页池 */
size_t buf_size = 1UL << 30; /* 1 GB */
void *buf = mmap(NULL, buf_size,
                 PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
                 -1, 0);
if (buf == MAP_FAILED) {
    perror("mmap hugepage"); exit(1);
}

/* mlock 防止 2MB 大页被拆分（compaction） */
if (mlock(buf, buf_size) != 0) {
    perror("mlock"); /* 非致命，但建议处理 */
}

/* NUMA 亲和：将内存绑定到 NIC 所在 socket，降低跨 NUMA 访问延迟 */
/* 查询 NIC NUMA node: cat /sys/class/infiniband/<dev>/device/numa_node */
int nic_numa_node = 0; /* 示例：NIC 在 node 0 */
unsigned long nodemask = 1UL << nic_numa_node;
if (mbind(buf, buf_size, MPOL_BIND, &nodemask,
          sizeof(nodemask) * 8, MPOL_MF_MOVE) != 0) {
    perror("mbind");
}

/* 注册 MR */
struct ibv_mr *mr = ibv_reg_mr(pd, buf, buf_size,
    IBV_ACCESS_LOCAL_WRITE |
    IBV_ACCESS_REMOTE_WRITE |
    IBV_ACCESS_REMOTE_READ);
```

**配置系统大页池**（运行前需 root 操作）：

```bash
# 配置 2MB 大页（持久化写入 /etc/sysctl.conf 则永久生效）
echo 512 > /proc/sys/vm/nr_hugepages

# 验证
cat /proc/meminfo | grep Huge
# HugePages_Total:     512
# HugePages_Free:      512
# Hugepagesize:       2048 kB
```

### THP 警告：透明大页与 RDMA 的注意事项

**澄清**：对于**普通注册的 MR**（非 ODP），`ibv_reg_mr` 内部通过
`get_user_pages` 把页 **pin 住**，内核的 THP 合并/拆分、compaction、迁移
都**不会**动这些已 pin 的页——这种情况下 DMA 是安全的。

THP 的真正风险出现在两类场景：
- **ODP MR**：页未 pin，khugepaged 迁移页面会触发 MR 失效与重新缺页，带来
  延迟抖动甚至正确性边界问题。
- **fork() + COW**：子进程写时复制可能让父进程注册的页被换到新物理页
  （配合 `ibv_fork_init()` / `RDMAV_FORK_SAFE=1` 规避，见阶段五 5.4）。

因此并非"THP 与 RDMA 不兼容"，而是：**ODP 或 fork 场景下需谨慎**；追求
确定性大页性能时，优先用显式 `MAP_HUGETLB` 而非依赖 THP 自动合并。

正确做法：

```bash
# 关闭 THP（或至少关闭 compaction）
echo never > /sys/kernel/mm/transparent_hugepage/enabled
# 或仅关闭 compaction
echo never > /sys/kernel/mm/transparent_hugepage/defrag
```

### NUMA 分配（libnuma 方式）

```c
#include <numa.h>

/* 使用 libnuma 接口，更清晰 */
void *buf = numa_alloc_onnode(buf_size, nic_numa_node);
if (!buf) { fprintf(stderr, "numa_alloc_onnode failed\n"); exit(1); }
/* 注意：numa_alloc_onnode 使用 mmap + mbind，页仍为 4K，
   若需大页须额外组合 MAP_HUGETLB 才能获得 2MB 页面 */
```

---

## 小结

| 特性 | 原理 | 关键 API | 性能收益 | 主要陷阱 |
|------|------|----------|----------|----------|
| 注册缓存 | 复用已 pin 内存，避免重复系统调用 | `ibv_reg_mr` / `ibv_dereg_mr` | 热路径省去 1–10 µs | 地址重用时 key 碰撞；释放顺序错误 |
| ODP | 按需 pin 页，NIC 触发缺页中断 | `IBV_ACCESS_ON_DEMAND`，`ibv_advise_mr` | 零预注册开销，适合稀疏大缓冲 | 首次访问延迟 10–100 µs/页 |
| Memory Windows | 大 MR 划分子窗口，独立 rkey | `ibv_alloc_mw`，`ibv_bind_mw`，`IBV_WR_LOCAL_INV` | 动态授权/撤销无需销毁 MR | 需 QP 支持；Type 2 需驱动版本匹配 |
| 大页 | 减少 MTT 项，提升 NIC TLB 命中率 | `MAP_HUGETLB`，`mlock`，`mbind` | 512× 更少 MTT 项，TLB 覆盖提升 512× | 普通 pin MR 安全；ODP/fork 下慎用 THP；优先显式 `MAP_HUGETLB` |

---

## 本阶段术语速查

> 完整术语表见 [`docs/glossary.md`](glossary.md)。

| 术语 | 含义 |
|------|------|
| **MR** | 已注册内存区域，reg_mr 代价来自 pin + MTT + IOMMU |
| **MTT / MPT** | 内存翻译表 / 保护表；大页大幅减少 MTT 条目数 |
| **lkey / rkey** | 本端 / 对端引用 MR 的密钥 |
| **ODP** | 按需分页，`IBV_ACCESS_ON_DEMAND`，访问时缺页 pin |
| **MW** | 内存窗口，MR 子区域动态授权，可撤销 rkey |
| **IOMMU** | DMA 地址翻译单元，影响注册开销 |
| **PD** | 保护域，MR/MW/QP 的归属边界 |
| **NUMA** | 非一致内存访问，应分配 NUMA-local 内存 |
