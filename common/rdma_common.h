#ifndef RDMA_TUTORIAL_COMMON_H
#define RDMA_TUTORIAL_COMMON_H

/*
 * 暴露 POSIX 接口（clock_gettime / CLOCK_MONOTONIC 等）。
 * 严格 -std=c11 下默认不声明 POSIX 符号，必须在任何系统头之前定义此宏。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

/*
 * 教程公共脚手架（被各 examples/ 复用）。
 *
 * 设计目标：把建链、MR 注册、CQ 轮询、计时这类**重复样板**收敛到一处，
 * 让每个示例只聚焦它要讲的那一个知识点。
 * 风格沿用仓库既有的 die_rdma / check_zero / wait_*_comp 约定。
 */

#include <errno.h>
#include <inttypes.h>
#include <netdb.h>
#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* 控制面消息：交换 MR 元数据（addr + rkey + size）与一段备注。 */
struct control_message {
    uint64_t addr;
    uint32_t rkey;
    uint32_t size;
    char note[64];
};

/* ---------- 错误处理 ---------- */

static inline void die_perror(const char *what)
{
    perror(what);
    exit(EXIT_FAILURE);
}

static inline void die_rdma(const char *what)
{
    fprintf(stderr, "%s: %s\n", what, strerror(errno));
    exit(EXIT_FAILURE);
}

static inline void check_zero(int rc, const char *what)
{
    if (rc) {
        die_rdma(what);
    }
}

/* ---------- 完成轮询（便捷封装，底层即 ibv_poll_cq） ---------- */

static inline void wait_send_comp(struct rdma_cm_id *id, const char *what)
{
    struct ibv_wc wc;

    if (rdma_get_send_comp(id, &wc) <= 0) {
        die_rdma(what);
    }
    if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "%s: send wc status=%s\n", what, ibv_wc_status_str(wc.status));
        exit(EXIT_FAILURE);
    }
}

static inline void wait_recv_comp(struct rdma_cm_id *id, const char *what)
{
    struct ibv_wc wc;

    if (rdma_get_recv_comp(id, &wc) <= 0) {
        die_rdma(what);
    }
    if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "%s: recv wc status=%s\n", what, ibv_wc_status_str(wc.status));
        exit(EXIT_FAILURE);
    }
}

/* ---------- 计时（性能示例用） ---------- */

static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ---------- QP 能力默认值 ---------- */

static inline void fill_qp_attr(struct ibv_qp_init_attr *qp_attr)
{
    memset(qp_attr, 0, sizeof(*qp_attr));
    qp_attr->cap.max_send_wr = 16;
    qp_attr->cap.max_recv_wr = 16;
    qp_attr->cap.max_send_sge = 1;
    qp_attr->cap.max_recv_sge = 1;
    qp_attr->sq_sig_all = 1;       /* 教学默认：每个 send 都产生 CQE */
    qp_attr->qp_type = IBV_QPT_RC; /* 可靠连接 */
}

#endif /* RDMA_TUTORIAL_COMMON_H */
