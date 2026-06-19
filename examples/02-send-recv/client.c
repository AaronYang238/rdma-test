/*
 * 示例 02 · SEND/RECV 双边乒乓 — 客户端（计时一方）
 *
 * 发 ping、等 pong，循环 N 次，测量平均往返延迟（RTT）与单向延迟（RTT/2）。
 * 这是 RDMA 延迟基准最朴素的形态；阶段三会在此基础上引入 inline、选择性
 * signaling、busy-poll 等优化并对比。
 */
#include "rdma_common.h"

#define MSG_SIZE 64

int main(int argc, char **argv)
{
    const char *server_addr = (argc >= 2) ? argv[1] : "127.0.0.1";
    const char *server_port = (argc >= 3) ? argv[2] : "7471";
    int iters = (argc >= 4) ? atoi(argv[3]) : 10000;

    struct rdma_addrinfo hints, *res = NULL;
    struct rdma_cm_id *id = NULL;
    struct ibv_qp_init_attr qp_attr;
    char send_buf[MSG_SIZE], recv_buf[MSG_SIZE];
    struct ibv_mr *send_mr, *recv_mr;

    memset(&hints, 0, sizeof(hints));
    hints.ai_port_space = RDMA_PS_TCP;
    check_zero(rdma_getaddrinfo((char *)server_addr, (char *)server_port, &hints, &res),
               "rdma_getaddrinfo");

    fill_qp_attr(&qp_attr);
    check_zero(rdma_create_ep(&id, res, NULL, &qp_attr), "rdma_create_ep");

    send_mr = rdma_reg_msgs(id, send_buf, MSG_SIZE);
    if (!send_mr) die_rdma("rdma_reg_msgs send");
    recv_mr = rdma_reg_msgs(id, recv_buf, MSG_SIZE);
    if (!recv_mr) die_rdma("rdma_reg_msgs recv");

    /* 预投递接收第一个 pong，必须在 connect 前 */
    check_zero(rdma_post_recv(id, NULL, recv_buf, MSG_SIZE, recv_mr), "post_recv");
    check_zero(rdma_connect(id, NULL), "rdma_connect");
    printf("[client] connected to %s:%s, ping-pong %d iters\n", server_addr, server_port, iters);

    memset(send_buf, 'p', MSG_SIZE);
    uint64_t t0 = now_ns();
    for (int i = 0; i < iters; i++) {
        check_zero(rdma_post_send(id, NULL, send_buf, MSG_SIZE, send_mr, IBV_SEND_SIGNALED),
                   "post_send ping");
        wait_send_comp(id, "send ping");
        wait_recv_comp(id, "recv pong");
        if (i + 1 < iters) {
            check_zero(rdma_post_recv(id, NULL, recv_buf, MSG_SIZE, recv_mr), "post_recv");
        }
    }
    uint64_t t1 = now_ns();

    double total_us = (t1 - t0) / 1000.0;
    double rtt_us = total_us / iters;
    printf("[client] %d round-trips in %.1f us\n", iters, total_us);
    printf("[client] avg RTT = %.2f us, one-way ~= %.2f us (msg %d B)\n",
           rtt_us, rtt_us / 2.0, MSG_SIZE);

    check_zero(rdma_disconnect(id), "rdma_disconnect");
    check_zero(rdma_dereg_mr(recv_mr), "dereg recv");
    check_zero(rdma_dereg_mr(send_mr), "dereg send");
    rdma_destroy_ep(id);
    rdma_freeaddrinfo(res);
    return 0;
}
