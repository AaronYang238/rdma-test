/*
 * 示例 01 · RDMA WRITE（单边写）— 服务端
 *
 * 教学点：控制面(SEND/RECV)交换 MR 元数据 + 数据面(RDMA WRITE)把数据直接
 * 写入客户端内存。这是仓库最初的基线 demo，收编为教程第一个示例。
 * 对应 CLAUDE.md 第 5/6/8 节。
 */
#include "rdma_common.h"

#define DEMO_DATA_SIZE 1024

struct server_context {
    struct rdma_cm_id *id;
    struct control_message recv_ctrl;
    struct control_message send_ctrl;
    char local_data[DEMO_DATA_SIZE];
    struct ibv_mr *recv_ctrl_mr;
    struct ibv_mr *send_ctrl_mr;
    struct ibv_mr *data_mr;
};

static void register_memory(struct server_context *ctx)
{
    ctx->recv_ctrl_mr = rdma_reg_msgs(ctx->id, &ctx->recv_ctrl, sizeof(ctx->recv_ctrl));
    if (!ctx->recv_ctrl_mr) {
        die_rdma("rdma_reg_msgs recv_ctrl");
    }
    ctx->send_ctrl_mr = rdma_reg_msgs(ctx->id, &ctx->send_ctrl, sizeof(ctx->send_ctrl));
    if (!ctx->send_ctrl_mr) {
        die_rdma("rdma_reg_msgs send_ctrl");
    }
    /* 服务端只是本地源缓冲，不需要对端访问权限 */
    ctx->data_mr = ibv_reg_mr(ctx->id->pd, ctx->local_data, sizeof(ctx->local_data),
                              IBV_ACCESS_LOCAL_WRITE);
    if (!ctx->data_mr) {
        die_rdma("ibv_reg_mr local_data");
    }
}

static void cleanup(struct server_context *ctx)
{
    if (ctx->data_mr) check_zero(ibv_dereg_mr(ctx->data_mr), "ibv_dereg_mr data_mr");
    if (ctx->send_ctrl_mr) check_zero(rdma_dereg_mr(ctx->send_ctrl_mr), "rdma_dereg_mr send_ctrl_mr");
    if (ctx->recv_ctrl_mr) check_zero(rdma_dereg_mr(ctx->recv_ctrl_mr), "rdma_dereg_mr recv_ctrl_mr");
}

int main(int argc, char **argv)
{
    const char *bind_addr = (argc >= 2) ? argv[1] : "0.0.0.0";
    const char *bind_port = (argc >= 3) ? argv[2] : "7471";
    struct rdma_addrinfo hints, *res = NULL;
    struct rdma_cm_id *listen_id = NULL, *conn_id = NULL;
    struct ibv_qp_init_attr qp_attr;
    struct server_context ctx;

    memset(&ctx, 0, sizeof(ctx));
    snprintf(ctx.local_data, sizeof(ctx.local_data),
             "Hello from server via RDMA Write at pid=%d", getpid());

    memset(&hints, 0, sizeof(hints));
    hints.ai_port_space = RDMA_PS_TCP;
    hints.ai_flags = RAI_PASSIVE;
    check_zero(rdma_getaddrinfo((char *)bind_addr, (char *)bind_port, &hints, &res),
               "rdma_getaddrinfo server");

    fill_qp_attr(&qp_attr);
    check_zero(rdma_create_ep(&listen_id, res, NULL, &qp_attr), "rdma_create_ep listen");
    check_zero(rdma_listen(listen_id, 1), "rdma_listen");
    printf("[server] listening on %s:%s\n", bind_addr, bind_port);

    check_zero(rdma_get_request(listen_id, &conn_id), "rdma_get_request");
    ctx.id = conn_id;
    register_memory(&ctx);

    /* 预投递接收，必须在 accept 前 */
    check_zero(rdma_post_recv(conn_id, NULL, &ctx.recv_ctrl, sizeof(ctx.recv_ctrl), ctx.recv_ctrl_mr),
               "rdma_post_recv control");
    check_zero(rdma_accept(conn_id, NULL), "rdma_accept");
    printf("[server] accepted connection\n");

    /* ① 收到客户端 MR 元数据 */
    wait_recv_comp(conn_id, "rdma_get_recv_comp control");
    printf("[server] remote MR: addr=0x%" PRIx64 ", rkey=%" PRIu32 ", size=%" PRIu32 "\n",
           ctx.recv_ctrl.addr, ctx.recv_ctrl.rkey, ctx.recv_ctrl.size);

    /* ② RDMA WRITE：直接写进客户端内存 */
    check_zero(rdma_post_write(conn_id, NULL, ctx.local_data, strlen(ctx.local_data) + 1,
                               ctx.data_mr, IBV_SEND_SIGNALED,
                               ctx.recv_ctrl.addr, ctx.recv_ctrl.rkey),
               "rdma_post_write");
    wait_send_comp(conn_id, "rdma_get_send_comp write");

    /* ③ SEND 一个 ACK 通知客户端写已完成 */
    memset(&ctx.send_ctrl, 0, sizeof(ctx.send_ctrl));
    snprintf(ctx.send_ctrl.note, sizeof(ctx.send_ctrl.note), "RDMA write completed");
    check_zero(rdma_post_send(conn_id, NULL, &ctx.send_ctrl, sizeof(ctx.send_ctrl),
                              ctx.send_ctrl_mr, IBV_SEND_SIGNALED),
               "rdma_post_send ack");
    wait_send_comp(conn_id, "rdma_get_send_comp ack");
    printf("[server] write done, sent ack, disconnecting\n");

    check_zero(rdma_disconnect(conn_id), "rdma_disconnect");
    cleanup(&ctx);
    rdma_destroy_ep(conn_id);
    rdma_destroy_ep(listen_id);
    rdma_freeaddrinfo(res);
    return 0;
}
