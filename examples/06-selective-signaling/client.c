/*
 * 示例 06 · 选择性 signaling — 客户端（发起方，性能对比）
 *
 * 同样做 N 次 RDMA WRITE，对比两种完成策略：
 *   A. 全 signaled：每个 WR 都 IBV_SEND_SIGNALED，且每发一个就 poll 一次完成
 *      —— 每次操作都被 CQE + poll 拖慢，吞吐低。
 *   B. 选择性 signaled：每 K 个才 signaled 一次，只 poll 这 1/K 的完成
 *      —— RC 保证按序完成，poll 到第 K 个即说明前 K-1 个也已完成，CQE/poll
 *      开销摊薄到 1/K，吞吐显著提升。
 *
 * 关键约束：未 signaled 的 WR 仍占用 SQ 槽位，直到其后的某个 signaled WR 完成
 * 才被回收。因此 in-flight 上限 ≈ K，QP 的 max_send_wr 必须 ≥ K。
 * 对应 docs/stage3-performance.md 3.1。
 */
#include "rdma_common.h"

#define MSG_SIZE 64
#define SIGNAL_EVERY 64        /* 每 64 个 WR signaled 一次 */
#define SQ_DEPTH 256           /* > SIGNAL_EVERY，容纳在途未回收的 WR */

/* 构造一个 RDMA WRITE WR 并投递；signaled 决定是否产生 CQE。 */
static void post_write(struct rdma_cm_id *id, void *buf, uint32_t len, struct ibv_mr *mr,
                       uint64_t remote_addr, uint32_t rkey, bool signaled, uint64_t wr_id)
{
    struct ibv_sge sge = {
        .addr = (uint64_t)(uintptr_t)buf,
        .length = len,
        .lkey = mr->lkey,
    };
    struct ibv_send_wr wr, *bad = NULL;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = wr_id;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_RDMA_WRITE;
    wr.send_flags = signaled ? IBV_SEND_SIGNALED : 0;
    wr.wr.rdma.remote_addr = remote_addr;
    wr.wr.rdma.rkey = rkey;
    check_zero(ibv_post_send(id->qp, &wr, &bad), "ibv_post_send write");
}

/* 阻塞 poll send CQ 直到拿到一个完成（只有 signaled 的 WR 会产生 CQE）。 */
static void poll_one_send(struct ibv_cq *cq, const char *what)
{
    struct ibv_wc wc;
    int n;
    do {
        n = ibv_poll_cq(cq, 1, &wc);
    } while (n == 0);
    if (n < 0) die_rdma(what);
    if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "%s: wc status=%s\n", what, ibv_wc_status_str(wc.status));
        exit(EXIT_FAILURE);
    }
}

