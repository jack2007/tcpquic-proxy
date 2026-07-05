# DGX server ACL 热更新与重启恢复执行记录

日期：2026-07-05

## 1. 执行入口

测试方案：

- `docs/test/dgx-server-acl-hot-update-test-plan-20260705_cn.md`

执行命令：

```bash
rtk bash scripts/run-dgx-server-acl-hot-update.sh
```

结果目录：

```text
docs/test/dgx-server-acl-hot-update-20260705-105904/
```

## 2. 环境

| 项 | 值 |
|---|---|
| git head | `65ae683f8f5781922ecfb5c005039830f091cac0` |
| 本机二进制 | `build/bin/Release/raypx2` |
| 远端测试二进制 | `/home/jack/tcpquic-dgx-bin/raypx2-server-acl-20260705-105904` |
| 远端 server 配置 | `/home/jack/dgx-server-acl-20260705-105904-server.json` |
| server listen | `10.201.1.2:4433,10.201.2.2:4433` |
| client port forward | `127.0.0.1:15446 -> 10.201.1.2:16001` |
| server admin | 对端 `127.0.0.1:18081` |
| client admin | 本机 `127.0.0.1:18082` |

## 3. 执行结果

| 场景 | 结果 | 证据 |
|---|---:|---|
| 初始 ACL 允许目标 | 通过 | `case/initial-iperf.rc=0`，`initial_connected=1` |
| PATCH deny 目标 | 通过 | `admin/patch-deny-response.status=200`，`case/deny-iperf.rc=1` |
| `acl_denied` 增长 | 通过 | `acl_denied_before=0`，`acl_denied_after=1` |
| PATCH 恢复允许 | 通过 | `admin/patch-allow-response.status=200`，`case/recover-iperf.rc=0` |
| 非法 CIDR 回滚 | 通过 | `admin/patch-invalid-response.status=400`，配置文件仍为允许版本 |
| server 重启恢复 | 通过 | `post_restart_connected=1`，`case/post-restart-iperf.rc=0` |
| 最终持久化配置 | 通过 | `final_allow_targets=["10.201.1.2/32"]`，`final_deny_targets=[]` |

摘要文件：

- `docs/test/dgx-server-acl-hot-update-20260705-105904/summary/summary.md`

## 4. 结论

server ACL admin hot update 在 2 台 DGX 环境通过系统级验证：

- 成功 PATCH `deny_targets` 后，新建 tunnel 被拒绝，`acl_denied` 计数增加。
- 成功 PATCH 恢复允许后，新建 tunnel 恢复可用。
- 非法 CIDR 返回 400，未污染运行中 ACL 和远端 JSON 配置文件。
- server 使用同一 JSON 配置重启后，ACL 从写回配置恢复，新建 tunnel 仍可用。

## 5. 清理状态

脚本退出时已清理：

- 本机本轮启动的 `raypx2 client`。
- 对端本轮启动的 `raypx2 server`。
- 对端本轮启动的 `iperf3`。

脚本保留：

- 远端测试二进制 `/home/jack/tcpquic-dgx-bin/raypx2-server-acl-20260705-105904`。
- 远端测试配置 `/home/jack/dgx-server-acl-20260705-105904-server.json`。
- 本机完整证据目录 `docs/test/dgx-server-acl-hot-update-20260705-105904/`。
