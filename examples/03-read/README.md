# 示例 03 · RDMA READ（单边读）

与示例 01 的 WRITE 对称：这次由**客户端发起 READ**，把服务端内存里的数据拉回本地。

要点：
- 服务端把内存注册为 `IBV_ACCESS_REMOTE_READ`，并通过 SEND 告知 addr/rkey/size。
- 客户端本地目标 buffer 需 `IBV_ACCESS_LOCAL_WRITE`（网卡要往本地写入读回的数据）。
- **READ 完成在发起方的发送队列产生 CQE**，用 `wait_send_comp` 获取；服务端对
  实际数据搬运无感知。
- 客户端读完后 SEND 一个 ACK，服务端据此收尾。

![RDMA READ 时序](../../docs/img/ex03-read.svg)

## 构建与运行

```bash
make
./bin/server <RDMA网卡IP> 7471   # 终端1
./bin/client <RDMA网卡IP> 7471   # 终端2
```

预期：客户端打印从远端读回的字符串；服务端打印客户端的完成 ACK。

## WRITE vs READ 对比

| | 数据流向 | 谁发起 | 对端是否产生 CQE |
|---|---|---|---|
| WRITE（示例 01） | 本地 → 对端 | 写入方 | 否 |
| READ（示例 03） | 对端 → 本地 | 读取方 | 否 |

## 关联章节

`CLAUDE.md` 第 6（WRITE/READ）、7（post/poll）节；`TODO.md` 阶段二 2.3。
