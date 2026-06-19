/*
 * 示例 03 · RDMA READ（单边读）— 服务端（被读一方）
 *
 * 与 WRITE 对称：这次由客户端发起 READ，从服务端内存把数据拉走。服务端只需
 * 把一段开放了 IBV_ACCESS_REMOTE_READ 的内存的 addr/rkey 告知客户端，之后
 * **CPU 不参与**实际数据搬运，仅等待客户端读完后的 ACK。
 * 对应 CLAUDE.md 第 6 节。
 */
#include "rdma_common.h"

#define DEMO_DATA_SIZE 1024

int main(int argc, char **argv)
{
    const char *bind_addr = (argc >= 2) ? argv[1] : "0.0.0.0";
    const char *bind_port = (argc >= 3) ? argv[2] : "7471";
    struct rdma_addrinfo hints, *res = NULL;
    struct rdma_cm_id *listen_id = NULL, *id = NULL;
    struct ibv_qp_init_attr qp_attr;
    char data[DEMO_DATA_SIZE];
    struct control_message send_ctrl, recv_ctrl;
    struct ibv_mr *data_mr, *send_ctrl_mr, *recv_ctrl_mr;

    snprintf(data, sizeof(data), "Payload exposed by server for client RDMA Read, pid=%d", getpid());

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

    /* 开放 REMOTE_READ，客户端才能 RDMA Read */
    data_mr = ibv_reg_mr(id->pd, data, sizeof(data),
                         IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ);
    if (!data_mr) die_rdma("ibv_reg_mr data");
    send_ctrl_mr = rdma_reg_msgs(id, &send_ctrl, sizeof(send_ctrl));
    if (!send_ctrl_mr) die_rdma("rdma_reg_msgs send_ctrl");
    recv_ctrl_mr = rdma_reg_msgs(id, &recv_ctrl, sizeof(recv_ctrl));
    if (!recv_ctrl_mr) die_rdma("rdma_reg_msgs recv_ctrl");

    /* 预投递接收客户端读完后的 ACK */
    check_zero(rdma_post_recv(id, NULL, &recv_ctrl, sizeof(recv_ctrl), recv_ctrl_mr), "post_recv");
    check_zero(rdma_accept(id, NULL), "rdma_accept");
    printf("[server] accepted\n");

    /* SEND 自己的 MR 元数据，供客户端发起 READ */
    memset(&send_ctrl, 0, sizeof(send_ctrl));
    send_ctrl.addr = (uint64_t)(uintptr_t)data;
    send_ctrl.rkey = data_mr->rkey;
    send_ctrl.size = (uint32_t)(strlen(data) + 1);
    snprintf(send_ctrl.note, sizeof(send_ctrl.note), "server readable MR");
    check_zero(rdma_post_send(id, NULL, &send_ctrl, sizeof(send_ctrl), send_ctrl_mr, IBV_SEND_SIGNALED),
               "post_send mr-info");
    wait_send_comp(id, "send mr-info");

    /* 等客户端读完后回的 ACK（服务端对实际 READ 无感知） */
    wait_recv_comp(id, "recv ack");
    printf("[server] client reported: %s\n", recv_ctrl.note);

    check_zero(rdma_disconnect(id), "rdma_disconnect");
    check_zero(ibv_dereg_mr(data_mr), "dereg data");
    check_zero(rdma_dereg_mr(send_ctrl_mr), "dereg send_ctrl");
    check_zero(rdma_dereg_mr(recv_ctrl_mr), "dereg recv_ctrl");
    rdma_destroy_ep(id);
    rdma_destroy_ep(listen_id);
    rdma_freeaddrinfo(res);
    return 0;
}
