# libuv Relay Backend Linux 功能与故障验证

> 文档状态：验证规程与 Task 13 libuv fresh 结果已登记
> 平台范围：仅 Linux
> macOS/Windows：未在本轮验证

## 1. 目标与判定边界

本规程验证 libuv relay backend 在 Linux 上的构建身份、allocator 身份、双向传输、压缩、
半关闭、故障收敛、队列压力和清理契约。正式结论由
`scripts/run-linux-libuv-relay-functional.sh` 的 production mode 生成；dry-run、fixture 或
case-driver 模式只用于测试 runner 本身，不能作为发布或设计验收证据。

功能验证不替代以下门禁：

- Task 13 的 libuv fresh configure、build 和完整测试；
- `scripts/check-libuv-backend-api.py` 的 D-27 静态门禁；
- D-24 的 native/libuv DGX 性能对照；
- macOS 和 Windows 的后续独立平台验证。

当前仓库没有本轮新生成的 `docs/test/libuv-relay-linux-functional-<timestamp>/` 结果目录，因此
不虚构 formal runner 的目录级 PASS。此前 Linux 功能与 DGX 验证结果已在会话中展示并由负责人
接受；2026-07-16 最新范围决定是只完成 libuv 相关工作，不再重跑 native，也不把 native
审计作为本次门禁。

## 2. 前置条件

1. 使用 Task 13 fresh libuv Release 构建，compiled backend 为 `libuv`；正常构建启用
   `TCPQUIC_USE_MIMALLOC=ON`。
2. 被测 `raypx2` 旁必须存在构建时生成的 `raypx2.build-manifest.json`；manifest、binary hash、
   当前 commit、递归 submodule 和 production source hash 必须一致。
3. 正式取证前工作树和递归 submodule 必须干净。文档变更应先由集成负责人按流程提交，再运行
   production formal gate；不得通过 fixture 绕过 provenance 检查。
4. Linux 主机具备 `openssl`、`curl`、Python 3 和可用的 loopback TCP/UDP 端口。
5. 不复用其他 `raypx2` 进程或 admin snapshot。runner 生成独立 token、动态端口、进程对和
   process-instance 证据，并只清理自身进程。

## 3. Fresh 执行命令

建议把结果直接写入带时间戳、不可覆盖的文档目录：

```bash
RESULT_DIR="docs/test/libuv-relay-linux-functional-$(date +%Y%m%d-%H%M%S)"
rtk bash scripts/run-linux-libuv-relay-functional.sh \
  --build-dir build/libuv-final \
  --output-dir "$RESULT_DIR"
```

runner 退出 0 仍需人工核对 `report.json`；正式接受必须同时满足：

```text
execution_mode == "production"
formal_gate_status == "passed"
valid == true
ready == {"client": 1, "server": 1}
failed == 0
duplicate_settlement == 0
provenance.superproject.dirty == false
provenance.build_manifest.compiled_relay_backend == "libuv"
provenance.build_manifest.binary_hash_match == true
provenance.build_manifest.compiled_source_match == true
cleanup.completed == true
```

任何字段缺失、case 非零、进程身份不匹配、forced cleanup、binary/source hash 不匹配或
allocator/admin contract 不满足，都必须判定为失败并保留目录。

## 4. 固定 case 矩阵

| case | 层次 | 主要能力 | 关联决策 | 必要证据 |
|---|---|---|---|---|
| `http_download_off` | E2E | HTTP CONNECT，QUIC→TCP 无压缩 | D-12、D-16、D-23 | command/rc、payload、双端 before/live/after admin、proxy log |
| `http_upload_off` | E2E | HTTP CONNECT，TCP→QUIC 无压缩 | D-14、D-16、D-23 | 同上 |
| `socks5_download_off` | E2E | SOCKS5 入口与下载 | D-04、D-12、D-23 | 同上 |
| `port_forward_off` | E2E | port forward 双向字节一致性 | D-04、D-12、D-14、D-23 | 同上 |
| `fin_half_close` | E2E | TCP EOF、QUIC FIN、反向流和完全收敛 | D-17、D-18 | payload、`fin-convergence.jsonl`、terminal counters |
| `tcp_refused` | E2E fault | publish 前/建连失败证据 | D-09、D-18、D-23 | `fault.json`、HTTP 502、直接 ECONNREFUSED 证明 |
| `tcp_reset` | E2E fault | RST 后 terminal convergence | D-18、D-23 | `fault.json`、target audit、双端 terminal delta |
| `quic_abort_unit` | focused | QUIC abort 与 terminal ledger | D-18 | `tcpquic_libuv_terminal_convergence_test` command/rc/log |
| `queue_pressure_unit` | focused | queue full、wake/fallback、停止安全性 | D-06、D-07、D-16、D-18 | `tcpquic_libuv_relay_worker_queue_test` command/rc/log |
| `allocator_bootstrap_failure_unit` | focused | allocator exactly-once 和失败关闭 | D-26 | `tcpquic_libuv_allocator_test` command/rc/log |
| `http_download_zstd` | E2E | QUIC→TCP 解压与统计 | D-03、D-13、D-22 | payload、zstd 输入/输出 delta、failure delta=0 |
| `http_upload_zstd` | E2E | TCP→QUIC 压缩与统计 | D-03、D-15、D-22 | payload、zstd 输入/输出 delta、failure delta=0 |

