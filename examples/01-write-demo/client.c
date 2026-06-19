/*
 * 示例 01 · RDMA WRITE（单边写）— 客户端
 *
 * 客户端开放一段可被远端写入的 MR，把 addr/rkey/size 通过 SEND 告知服务端，
 * 然后被动等待：服务端用 RDMA WRITE 覆盖该 buffer，客户端 CPU 全程无感，
 * 仅通过随后收到的 ACK 得知，并打印被覆盖后的内容。
 */
#include "rdma_common.h"

#define DEMO_DATA_SIZE 1024

struct client_context {
    struct rdma_cm_id *id;
    struct control_message send_ctrl;
    struct control_message recv_ctrl;
    char remote_writable_data[DEMO_DATA_SIZE];
    struct ibv_mr *send_ctrl_mr;
    struct ibv_mr *recv_ctrl_mr;
    struct ibv_mr *data_mr;
};

static void register_memory(struct client_context *ctx)
{
    ctx->send_ctrl_mr = rdma_reg_msgs(ctx->id, &ctx->send_ctrl, sizeof(ctx->send_ctrl));
    if (!ctx->send_ctrl_mr) {
        die_rdma("rdma_reg_msgs send_ctrl");
    }
    ctx->recv_ctrl_mr = rdma_reg_msgs(ctx->id, &ctx->recv_ctrl, sizeof(ctx->recv_ctrl));
    if (!ctx->recv_ctrl_mr) {
        die_rdma("rdma_reg_msgs recv_ctrl");
    }
    /* 开放 REMOTE_WRITE，服务端才能 RDMA Write 进来 */
    ctx->data_mr = ibv_reg_mr(ctx->id->pd, ctx->remote_writable_data,
                              sizeof(ctx->remote_writable_data),
                              IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                                  IBV_ACCESS_REMOTE_READ);
    if (!ctx->data_mr) {
        die_rdma("ibv_reg_mr remote_writable_data");
    }
}

static void cleanup(struct client_context *ctx)
{
    if (ctx->data_mr) check_zero(ibv_dereg_mr(ctx->data_mr), "ibv_dereg_mr data_mr");
    if (ctx->recv_ctrl_mr) check_zero(rdma_dereg_mr(ctx->recv_ctrl_mr), "rdma_dereg_mr recv_ctrl_mr");
    if (ctx->send_ctrl_mr) check_zero(rdma_dereg_mr(ctx->send_ctrl_mr), "rdma_dereg_mr send_ctrl_mr");
}

int main(int argc, char **argv)
{
    const char *server_addr = (argc >= 2) ? argv[1] : "127.0.0.1";
    const char *server_port = (argc >= 3) ? argv[2] : "7471";
    struct rdma_addrinfo hints, *res = NULL;
    struct rdma_cm_id *id = NULL;
    struct ibv_qp_init_attr qp_attr;
    struct client_context ctx;

    memset(&ctx, 0, sizeof(ctx));
    snprintf(ctx.remote_writable_data, sizeof(ctx.remote_writable_data),
             "This text should be overwritten by server RDMA Write");

    memset(&hints, 0, sizeof(hints));
    hints.ai_port_space = RDMA_PS_TCP;
    check_zero(rdma_getaddrinfo((char *)server_addr, (char *)server_port, &hints, &res),
               "rdma_getaddrinfo client");

    fill_qp_attr(&qp_attr);
    check_zero(rdma_create_ep(&id, res, NULL, &qp_attr), "rdma_create_ep client");
    ctx.id = id;
    register_memory(&ctx);

    /* 预投递接收 ACK，必须在 connect 前 */
    check_zero(rdma_post_recv(id, NULL, &ctx.recv_ctrl, sizeof(ctx.recv_ctrl), ctx.recv_ctrl_mr),
               "rdma_post_recv ack");
    check_zero(rdma_connect(id, NULL), "rdma_connect");
    printf("[client] connected to %s:%s\n", server_addr, server_port);

    /* ① SEND 自己的 MR 元数据给服务端 */
    memset(&ctx.send_ctrl, 0, sizeof(ctx.send_ctrl));
    ctx.send_ctrl.addr = (uint64_t)(uintptr_t)ctx.remote_writable_data;
    ctx.send_ctrl.rkey = ctx.data_mr->rkey;
    ctx.send_ctrl.size = sizeof(ctx.remote_writable_data);
    snprintf(ctx.send_ctrl.note, sizeof(ctx.send_ctrl.note), "client MR metadata");
    check_zero(rdma_post_send(id, NULL, &ctx.send_ctrl, sizeof(ctx.send_ctrl),
                              ctx.send_ctrl_mr, IBV_SEND_SIGNALED),
               "rdma_post_send mr-info");
    wait_send_comp(id, "rdma_get_send_comp mr-info");

    /* ③ 等 ACK；此时本地 buffer 已被服务端 RDMA Write 覆盖 */
    wait_recv_comp(id, "rdma_get_recv_comp ack");
    printf("[client] server note: %s\n", ctx.recv_ctrl.note);
    printf("[client] local buffer after remote write: \"%s\"\n", ctx.remote_writable_data);

    check_zero(rdma_disconnect(id), "rdma_disconnect");
    cleanup(&ctx);
    rdma_destroy_ep(id);
    rdma_freeaddrinfo(res);
    return 0;
}
