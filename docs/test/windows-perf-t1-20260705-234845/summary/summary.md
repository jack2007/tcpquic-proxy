# Windows 压测摘要

- 场景: F02 download ×3 + F03 upload ×1 + F06 download connections=4 ×1
- 拓扑: T1（Windows client → `172.16.10.80:8443`）
- 时间: 2026-07-05 23:48–23:56（本地）
- git: `903a3079c3c1d6634c788974aabd72890636f2eb`
- binary sha256: `598E12078B30640EFBE810E2DA52E5ADB8913CFDC5B0C1D0DE6D68DAE1D69FE0`
- 结果: **FAIL**

## 环境

- binary: `build-x64\bin\Release\raypx2.exe`
- CA: `cert\ca.crt`
- peer: `172.16.10.80:8443`
- compress: `off`
- relay backend: `windows-iocp (8 workers)`
- trace: enabled，interval 30s

## F02 download，connections=1，60s ×3

| 轮次 | local_bytes | server_bytes | gbps | MiB/s | exit | 备注 |
|---:|---:|---:|---:|---:|---:|---|
| 1 | 5,982,647,681 | 6,260,830,001 | 0.798 | 95.09 | 1 | local/server mismatch: 278,182,320 > 50,331,648 |
| 2 | 5,855,520,720 | 6,021,167,203 | 0.781 | 93.07 | 1 | local/server mismatch: 165,646,483 > 50,331,648 |
| 3 | 5,845,634,527 | 6,027,333,417 | 0.779 | 92.91 | 1 | local/server mismatch: 181,698,890 > 50,331,648 |

- 平均吞吐: **0.786 Gbps / 93.69 MiB/s**
- 三轮 MiB/s 偏差: max/min = 95.09 / 92.91，约 2.3%，吞吐本身可复现
- 判定: **FAIL**，原因是三轮均触发 `download local/server byte mismatch exceeds limit`，进程退出码为 1

## F03 upload，connections=1，60s ×1

| local_bytes | server_bytes | gbps | MiB/s | exit | 备注 |
|---:|---:|---:|---:|---:|---|
| 7,034,241,024 | 7,034,241,024 | 0.935 | 111.46 | 0 | PASS |

- 判定: **PASS**
- 方向性观察: upload 不复现 byte mismatch，download 方向存在稳定异常

## F06 download，connections=4，60s ×1

| local_bytes | server_bytes | gbps | MiB/s | accepted/closed | exit | 备注 |
|---:|---:|---:|---:|---:|---:|---|
| 7,010,811,650 | 7,776,697,134 | 0.935 | 111.43 | 4/4 | 1 | local/server mismatch: 765,885,484 > 50,331,648 |

- 判定: **FAIL**
- 多连接吞吐提升到与 upload 接近，但 server 统计显著大于 local，mismatch 更大

## 结论

本轮基于当前 `raypx2.exe` 构建产物完成了 T1 L2 性能测试。链路和远端服务可用，QUIC client 均成功连接并完成 speed result；upload 方向通过，download 方向可以稳定产出吞吐但均因 `local/server byte mismatch exceeds limit` 失败。

因此本轮整体结论为 **FAIL**。吞吐数据可作为临时性能参考，但不应作为通过基线；建议继续排查 Windows client download relay 或 speed test pump 的计数/收尾逻辑。
