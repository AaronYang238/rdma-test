/*
 * 示例 03 · RDMA READ（单边读）— 客户端（发起方）
 *
 * 客户端收到服务端 MR 的 addr/rkey 后，用 rdma_post_read 把对端内存的数据
 * 拉进本地 buffer。READ 完成同样在**发送队列**产生 CQE（用 wait_send_comp 取）。
 * 读完后 SEND 一个 ACK 告知服务端可以收尾。
 */
#include "rdma_common.h"

#define DEMO_DATA_SIZE 1024

int main(int argc, char **argv)
{
    const char *server_addr = (argc >= 2) ? argv[1] : "127.0.0.1";
    const char *server_port = (argc >= 3) ? argv[2] : "7471";
    struct rdma_addrinfo hints, *res = NULL;
    struct rdma_cm_id *id = NULL;
    struct ibv_qp_init_attr qp_attr;
    char local[DEMO_DATA_SIZE];
    struct control_message recv_ctrl, send_ctrl;
    struct ibv_mr *local_mr, *recv_ctrl_mr, *send_ctrl_mr;

    memset(local, 0, sizeof(local));

    memset(&hints, 0, sizeof(hints));
    hints.ai_port_space = RDMA_PS_TCP;
    check_zero(rdma_getaddrinfo((char *)server_addr, (char *)server_port, &hints, &res),
               "rdma_getaddrinfo");

    fill_qp_attr(&qp_attr);
    check_zero(rdma_create_ep(&id, res, NULL, &qp_attr), "rdma_create_ep");

    /* 本地目标 buffer 需要 LOCAL_WRITE：READ 是网卡往本地写 */
    local_mr = ibv_reg_mr(id->pd, local, sizeof(local), IBV_ACCESS_LOCAL_WRITE);
    if (!local_mr) die_rdma("ibv_reg_mr local");
    recv_ctrl_mr = rdma_reg_msgs(id, &recv_ctrl, sizeof(recv_ctrl));
    if (!recv_ctrl_mr) die_rdma("rdma_reg_msgs recv_ctrl");
    send_ctrl_mr = rdma_reg_msgs(id, &send_ctrl, sizeof(send_ctrl));
    if (!send_ctrl_mr) die_rdma("rdma_reg_msgs send_ctrl");

    /* 预投递接收服务端 MR 元数据 */
    check_zero(rdma_post_recv(id, NULL, &recv_ctrl, sizeof(recv_ctrl), recv_ctrl_mr), "post_recv");
    check_zero(rdma_connect(id, NULL), "rdma_connect");
    printf("[client] connected to %s:%s\n", server_addr, server_port);

    wait_recv_comp(id, "recv mr-info");
    printf("[client] server MR: addr=0x%" PRIx64 ", rkey=%" PRIu32 ", size=%" PRIu32 "\n",
           recv_ctrl.addr, recv_ctrl.rkey, recv_ctrl.size);

    /* 发起 RDMA READ：从对端内存拉数据到 local */
    uint32_t len = recv_ctrl.size;
    if (len > sizeof(local)) len = sizeof(local);
    check_zero(rdma_post_read(id, NULL, local, len, local_mr, IBV_SEND_SIGNALED,
                              recv_ctrl.addr, recv_ctrl.rkey),
               "rdma_post_read");
    wait_send_comp(id, "read completion");
    printf("[client] read from remote: \"%s\"\n", local);

    /* 告知服务端读取完成 */
    memset(&send_ctrl, 0, sizeof(send_ctrl));
    snprintf(send_ctrl.note, sizeof(send_ctrl.note), "RDMA read completed");
    check_zero(rdma_post_send(id, NULL, &send_ctrl, sizeof(send_ctrl), send_ctrl_mr, IBV_SEND_SIGNALED),
               "post_send ack");
    wait_send_comp(id, "send ack");

    check_zero(rdma_disconnect(id), "rdma_disconnect");
    check_zero(ibv_dereg_mr(local_mr), "dereg local");
    check_zero(rdma_dereg_mr(recv_ctrl_mr), "dereg recv_ctrl");
    check_zero(rdma_dereg_mr(send_ctrl_mr), "dereg send_ctrl");
    rdma_destroy_ep(id);
    rdma_freeaddrinfo(res);
    return 0;
}
