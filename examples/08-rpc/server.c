/*
 * 示例 08 · 极简 RPC — 服务端（请求处理方）
 *
 * 循环：post_recv 收请求 → 计算 → post_send 回响应 → 再 post_recv。
 * 体现 RPC 的本质：双边 SEND/RECV 承载控制语义，服务端 CPU 参与每次调用。
 * 对应 docs/stage7-integration.md 7.1。
 */
#include "rdma_common.h"
#include "rpc.h"

static int64_t compute(const struct rpc_request *req, int32_t *status)
{
    *status = 0;
    switch (req->op) {
    case RPC_OP_ADD: return req->a + req->b;
    case RPC_OP_MUL: return req->a * req->b;
    default:         *status = 1; return 0;
    }
}

int main(int argc, char **argv)
{
    const char *bind_addr = (argc >= 2) ? argv[1] : "0.0.0.0";
    const char *bind_port = (argc >= 3) ? argv[2] : "7471";
    int iters = (argc >= 4) ? atoi(argv[3]) : 10000;
    int total = 2 + iters; /* 2 个演示调用 + iters 次压测，与客户端约定一致 */

    struct rdma_addrinfo hints, *res = NULL;
    struct rdma_cm_id *listen_id = NULL, *id = NULL;
    struct ibv_qp_init_attr qp_attr;
    struct rpc_request req;
    struct rpc_response resp;
    struct ibv_mr *req_mr, *resp_mr;

    memset(&hints, 0, sizeof(hints));
    hints.ai_port_space = RDMA_PS_TCP;
    hints.ai_flags = RAI_PASSIVE;
    check_zero(rdma_getaddrinfo((char *)bind_addr, (char *)bind_port, &hints, &res),
               "rdma_getaddrinfo");

    fill_qp_attr(&qp_attr);
    check_zero(rdma_create_ep(&listen_id, res, NULL, &qp_attr), "rdma_create_ep");
    check_zero(rdma_listen(listen_id, 1), "rdma_listen");
    printf("[server] RPC 服务监听 %s:%s\n", bind_addr, bind_port);

    check_zero(rdma_get_request(listen_id, &id), "rdma_get_request");

    req_mr = rdma_reg_msgs(id, &req, sizeof(req));
    if (!req_mr) die_rdma("rdma_reg_msgs req");
    resp_mr = rdma_reg_msgs(id, &resp, sizeof(resp));
    if (!resp_mr) die_rdma("rdma_reg_msgs resp");

    /* 第一个 recv 必须在 accept 前预投递 */
    check_zero(rdma_post_recv(id, NULL, &req, sizeof(req), req_mr), "post_recv");
    check_zero(rdma_accept(id, NULL), "rdma_accept");
    printf("[server] 已接受客户端，开始服务 %d 次调用\n", total);

    for (int i = 0; i < total; i++) {
        wait_recv_comp(id, "recv request");

        int32_t status;
        int64_t result = compute(&req, &status);
        if (i < 2) {
            printf("[server] RPC #%u: op=%u a=%" PRId64 " b=%" PRId64 " -> %" PRId64 "\n",
                   req.seq, req.op, req.a, req.b, result);
        }

        resp.seq = req.seq;
        resp.status = status;
        resp.result = result;

        /* 回响应前先为下一次请求补投递接收（流水化，减少 RNR 风险） */
        if (i + 1 < total) {
            check_zero(rdma_post_recv(id, NULL, &req, sizeof(req), req_mr), "post_recv");
        }
        check_zero(rdma_post_send(id, NULL, &resp, sizeof(resp), resp_mr, IBV_SEND_SIGNALED),
                   "post_send response");
        wait_send_comp(id, "send response");
    }

    printf("[server] 完成 %d 次 RPC，断开\n", total);
    check_zero(rdma_disconnect(id), "rdma_disconnect");
    check_zero(rdma_dereg_mr(req_mr), "dereg req");
    check_zero(rdma_dereg_mr(resp_mr), "dereg resp");
    rdma_destroy_ep(id);
    rdma_destroy_ep(listen_id);
    rdma_freeaddrinfo(res);
    return 0;
}
