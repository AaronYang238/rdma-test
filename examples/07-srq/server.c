/*
 * 示例 07 · SRQ（共享接收队列）— 服务端
 *
 * 服务端创建**一个 SRQ**，让多个连接（QP）共享同一份预投递的接收 WR。
 * 两个客户端各发一条 SEND，服务端从共享 SRQ 取出，用 wc.qp_num 区分来源。
 *
 * 对比无 SRQ：每个 QP 各自维护 RQ，内存 = QP数 × N；有 SRQ：内存 = 单份 N。
 * 注意：用 SRQ 必须手工 ibv_create_qp（把 init_attr.srq 指过去），不能用
 * rdma_create_ep 的自动 QP。所有 QP 与 SRQ 必须在同一个 PD 下。
 * 对应 docs/stage4-scalability.md 4.1。
 */
#include "rdma_common.h"

#define N_CLIENTS 2
#define SRQ_DEPTH 8        /* 共享的预投递深度：无论多少 QP 都只此一份 */
#define MSG_SIZE 64

struct recv_slot {
    char buf[MSG_SIZE];
};

int main(int argc, char **argv)
{
    const char *bind_addr = (argc >= 2) ? argv[1] : "0.0.0.0";
    const char *bind_port = (argc >= 3) ? argv[2] : "7471";

    struct rdma_addrinfo hints, *res = NULL;
    struct rdma_cm_id *listen_id = NULL;
    struct rdma_cm_id *conn[N_CLIENTS] = {0};

    memset(&hints, 0, sizeof(hints));
    hints.ai_port_space = RDMA_PS_TCP;
    hints.ai_flags = RAI_PASSIVE;
    check_zero(rdma_getaddrinfo((char *)bind_addr, (char *)bind_port, &hints, &res),
               "rdma_getaddrinfo");

    /* 仅监听：不需要 rdma_create_ep 自动建 QP，传 NULL qp_attr */
    check_zero(rdma_create_ep(&listen_id, res, NULL, NULL), "rdma_create_ep");
    check_zero(rdma_listen(listen_id, N_CLIENTS), "rdma_listen");
    printf("[server] listening on %s:%s, 等待 %d 个客户端共享一个 SRQ\n",
           bind_addr, bind_port, N_CLIENTS);

    /* 用监听 id 的设备上下文自建 PD / CQ / SRQ，供所有连接共享 */
    struct ibv_context *verbs = listen_id->verbs;
    struct ibv_pd *pd = ibv_alloc_pd(verbs);
    if (!pd) die_rdma("ibv_alloc_pd");
    struct ibv_cq *cq = ibv_create_cq(verbs, 16, NULL, NULL, 0);
    if (!cq) die_rdma("ibv_create_cq");

    struct ibv_srq_init_attr srq_attr;
    memset(&srq_attr, 0, sizeof(srq_attr));
    srq_attr.attr.max_wr = SRQ_DEPTH;
    srq_attr.attr.max_sge = 1;
    struct ibv_srq *srq = ibv_create_srq(pd, &srq_attr);
    if (!srq) die_rdma("ibv_create_srq");

    /* 预投递接收缓冲到 SRQ —— 仅此一份，被所有 QP 共享 */
    struct recv_slot slots[SRQ_DEPTH];
    struct ibv_mr *slots_mr = ibv_reg_mr(pd, slots, sizeof(slots), IBV_ACCESS_LOCAL_WRITE);
    if (!slots_mr) die_rdma("ibv_reg_mr slots");
    for (int i = 0; i < SRQ_DEPTH; i++) {
        struct ibv_sge sge = {
            .addr = (uint64_t)(uintptr_t)slots[i].buf,
            .length = MSG_SIZE,
            .lkey = slots_mr->lkey,
        };
        struct ibv_recv_wr wr = { .wr_id = (uint64_t)i, .sg_list = &sge, .num_sge = 1 }, *bad;
        check_zero(ibv_post_srq_recv(srq, &wr, &bad), "ibv_post_srq_recv");
    }
    printf("[server] SRQ 预投递 %d 个接收 WR（共享，与连接数无关）\n", SRQ_DEPTH);

    /* 接受 N 个连接，每个 QP 都绑定到同一个 SRQ */
    for (int c = 0; c < N_CLIENTS; c++) {
        check_zero(rdma_get_request(listen_id, &conn[c]), "rdma_get_request");

        struct ibv_qp_init_attr qp_attr;
        memset(&qp_attr, 0, sizeof(qp_attr));
        qp_attr.qp_type = IBV_QPT_RC;
        qp_attr.sq_sig_all = 1;
        qp_attr.send_cq = cq;
        qp_attr.recv_cq = cq;
        qp_attr.srq = srq;                 /* ← 关键：接收走共享 SRQ */
        qp_attr.cap.max_send_wr = 4;
        qp_attr.cap.max_send_sge = 1;
        /* 注意：用 SRQ 时 cap.max_recv_wr 由 SRQ 决定，这里无需设置 */
        check_zero(rdma_create_qp(conn[c], pd, &qp_attr), "rdma_create_qp");
        check_zero(rdma_accept(conn[c], NULL), "rdma_accept");
        printf("[server] 接受连接 #%d，QP num=%u（共享 SRQ）\n", c, conn[c]->qp->qp_num);
    }

    /* 从共享 CQ 收两条消息，用 qp_num 区分是哪个连接发来的 */
    int got = 0;
    while (got < N_CLIENTS) {
        struct ibv_wc wc;
        int n = ibv_poll_cq(cq, 1, &wc);
        if (n < 0) die_rdma("ibv_poll_cq");
        if (n == 0) continue;
        if (wc.status != IBV_WC_SUCCESS) {
            fprintf(stderr, "recv wc status=%s\n", ibv_wc_status_str(wc.status));
            exit(EXIT_FAILURE);
        }
        struct recv_slot *s = &slots[wc.wr_id];
        printf("[server] 收到来自 QP num=%u 的消息（SRQ slot %" PRIu64 "）：\"%s\"\n",
               wc.qp_num, (uint64_t)wc.wr_id, s->buf);
        got++;
    }
    printf("[server] 两条消息都从同一个 SRQ 取出，内存只用了单份 %d 个 WR\n", SRQ_DEPTH);

    for (int c = 0; c < N_CLIENTS; c++) {
        rdma_disconnect(conn[c]);
        rdma_destroy_qp(conn[c]);
        rdma_destroy_id(conn[c]);
    }
    check_zero(ibv_dereg_mr(slots_mr), "dereg slots");
    check_zero(ibv_destroy_srq(srq), "destroy srq");
    check_zero(ibv_destroy_cq(cq), "destroy cq");
    check_zero(ibv_dealloc_pd(pd), "dealloc pd");
    rdma_destroy_ep(listen_id);
    rdma_freeaddrinfo(res);
    return 0;
}
