/*
 * 示例 08 · 极简 RPC — 共享协议定义
 *
 * 一个最小的请求/响应协议：客户端发 {op, a, b}，服务端算出 result 回传。
 * 真实系统常用「单边 WRITE + WRITE_WITH_IMM 环形缓冲」消除 ACK 往返
 * （见 docs/stage7-integration.md 7.1）；本示例用 SEND/RECV 把 RPC 骨架讲清楚。
 */
#ifndef RPC_DEMO_H
#define RPC_DEMO_H

#include <stdint.h>

enum rpc_op {
    RPC_OP_ADD = 0,
    RPC_OP_MUL = 1,
};

struct rpc_request {
    uint32_t op;     /* enum rpc_op */
    uint32_t seq;    /* 序号，便于核对请求/响应配对 */
    int64_t a;
    int64_t b;
};

struct rpc_response {
    uint32_t seq;
    int32_t status;  /* 0 = OK，非 0 = 错误 */
    int64_t result;
};

#endif /* RPC_DEMO_H */
