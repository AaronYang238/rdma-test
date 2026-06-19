/*
 * 示例 04 · WRITE_WITH_IMM（带立即数的单边写）— 服务端（发起方）
 *
 * 普通 RDMA WRITE 对端 CPU 完全无感，问题是：对端如何知道"写好了、可以读了"？
 * 示例 01 的做法是 WRITE 之后再补一发 SEND 当 ACK。更高效的做法是
 * WRITE_WITH_IMM：在写的同时携带一个 32 位立即数，**对端 RQ 会消费一个 WR
 * 并产生 recv CQE**（wc.opcode = IBV_WC_RECV_RDMA_WITH_IMM，wc.imm_data 即立即数）。
 * 一次操作既送数据又送通知，省掉一来回。
 *
 * 注意：librdmacm 的便捷封装 rdma_post_write 不支持立即数，必须用 ibv 原语
 * 手工构造 ibv_send_wr，opcode = IBV_WR_RDMA_WRITE_WITH_IMM。
 * 对应 CLAUDE.md 第 6/7 节；TODO 阶段二 2.4。
 */
#include "rdma_common.h"
#include <arpa/inet.h> /* htonl */

#define DEMO_DATA_SIZE 1024

int main(int argc, char **argv)
{
    const char *bind_addr = (argc >= 2) ? argv[1] : "0.0.0.0";
    const char *bind_port = (argc >= 3) ? argv[2] : "7471";
    struct rdma_addrinfo hints, *res = NULL;
    struct rdma_cm_id *listen_id = NULL, *id = NULL;
    struct ibv_qp_init_attr qp_attr;
    char data[DEMO_DATA_SIZE];
    struct control_message recv_ctrl;
    struct ibv_mr *data_mr, *recv_ctrl_mr;

    snprintf(data, sizeof(data), "Hello via WRITE_WITH_IMM from server pid=%d", getpid());

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

    data_mr = ibv_reg_mr(id->pd, data, sizeof(data), IBV_ACCESS_LOCAL_WRITE);
    if (!data_mr) die_rdma("ibv_reg_mr data");
    recv_ctrl_mr = rdma_reg_msgs(id, &recv_ctrl, sizeof(recv_ctrl));
    if (!recv_ctrl_mr) die_rdma("rdma_reg_msgs recv_ctrl");

    /* 预投递接收客户端的 MR 元数据 */
    check_zero(rdma_post_recv(id, NULL, &recv_ctrl, sizeof(recv_ctrl), recv_ctrl_mr), "post_recv");
    check_zero(rdma_accept(id, NULL), "rdma_accept");
    printf("[server] accepted\n");

    /* 收到客户端 addr/rkey */
    wait_recv_comp(id, "recv mr-info");
    printf("[server] remote MR: addr=0x%" PRIx64 ", rkey=%" PRIu32 "\n",
           recv_ctrl.addr, recv_ctrl.rkey);

    /* WRITE_WITH_IMM：手工构造 WR，写数据 + 携带立即数 */
    uint32_t imm = 0xCAFEF00D;
    struct ibv_sge sge = {
        .addr = (uint64_t)(uintptr_t)data,
        .length = (uint32_t)(strlen(data) + 1),
        .lkey = data_mr->lkey,
    };
    struct ibv_send_wr wr, *bad = NULL;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = 1;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.imm_data = htonl(imm); /* 立即数走线时为网络字节序，对端用 ntohl 还原 */
    wr.wr.rdma.remote_addr = recv_ctrl.addr;
    wr.wr.rdma.rkey = recv_ctrl.rkey;
    check_zero(ibv_post_send(id->qp, &wr, &bad), "ibv_post_send write_with_imm");
    wait_send_comp(id, "write_with_imm completion");
    printf("[server] WRITE_WITH_IMM done (imm=0x%08X), client will get a recv CQE\n", imm);

    check_zero(rdma_disconnect(id), "rdma_disconnect");
    check_zero(ibv_dereg_mr(data_mr), "dereg data");
    check_zero(rdma_dereg_mr(recv_ctrl_mr), "dereg recv_ctrl");
    rdma_destroy_ep(id);
    rdma_destroy_ep(listen_id);
    rdma_freeaddrinfo(res);
    return 0;
}
