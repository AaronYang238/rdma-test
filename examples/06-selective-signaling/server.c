/*
 * 示例 06 · 选择性 signaling — 服务端（被写入方）
 *
 * 服务端只提供一块开放 REMOTE_WRITE 的目标内存，把 addr/rkey 告诉客户端，
 * 然后等待客户端做完两轮 RDMA WRITE 压测后发来的 ACK。
 * 单边 WRITE 不消耗服务端 CPU，所有性能差异都来自客户端的 signaling 策略。
 * 对应 docs/stage3-performance.md 3.1。
 */
#include "rdma_common.h"

#define TARGET_SIZE 4096

int main(int argc, char **argv)
{
    const char *bind_addr = (argc >= 2) ? argv[1] : "0.0.0.0";
    const char *bind_port = (argc >= 3) ? argv[2] : "7471";

    struct rdma_addrinfo hints, *res = NULL;
    struct rdma_cm_id *listen_id = NULL, *id = NULL;
    struct ibv_qp_init_attr qp_attr;
    char target[TARGET_SIZE];
    struct control_message send_ctrl, recv_ctrl;
    struct ibv_mr *target_mr, *send_ctrl_mr, *recv_ctrl_mr;

    memset(&hints, 0, sizeof(hints));
    hints.ai_port_space = RDMA_PS_TCP;
    hints.ai_flags = RAI_PASSIVE;
    check_zero(rdma_getaddrinfo((char *)bind_addr, (char *)bind_port, &hints, &res),
               "rdma_getaddrinfo");

    fill_qp_attr(&qp_attr);
    check_zero(rdma_create_ep(&listen_id, res, NULL, &qp_attr), "rdma_create_ep");
    check_zero(rdma_listen(listen_id, 1), "rdma_listen");
    printf("[server] listening on %s:%s\n", bind_addr, bind_port);

    check_zero(rdma_get_request(listen_id, &id), "rdma_get_request");

    /* 目标缓冲必须开放 REMOTE_WRITE，客户端才能写进来 */
    target_mr = ibv_reg_mr(id->pd, target, sizeof(target),
                           IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!target_mr) die_rdma("ibv_reg_mr target");
    send_ctrl_mr = rdma_reg_msgs(id, &send_ctrl, sizeof(send_ctrl));
    if (!send_ctrl_mr) die_rdma("rdma_reg_msgs send_ctrl");
    recv_ctrl_mr = rdma_reg_msgs(id, &recv_ctrl, sizeof(recv_ctrl));
    if (!recv_ctrl_mr) die_rdma("rdma_reg_msgs recv_ctrl");

    /* 预投递接收最终 ACK，必须在 accept 前 */
    check_zero(rdma_post_recv(id, NULL, &recv_ctrl, sizeof(recv_ctrl), recv_ctrl_mr), "post_recv");
    check_zero(rdma_accept(id, NULL), "rdma_accept");
    printf("[server] accepted\n");

    /* 把目标 MR 元数据告诉客户端 */
    memset(&send_ctrl, 0, sizeof(send_ctrl));
    send_ctrl.addr = (uint64_t)(uintptr_t)target;
    send_ctrl.rkey = target_mr->rkey;
    send_ctrl.size = sizeof(target);
    snprintf(send_ctrl.note, sizeof(send_ctrl.note), "write target ready");
    check_zero(rdma_post_send(id, NULL, &send_ctrl, sizeof(send_ctrl), send_ctrl_mr,
                              IBV_SEND_SIGNALED), "post_send mr-info");
    wait_send_comp(id, "send mr-info");

    /* 客户端跑完两轮压测后发来 ACK；服务端 CPU 全程未参与数据搬运 */
    wait_recv_comp(id, "recv ack");
    printf("[server] client done: %s\n", recv_ctrl.note);
    printf("[server] 注意：上面两轮 WRITE 压测，服务端 CPU 完全未参与数据搬运\n");

    check_zero(rdma_disconnect(id), "rdma_disconnect");
    check_zero(ibv_dereg_mr(target_mr), "dereg target");
    check_zero(rdma_dereg_mr(send_ctrl_mr), "dereg send_ctrl");
    check_zero(rdma_dereg_mr(recv_ctrl_mr), "dereg recv_ctrl");
    rdma_destroy_ep(id);
    rdma_destroy_ep(listen_id);
    rdma_freeaddrinfo(res);
    return 0;
}
