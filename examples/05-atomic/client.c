/*
 * 示例 05 · ATOMIC（FETCH_ADD / CMP_SWAP）— 客户端（发起方）
 *
 * 客户端对服务端的 64 位计数器先做 FETCH_AND_ADD(+5)，再做 COMPARE_AND_SWAP。
 * 两类原子操作都把**操作前的原值**写回客户端本地 8 字节 buffer，并在发送队列
 * 产生 CQE。原子操作必须用 ibv_post_send 手工构造 WR。
 *
 * 语义：
 *   FETCH_AND_ADD：  *remote += compare_add;        返回旧值
 *   COMPARE_AND_SWAP：if (*remote == compare_add) *remote = swap; 返回旧值
 */
#include "rdma_common.h"

static uint64_t do_atomic(struct rdma_cm_id *id, struct ibv_mr *res_mr, uint64_t *result,
                          enum ibv_wr_opcode opcode, uint64_t remote_addr, uint32_t rkey,
                          uint64_t compare_add, uint64_t swap, const char *what)
{
    struct ibv_sge sge = {
        .addr = (uint64_t)(uintptr_t)result,
        .length = sizeof(uint64_t),
        .lkey = res_mr->lkey,
    };
    struct ibv_send_wr wr, *bad = NULL;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = 1;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = opcode;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.atomic.remote_addr = remote_addr;
    wr.wr.atomic.rkey = rkey;
    wr.wr.atomic.compare_add = compare_add; /* FETCH_ADD: 加数；CMP_SWAP: 比较值 */
    wr.wr.atomic.swap = swap;               /* 仅 CMP_SWAP 使用 */
    check_zero(ibv_post_send(id->qp, &wr, &bad), what);
    wait_send_comp(id, what);
    return *result; /* 原子操作前的旧值 */
}

int main(int argc, char **argv)
{
    const char *server_addr = (argc >= 2) ? argv[1] : "127.0.0.1";
    const char *server_port = (argc >= 3) ? argv[2] : "7471";
    struct rdma_addrinfo hints, *res = NULL;
    struct rdma_cm_id *id = NULL;
    struct ibv_qp_init_attr qp_attr;
    uint64_t result __attribute__((aligned(8))) = 0;
    struct control_message recv_ctrl, send_ctrl;
    struct ibv_mr *result_mr, *recv_ctrl_mr, *send_ctrl_mr;

    memset(&hints, 0, sizeof(hints));
    hints.ai_port_space = RDMA_PS_TCP;
    check_zero(rdma_getaddrinfo((char *)server_addr, (char *)server_port, &hints, &res),
               "rdma_getaddrinfo");

    fill_qp_attr(&qp_attr);
    check_zero(rdma_create_ep(&id, res, NULL, &qp_attr), "rdma_create_ep");

    result_mr = ibv_reg_mr(id->pd, &result, sizeof(result), IBV_ACCESS_LOCAL_WRITE);
    if (!result_mr) die_rdma("ibv_reg_mr result");
    recv_ctrl_mr = rdma_reg_msgs(id, &recv_ctrl, sizeof(recv_ctrl));
    if (!recv_ctrl_mr) die_rdma("rdma_reg_msgs recv_ctrl");
    send_ctrl_mr = rdma_reg_msgs(id, &send_ctrl, sizeof(send_ctrl));
    if (!send_ctrl_mr) die_rdma("rdma_reg_msgs send_ctrl");

    check_zero(rdma_post_recv(id, NULL, &recv_ctrl, sizeof(recv_ctrl), recv_ctrl_mr), "post_recv");
    check_zero(rdma_connect(id, NULL), "rdma_connect");
    printf("[client] connected to %s:%s\n", server_addr, server_port);

    wait_recv_comp(id, "recv mr-info");
    printf("[client] server counter MR: addr=0x%" PRIx64 ", rkey=%" PRIu32 "\n",
           recv_ctrl.addr, recv_ctrl.rkey);

    /* ① FETCH_AND_ADD(+5)：返回旧值，远端变为 旧值+5 */
    uint64_t old1 = do_atomic(id, result_mr, &result, IBV_WR_ATOMIC_FETCH_AND_ADD,
                              recv_ctrl.addr, recv_ctrl.rkey, 5, 0, "fetch_add");
    printf("[client] FETCH_ADD(+5): old=%" PRIu64 " -> remote now %" PRIu64 "\n", old1, old1 + 5);

    /* ② CMP_SWAP：若远端 == (old1+5) 则置为 999 */
    uint64_t expect = old1 + 5;
    uint64_t old2 = do_atomic(id, result_mr, &result, IBV_WR_ATOMIC_CMP_AND_SWP,
                              recv_ctrl.addr, recv_ctrl.rkey, expect, 999, "cmp_swap");
    printf("[client] CMP_SWAP(expect=%" PRIu64 ",swap=999): old=%" PRIu64 " -> %s\n",
           expect, old2, (old2 == expect) ? "交换成功，远端=999" : "比较失败，未交换");

    /* 通知服务端收尾 */
    memset(&send_ctrl, 0, sizeof(send_ctrl));
    snprintf(send_ctrl.note, sizeof(send_ctrl.note), "atomic ops completed");
    check_zero(rdma_post_send(id, NULL, &send_ctrl, sizeof(send_ctrl), send_ctrl_mr, IBV_SEND_SIGNALED),
               "post_send ack");
    wait_send_comp(id, "send ack");

    check_zero(rdma_disconnect(id), "rdma_disconnect");
    check_zero(ibv_dereg_mr(result_mr), "dereg result");
    check_zero(rdma_dereg_mr(recv_ctrl_mr), "dereg recv_ctrl");
    check_zero(rdma_dereg_mr(send_ctrl_mr), "dereg send_ctrl");
    rdma_destroy_ep(id);
    rdma_freeaddrinfo(res);
    return 0;
}
