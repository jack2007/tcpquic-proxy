# Windows 压测摘要（T1 首轮）

- 场景: F02/F03/F06（部分）
- 拓扑: T1（Windows client → Linux `172.16.10.80:8443`）
- 证据目录: `docs/test/windows-perf-t1-20260705-183250/`
- 结果: **FAIL**（下载方向）；上传方向 **PASS**

## 环境

| 项 | 值 |
|---|---|
| Windows | Windows 10 x64（build 19045），8 逻辑 CPU，约 32 GB RAM |
| Client 二进制 | `build-x64/bin/Release/raypx2.exe` |
| Client CA | `cert/ca.crt` |
| Linux server | `172.16.10.80:8443`，用户 `jack`，`raypx2` 已运行 |
| Server 证书 | `cert/server/server.crt` + `cert/server/server.key` |
| Relay 后端 | `windows-iocp`（8 workers） |

## 门禁执行

| 层级 | 场景 | 结果 | 说明 |
|---|---|---|---|
| L0 | `tcpquic_windows_relay_worker_test` | SKIP | 本机 shell 无 `cmake`/MSBuild，无现成 test 二进制 |
| L1 | loopback smoke | SKIP | 本机无 `openssl`（`OPENSSL_BIN` 未配置） |
| L2 | T1 连通 10 s | WARN | QUIC 连通；`13.84 MiB/s`；local/server 字节不一致，exit 1 |
| L2 | F02 download 60 s × 3 | FAIL | 3 轮均 `speed test pump worker failed`；client 接收远低于 server 发送 |
| L2 | F03 upload 60 s | **PASS** | `111.46 MiB/s`（~0.935 Gbps）；local/server 字节一致，exit 0 |
| L2 | F06 download conn=4 60 s | FAIL | 4 连接建立成功；pump worker failed；local 1.22 GB vs server 7.75 GB |

## 吞吐（实测）

### 上传（F03，PASS）

| 指标 | 值 |
|---|---|
| local_bytes | 7,037,386,752 |
| server_bytes | 7,037,386,752 |
| 吞吐 | **111.46 MiB/s**（0.935 Gbps） |
| RTT（启动时） | ~3 ms |

### 下载（F02，FAIL）

| 轮次 | local_bytes（约） | server_bytes（约） | 隐含 client 吞吐 |
|---|---|---|---|
| 1 | 181 MB | 2.99 GB | ~24 Mbps |
| 2 | 39 MB | 2.49 GB | ~5 Mbps |
| 3 | 67 MB | 2.59 GB | ~9 Mbps |

10 s 连通探测：local 145 MB / 10 s ≈ **116 Mbps**（仍低于 server 计数，exit 1）。

## 结论

1. **T1 链路 QUIC/TLS 正常**：client 可连接 Linux server，上传 speed test 达标且计数一致。
2. **下载方向存在跨平台问题**：Linux server 高速发送时，Windows client 侧 download pump 无法稳定收满，出现 `pump worker failed` 与 local/server 字节严重偏差；需优先排查 Windows IOCP relay 接收路径或 speed test download pump（`src/runtime/speed_test.cpp`）。
3. **本机工具链缺口**：L0/L1 未执行——需安装 OpenSSL（或 Git for Windows）并确保 `cmake` 在 PATH。

## 后续建议

1. 在 Windows 上补跑 L0/L1（安装 OpenSSL + 配置构建工具 PATH）。
2. 对比 T0 loopback download 60 s：若 T0 通过而 T1 失败，则问题在跨平台或链路侧；若 T0 也失败，则聚焦 Windows download relay。
3. 采集压测期间 Windows `log/client.log` 与 Linux server stderr，对照 `windows_relay_callback_receive_budget_*` 指标。
4. 修复下载路径前，**不宜**进入 L3/L4 Stress/Soak。

## 原始日志

- `case/l2-t1-connect-10s.log`
- `case/speed-test-round-{1,2,3}.txt`
- `case/speed-test-upload-60s.txt`
- `case/speed-test-f06-conn4-60s.txt`