E2E case 必须实际观察 active relay、worker event、terminal handoff 和应用字节；fault case 还须
满足各自的严格 fault evidence。zstd case 必须证明压缩/解压计数发生且 failure delta 为 0。
只看到 `curl` 成功或测试二进制退出 0，不足以替代这些证据。

## 5. 证据目录结构

正式目录的关键结构如下；runner 可以增加辅助文件，但不得删除机器可读核心证据：

```text
docs/test/libuv-relay-linux-functional-<timestamp>/
  report.json
  results.tsv
  proxy-client.log
  proxy-server.log
  client-preflight.json
  server-preflight.json
  groups/
    off-pair.json
    off-shutdown.json
    zstd-pair.json
    zstd-shutdown.json
    *-final.json
  cases/
    <case>/
      case.json
      command.txt
      exit-code.txt
      case.log
      client-before.json
      client-live.json
      client-after.json
      server-before.json
      server-live.json
      server-after.json
      deltas.json
      proxy.log
      payload.out / fault.json / target-audit.jsonl / fin-convergence.jsonl
```

每个 E2E case 的 `case_id`、`pair_id`、run id、binary path/hash、PID、`/proc` start ticks 和 admin
port 必须互相绑定。off 与 zstd 进程组都必须保存 shutdown evidence；shutdown 结果必须明确是
graceful 或 forced 二选一，且全部进程已终止。正式接受不允许 forced kill。

## 6. Admin 与 allocator 契约

client/server preflight snapshot 都必须报告：

| 字段 | 期望值 |
|---|---|
| `compiled_relay_backend` / `backend` / `relay_backend` / `linux_relay_backend` | `libuv` |
| `relay_snapshot_complete` | `true` |
| `libuv_allocator_mode` | `mimalloc` |
| `libuv_allocator_attempted` | `true` |
| `libuv_allocator_in_progress` | `false` |
| `libuv_allocator_installed` | `true` |
| `libuv_allocator_status` | `0` |

上述字段用于正常 Release 构建。ASan 等按 D-26 允许 system allocator 的构建必须另立 sanitizer
证据，不能拿来替代本规程的 mimalloc formal run。

## 7. 与全量验证和性能验证的关系

- 本规程结果写入设计追踪的 E-FUNC。
- `build/libuv-final` 的 fresh build/test 写入 E-LUV；native regression 已由负责人取消，不再
  产生 E-NAT 门禁。
- D-27 scanner 写入 E-API。
- 后续若重跑 DGX runner，仍应建立同一根目录下的 `native/` 和 `libuv/`，最后生成
  `comparison.csv`/`comparison.md`；本次会话数据已经负责人确认满足需求，不再重跑。
- D-24 不设置吞吐、CPU、RSS 或其他自动 backend 淘汰线；最终取舍由负责人读取完整数据后
  人工判断。
- D-19 的完整 runtime stop/强制 abort 是开发顺序中最后完成的能力；本规程通过进程组清理、
  shutdown evidence 和 focused stop/terminal 交叉验证它，但不能替代 Task 13 全量测试。

## 8. 本轮结果登记

| 项目 | 当前状态 | 待集成负责人填写 |
|---|---|---|
| libuv Release configure/build | 通过 | `build/libuv-final`，backend=`libuv`、mimalloc=ON，exit 0 |
| libuv 可执行测试回归 | 通过 | 53/53 个 `tcpquic_*test` 逐个运行通过；CTest 当前未注册测试 |
| libuv API 门禁 | 通过 | 生产源码扫描 exit 0；Python 契约测试 32 passed、1 skipped |
| libuv ASan 聚焦回归 | 通过 | worker queue、runtime stop、terminal convergence 3/3 通过 |
| Linux libuv functional production runner | 本轮未重跑 | 当前工作区无新的 formal 结果目录；保留本规程供后续复跑 |
| Linux DGX 性能 | 负责人已接受 | 当前性能满足需求；无自动淘汰线；本次不重跑 |
| native regression / 零改动审计 | 已取消 | 2026-07-16 负责人决定本次只闭环 libuv |
| macOS | 未在本轮验证 | 后续独立平台证据 |
| Windows | 未在本轮验证 | 后续独立平台证据 |

本轮可以表述为“libuv Release、API 门禁和 D-19/terminal ASan 聚焦验证通过”。因为没有新建
formal runner 目录，不把本轮表述为新一次 12/12 production formal run。后续派生只保留
libuv 代码的分支属于独立目标，本次不执行。
