/*
 * 示例 08 · 极简 RPC — 客户端（调用方）
 *
 * 先做两次演示调用（ADD / MUL）打印结果，再压测 iters 次 RPC 测平均延迟。
 * 每次调用：post_send 请求 → 等响应 recv 完成 → 读 result。
 * 对应 docs/stage7-integration.md 7.1。
 */
#include "rdma_common.h"
#include "rpc.h"

int main(int argc, char **argv)
{
    const char *server_addr = (argc >= 2) ? argv[1] : "127.0.0.1";
    const char *server_port = (argc >= 3) ? argv[2] : "7471";
    int iters = (argc >= 4) ? atoi(argv[3]) : 10000;

    struct rdma_addrinfo hints, *res = NULL;
    struct rdma_cm_id *id = NULL;
    struct ibv_qp_init_attr qp_attr;
    struct rpc_request req;
    struct rpc_response resp;
    struct ibv_mr *req_mr, *resp_mr;
    uint32_t seq = 0;

    memset(&hints, 0, sizeof(hints));
    hints.ai_port_space = RDMA_PS_TCP;
    check_zero(rdma_getaddrinfo((char *)server_addr, (char *)server_port, &hints, &res),
               "rdma_getaddrinfo");

    fill_qp_attr(&qp_attr);
    check_zero(rdma_create_ep(&id, res, NULL, &qp_attr), "rdma_create_ep");

    req_mr = rdma_reg_msgs(id, &req, sizeof(req));
    if (!req_mr) die_rdma("rdma_reg_msgs req");
    resp_mr = rdma_reg_msgs(id, &resp, sizeof(resp));
    if (!resp_mr) die_rdma("rdma_reg_msgs resp");

    /* 预投递接收第一个响应，必须在 connect 前 */
    check_zero(rdma_post_recv(id, NULL, &resp, sizeof(resp), resp_mr), "post_recv");
    check_zero(rdma_connect(id, NULL), "rdma_connect");
    printf("[client] 已连接 %s:%s\n", server_addr, server_port);

    /* 一次同步 RPC：发请求 → 等响应（响应的 recv 必须已预投递） */
    #define DO_RPC(OP, A, B) do {                                                        \
        req.op = (OP); req.seq = seq++; req.a = (A); req.b = (B);                        \
        check_zero(rdma_post_send(id, NULL, &req, sizeof(req), req_mr, IBV_SEND_SIGNALED),\
                   "post_send request");                                                 \
        wait_send_comp(id, "send request");                                             \
        wait_recv_comp(id, "recv response");                                            \
        if (req.seq + 1u < (uint32_t)(2 + iters))                                        \
            check_zero(rdma_post_recv(id, NULL, &resp, sizeof(resp), resp_mr),           \
                       "post_recv");                                                     \
    } while (0)

    /* ① 两次演示调用 */
    DO_RPC(RPC_OP_ADD, 3, 4);
    printf("[client] ADD(3,4) = %" PRId64 " (status=%d)\n", resp.result, resp.status);
    DO_RPC(RPC_OP_MUL, 6, 7);
    printf("[client] MUL(6,7) = %" PRId64 " (status=%d)\n", resp.result, resp.status);

    /* ② 延迟压测：iters 次 ADD */
    printf("[client] 压测 %d 次同步 RPC...\n", iters);
    uint64_t t0 = now_ns();
    for (int i = 0; i < iters; i++) {
        DO_RPC(RPC_OP_ADD, i, 1);
    }
    uint64_t t1 = now_ns();
    double total_us = (t1 - t0) / 1000.0;
    printf("[client] %d 次 RPC 用时 %.1f us，平均每次 %.2f us\n",
           iters, total_us, total_us / iters);
    printf("[client] 提示：单边 WRITE_WITH_IMM 环形缓冲可进一步省去 ACK 往返"
           "（见 stage7 7.1）\n");

    check_zero(rdma_disconnect(id), "rdma_disconnect");
    check_zero(rdma_dereg_mr(req_mr), "dereg req");
    check_zero(rdma_dereg_mr(resp_mr), "dereg resp");
    rdma_destroy_ep(id);
    rdma_freeaddrinfo(res);
    return 0;
}
