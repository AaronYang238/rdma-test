/*
 * 示例 05 · ATOMIC（FETCH_ADD / CMP_SWAP）— 服务端（被操作一方）
 *
 * RDMA 原子操作直接在网卡上对对端内存的一个 64 位字做"读-改-写"，全程不打扰
 * 对端 CPU，可用于分布式锁、无锁计数器、序列号分配等。服务端只需：
 *   1) 把一个 8 字节对齐的 uint64_t 计数器注册为 IBV_ACCESS_REMOTE_ATOMIC；
 *   2) 通过 SEND 告知其 addr/rkey；
 *   3) 等客户端做完原子操作后回的 ACK，再打印计数器最终值。
 * 对应 CLAUDE.md 第 6 节扩展；TODO 阶段二 2.5。
 */
#include "rdma_common.h"

int main(int argc, char **argv)
{
    const char *bind_addr = (argc >= 2) ? argv[1] : "0.0.0.0";
    const char *bind_port = (argc >= 3) ? argv[2] : "7471";
    struct rdma_addrinfo hints, *res = NULL;
    struct rdma_cm_id *listen_id = NULL, *id = NULL;
    struct ibv_qp_init_attr qp_attr;
    /* 原子操作目标必须 8 字节对齐 */
    uint64_t counter __attribute__((aligned(8))) = 100;
    struct control_message send_ctrl, recv_ctrl;
    struct ibv_mr *counter_mr, *send_ctrl_mr, *recv_ctrl_mr;

    memset(&hints, 0, sizeof(hints));
    hints.ai_port_space = RDMA_PS_TCP;
    hints.ai_flags = RAI_PASSIVE;
    check_zero(rdma_getaddrinfo((char *)bind_addr, (char *)bind_port, &hints, &res),
               "rdma_getaddrinfo");

    fill_qp_attr(&qp_attr);
    check_zero(rdma_create_ep(&listen_id, res, NULL, &qp_attr), "rdma_create_ep");
    check_zero(rdma_listen(listen_id, 1), "rdma_listen");
    printf("[server] listening on %s:%s, initial counter=%" PRIu64 "\n",
           bind_addr, bind_port, counter);

    check_zero(rdma_get_request(listen_id, &id), "rdma_get_request");

    /* 关键：开放 REMOTE_ATOMIC（通常也需 REMOTE_WRITE/READ） */
    counter_mr = ibv_reg_mr(id->pd, &counter, sizeof(counter),
                            IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                                IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC);
    if (!counter_mr) die_rdma("ibv_reg_mr counter");
    send_ctrl_mr = rdma_reg_msgs(id, &send_ctrl, sizeof(send_ctrl));
    if (!send_ctrl_mr) die_rdma("rdma_reg_msgs send_ctrl");
    recv_ctrl_mr = rdma_reg_msgs(id, &recv_ctrl, sizeof(recv_ctrl));
    if (!recv_ctrl_mr) die_rdma("rdma_reg_msgs recv_ctrl");

    check_zero(rdma_post_recv(id, NULL, &recv_ctrl, sizeof(recv_ctrl), recv_ctrl_mr), "post_recv");
    check_zero(rdma_accept(id, NULL), "rdma_accept");
    printf("[server] accepted\n");

    memset(&send_ctrl, 0, sizeof(send_ctrl));
    send_ctrl.addr = (uint64_t)(uintptr_t)&counter;
    send_ctrl.rkey = counter_mr->rkey;
    send_ctrl.size = sizeof(counter);
    snprintf(send_ctrl.note, sizeof(send_ctrl.note), "server atomic counter");
    check_zero(rdma_post_send(id, NULL, &send_ctrl, sizeof(send_ctrl), send_ctrl_mr, IBV_SEND_SIGNALED),
               "post_send mr-info");
    wait_send_comp(id, "send mr-info");

    /* 等客户端做完原子操作后回的 ACK；此时 counter 已被网卡原子修改 */
    wait_recv_comp(id, "recv ack");
    printf("[server] client done: %s\n", recv_ctrl.note);
    printf("[server] final counter=%" PRIu64 " (服务端 CPU 未参与任何修改)\n", counter);

    check_zero(rdma_disconnect(id), "rdma_disconnect");
    check_zero(ibv_dereg_mr(counter_mr), "dereg counter");
    check_zero(rdma_dereg_mr(send_ctrl_mr), "dereg send_ctrl");
    check_zero(rdma_dereg_mr(recv_ctrl_mr), "dereg recv_ctrl");
    rdma_destroy_ep(id);
    rdma_destroy_ep(listen_id);
    rdma_freeaddrinfo(res);
    return 0;
}