int main(int argc, char **argv)
{
    const char *server_addr = (argc >= 2) ? argv[1] : "127.0.0.1";
    const char *server_port = (argc >= 3) ? argv[2] : "7471";
    int iters = (argc >= 4) ? atoi(argv[3]) : 200000;
    if (iters % SIGNAL_EVERY) iters -= iters % SIGNAL_EVERY; /* 取 K 的整数倍 */

    struct rdma_addrinfo hints, *res = NULL;
    struct rdma_cm_id *id = NULL;
    struct ibv_qp_init_attr qp_attr;
    char src[MSG_SIZE];
    struct control_message recv_ctrl, send_ctrl;
    struct ibv_mr *src_mr, *recv_ctrl_mr, *send_ctrl_mr;

    memset(&hints, 0, sizeof(hints));
    hints.ai_port_space = RDMA_PS_TCP;
    check_zero(rdma_getaddrinfo((char *)server_addr, (char *)server_port, &hints, &res),
               "rdma_getaddrinfo");

    /* 加深 SQ：选择性 signaling 下会有多达 SIGNAL_EVERY 个未回收 WR 在途 */
    fill_qp_attr(&qp_attr);
    qp_attr.cap.max_send_wr = SQ_DEPTH;
    check_zero(rdma_create_ep(&id, res, NULL, &qp_attr), "rdma_create_ep");

    memset(src, 'w', sizeof(src));
    src_mr = ibv_reg_mr(id->pd, src, sizeof(src), IBV_ACCESS_LOCAL_WRITE);
    if (!src_mr) die_rdma("ibv_reg_mr src");
    recv_ctrl_mr = rdma_reg_msgs(id, &recv_ctrl, sizeof(recv_ctrl));
    if (!recv_ctrl_mr) die_rdma("rdma_reg_msgs recv_ctrl");
    send_ctrl_mr = rdma_reg_msgs(id, &send_ctrl, sizeof(send_ctrl));
    if (!send_ctrl_mr) die_rdma("rdma_reg_msgs send_ctrl");

    check_zero(rdma_post_recv(id, NULL, &recv_ctrl, sizeof(recv_ctrl), recv_ctrl_mr), "post_recv");
    check_zero(rdma_connect(id, NULL), "rdma_connect");
    printf("[client] connected to %s:%s\n", server_addr, server_port);

    wait_recv_comp(id, "recv mr-info");
    uint64_t raddr = recv_ctrl.addr;
    uint32_t rkey = recv_ctrl.rkey;
    printf("[client] server target: addr=0x%" PRIx64 ", rkey=%" PRIu32 "\n", raddr, rkey);
    printf("[client] 每轮 %d 次 RDMA WRITE（%d B），SIGNAL_EVERY=%d\n",
           iters, MSG_SIZE, SIGNAL_EVERY);

    /* ---------- 轮 A：全 signaled，每发一个 poll 一个 ---------- */
    uint64_t a0 = now_ns();
    for (int i = 0; i < iters; i++) {
        post_write(id, src, MSG_SIZE, src_mr, raddr, rkey, true, i);
        poll_one_send(id->qp->send_cq, "write A");
    }
    uint64_t a1 = now_ns();
    double a_us = (a1 - a0) / 1000.0;

    /* ---------- 轮 B：每 K 个 signaled 一次，只 poll 1/K ---------- */
    uint64_t b0 = now_ns();
    for (int i = 0; i < iters; i++) {
        bool signaled = ((i + 1) % SIGNAL_EVERY == 0);
        post_write(id, src, MSG_SIZE, src_mr, raddr, rkey, signaled, i);
        if (signaled) poll_one_send(id->qp->send_cq, "write B"); /* 顺带回收前 K-1 个 */
    }
    uint64_t b1 = now_ns();
    double b_us = (b1 - b0) / 1000.0;

    printf("\n[结果]\n");
    printf("  A 全 signaled    : %9.1f us  (%.2f Mops, %.0f ns/op)\n",
           a_us, iters / a_us, a_us * 1000.0 / iters);
    printf("  B 选择性 signaled: %9.1f us  (%.2f Mops, %.0f ns/op)\n",
           b_us, iters / b_us, b_us * 1000.0 / iters);
    printf("  加速比 A/B       : %.2fx\n", a_us / b_us);

    /* 通知服务端收尾 */
    memset(&send_ctrl, 0, sizeof(send_ctrl));
    snprintf(send_ctrl.note, sizeof(send_ctrl.note), "selective-signaling bench done");
    check_zero(rdma_post_send(id, NULL, &send_ctrl, sizeof(send_ctrl), send_ctrl_mr,
                              IBV_SEND_SIGNALED), "post_send ack");
    wait_send_comp(id, "send ack");

    check_zero(rdma_disconnect(id), "rdma_disconnect");
    check_zero(ibv_dereg_mr(src_mr), "dereg src");
    check_zero(rdma_dereg_mr(recv_ctrl_mr), "dereg recv_ctrl");
    check_zero(rdma_dereg_mr(send_ctrl_mr), "dereg send_ctrl");
    rdma_destroy_ep(id);
    rdma_freeaddrinfo(res);
    return 0;
}
