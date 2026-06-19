/*
 * 示例 04 · WRITE_WITH_IMM — 客户端（被写 + 收通知一方）
 *
 * 客户端开放可被远端写入的 MR，并预投递一个 recv WR。服务端的 WRITE_WITH_IMM
 * 到达时：数据被直接 DMA 进客户端 buffer，**同时消费这个 recv WR 产生一个
 * recv CQE**，wc.opcode == IBV_WC_RECV_RDMA_WITH_IMM，wc.imm_data 携带立即数。
 * 于是客户端无需额外 ACK 报文即可得知"数据已就绪"。
 *
 * 关键点：用于接收 IMM 的 recv WR 不承载数据（数据走 RDMA 目标 buffer），
 * 但仍必须事先 post_recv，否则触发 RNR。这里仍要读取 wc，故直接用
 * rdma_get_recv_comp 取回 ibv_wc 以读出 imm_data，而不复用丢弃 wc 的封装。
 */
#include "rdma_common.h"
#include <arpa/inet.h> /* ntohl */

#define DEMO_DATA_SIZE 1024

int main(int argc, char **argv)
{
    const char *server_addr = (argc >= 2) ? argv[1] : "127.0.0.1";
    const char *server_port = (argc >= 3) ? argv[2] : "7471";
    struct rdma_addrinfo hints, *res = NULL;
    struct rdma_cm_id *id = NULL;
    struct ibv_qp_init_attr qp_attr;
    char data[DEMO_DATA_SIZE];
    struct control_message send_ctrl;
    char imm_recv_dummy[1]; /* IMM 的 recv WR 不承载数据，占位即可 */
    struct ibv_mr *data_mr, *send_ctrl_mr, *dummy_mr;

    snprintf(data, sizeof(data), "to be overwritten by WRITE_WITH_IMM");

    memset(&hints, 0, sizeof(hints));
    hints.ai_port_space = RDMA_PS_TCP;
    check_zero(rdma_getaddrinfo((char *)server_addr, (char *)server_port, &hints, &res),
               "rdma_getaddrinfo");

    fill_qp_attr(&qp_attr);
    check_zero(rdma_create_ep(&id, res, NULL, &qp_attr), "rdma_create_ep");

    data_mr = ibv_reg_mr(id->pd, data, sizeof(data),
                         IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!data_mr) die_rdma("ibv_reg_mr data");
    send_ctrl_mr = rdma_reg_msgs(id, &send_ctrl, sizeof(send_ctrl));
    if (!send_ctrl_mr) die_rdma("rdma_reg_msgs send_ctrl");
    dummy_mr = rdma_reg_msgs(id, imm_recv_dummy, sizeof(imm_recv_dummy));
    if (!dummy_mr) die_rdma("rdma_reg_msgs dummy");

    /* 预投递 recv 以接收 WRITE_WITH_IMM 的完成通知，必须在 connect 前 */
    check_zero(rdma_post_recv(id, NULL, imm_recv_dummy, sizeof(imm_recv_dummy), dummy_mr),
               "post_recv imm");
    check_zero(rdma_connect(id, NULL), "rdma_connect");
    printf("[client] connected to %s:%s\n", server_addr, server_port);

    /* 把本地可写 MR 的 addr/rkey 告知服务端 */
    memset(&send_ctrl, 0, sizeof(send_ctrl));
    send_ctrl.addr = (uint64_t)(uintptr_t)data;
    send_ctrl.rkey = data_mr->rkey;
    send_ctrl.size = sizeof(data);
    snprintf(send_ctrl.note, sizeof(send_ctrl.note), "client writable MR");
    check_zero(rdma_post_send(id, NULL, &send_ctrl, sizeof(send_ctrl), send_ctrl_mr, IBV_SEND_SIGNALED),
               "post_send mr-info");
    wait_send_comp(id, "send mr-info");

    /* 等 WRITE_WITH_IMM 产生的 recv CQE，并读出立即数 */
    struct ibv_wc wc;
    if (rdma_get_recv_comp(id, &wc) <= 0) die_rdma("rdma_get_recv_comp imm");
    if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "imm recv wc status=%s\n", ibv_wc_status_str(wc.status));
        exit(EXIT_FAILURE);
    }
    if (wc.opcode == IBV_WC_RECV_RDMA_WITH_IMM && (wc.wc_flags & IBV_WC_WITH_IMM)) {
        printf("[client] got WRITE_WITH_IMM, imm=0x%08X\n", ntohl(wc.imm_data));
    } else {
        printf("[client] unexpected wc.opcode=%d\n", wc.opcode);
    }
    printf("[client] buffer after remote write: \"%s\"\n", data);

    check_zero(rdma_disconnect(id), "rdma_disconnect");
    check_zero(ibv_dereg_mr(data_mr), "dereg data");
    check_zero(rdma_dereg_mr(send_ctrl_mr), "dereg send_ctrl");
    check_zero(rdma_dereg_mr(dummy_mr), "dereg dummy");
    rdma_destroy_ep(id);
    rdma_freeaddrinfo(res);
    return 0;
}
