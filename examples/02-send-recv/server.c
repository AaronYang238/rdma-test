/*
 * 示例 02 · SEND/RECV 双边乒乓 — 服务端（echo 一方）
 *
 * 教学点：双边操作两端 CPU 都参与；接收方必须先 post_recv 才能接收，否则
 * 触发 RNR（Receiver Not Ready）。本例服务端做 echo：收到 ping 立即回 pong。
 * 对应 CLAUDE.md 第 5 节；并为阶段三的延迟测量打基础。
 */
#include "rdma_common.h"

#define MSG_SIZE 64

int main(int argc, char **argv)
{
    const char *bind_addr = (argc >= 2) ? argv[1] : "0.0.0.0";
    const char *bind_port = (argc >= 3) ? argv[2] : "7471";
    int iters = (argc >= 4) ? atoi(argv[3]) : 10000;

    struct rdma_addrinfo hints, *res = NULL;
    struct rdma_cm_id *listen_id = NULL, *id = NULL;
    struct ibv_qp_init_attr qp_attr;
    char recv_buf[MSG_SIZE], send_buf[MSG_SIZE];
    struct ibv_mr *recv_mr, *send_mr;

    memset(&hints, 0, sizeof(hints));
    hints.ai_port_space = RDMA_PS_TCP;
    hints.ai_flags = RAI_PASSIVE;
    check_zero(rdma_getaddrinfo((char *)bind_addr, (char *)bind_port, &hints, &res),
               "rdma_getaddrinfo");

    fill_qp_attr(&qp_attr);
    check_zero(rdma_create_ep(&listen_id, res, NULL, &qp_attr), "rdma_create_ep");
    check_zero(rdma_listen(listen_id, 1), "rdma_listen");
    printf("[server] listening on %s:%s (%d iters)\n", bind_addr, bind_port, iters);

    check_zero(rdma_get_request(listen_id, &id), "rdma_get_request");

    recv_mr = rdma_reg_msgs(id, recv_buf, MSG_SIZE);
    if (!recv_mr) die_rdma("rdma_reg_msgs recv");
    send_mr = rdma_reg_msgs(id, send_buf, MSG_SIZE);
    if (!send_mr) die_rdma("rdma_reg_msgs send");

    /* 第一个 recv 必须在 accept 前预投递 */
    check_zero(rdma_post_recv(id, NULL, recv_buf, MSG_SIZE, recv_mr), "post_recv");
    check_zero(rdma_accept(id, NULL), "rdma_accept");
    printf("[server] accepted, echoing...\n");

    for (int i = 0; i < iters; i++) {
        wait_recv_comp(id, "recv ping");
        memcpy(send_buf, recv_buf, MSG_SIZE);            /* echo 回去 */
        check_zero(rdma_post_send(id, NULL, send_buf, MSG_SIZE, send_mr, IBV_SEND_SIGNALED),
                   "post_send pong");
        if (i + 1 < iters) {
            /* 为下一发 ping 预投递接收 */
            check_zero(rdma_post_recv(id, NULL, recv_buf, MSG_SIZE, recv_mr), "post_recv");
        }
        wait_send_comp(id, "send pong");
    }

    printf("[server] done %d echoes, disconnecting\n", iters);
    check_zero(rdma_disconnect(id), "rdma_disconnect");
    check_zero(rdma_dereg_mr(send_mr), "dereg send");
    check_zero(rdma_dereg_mr(recv_mr), "dereg recv");
    rdma_destroy_ep(id);
    rdma_destroy_ep(listen_id);
    rdma_freeaddrinfo(res);
    return 0;
}
