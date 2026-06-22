/*
 * 示例 07 · SRQ（共享接收队列）— 客户端
 *
 * 客户端很简单：连接后发一条带自己标识的 SEND，由服务端的共享 SRQ 接收。
 * 客户端侧不涉及 SRQ —— SRQ 是接收方降低海量连接 RQ 内存的手段。
 * 用法：起两个客户端（标识不同），观察服务端从同一 SRQ 收到两者的消息。
 */
#include "rdma_common.h"

#define MSG_SIZE 64

int main(int argc, char **argv)
{
    const char *server_addr = (argc >= 2) ? argv[1] : "127.0.0.1";
    const char *server_port = (argc >= 3) ? argv[2] : "7471";
    const char *tag = (argc >= 4) ? argv[3] : "client-A";

    struct rdma_addrinfo hints, *res = NULL;
    struct rdma_cm_id *id = NULL;
    struct ibv_qp_init_attr qp_attr;
    char msg[MSG_SIZE];
    struct ibv_mr *msg_mr;

    memset(&hints, 0, sizeof(hints));
    hints.ai_port_space = RDMA_PS_TCP;
    check_zero(rdma_getaddrinfo((char *)server_addr, (char *)server_port, &hints, &res),
               "rdma_getaddrinfo");

    fill_qp_attr(&qp_attr);
    check_zero(rdma_create_ep(&id, res, NULL, &qp_attr), "rdma_create_ep");

    snprintf(msg, sizeof(msg), "hello from %s (pid=%d)", tag, getpid());
    msg_mr = rdma_reg_msgs(id, msg, sizeof(msg));
    if (!msg_mr) die_rdma("rdma_reg_msgs");

    check_zero(rdma_connect(id, NULL), "rdma_connect");
    printf("[%s] connected, 发送一条 SEND 给服务端共享 SRQ\n", tag);

    check_zero(rdma_post_send(id, NULL, msg, sizeof(msg), msg_mr, IBV_SEND_SIGNALED),
               "post_send");
    wait_send_comp(id, "send");
    printf("[%s] 发送完成\n", tag);

    /* 稍等片刻再断开，确保服务端已收完（教学示例的简化处理） */
    sleep(1);
    check_zero(rdma_disconnect(id), "rdma_disconnect");
    check_zero(rdma_dereg_mr(msg_mr), "dereg");
    rdma_destroy_ep(id);
    rdma_freeaddrinfo(res);
    return 0;
}
