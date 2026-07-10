# System Test Design: Relay stream wrapper terminal 生命周期

**系统基线:** `docs/system-test-design-tcpquic-proxy_cn.md`。本文只补充 stream lifetime
改造特有的 handoff、terminal、operation ownership 和 stop/recovery门禁；冲突时采用更严格值。

## 1. 范围与目标

本测试设计验证 relay-capable stream 从创建、callback target handoff、数据转发、shutdown、
worker/runtime stop 到 wrapper 最终析构的完整生产链路。覆盖 Linux epoll、Windows IOCP、
Darwin kqueue，以及 server incoming dispatcher 到 tunnel/speed-control target 的公共路径。

核心目标：

- `CleanUpManual` wrapper 只有一个 `TqStreamLifetime` owner，析构一次且不泄漏。
- wrapper callback/context 启动前设置一次，后续只切换 stable router target。
- target handoff、terminal publish、send completion、pending receive 和 worker stop 并发安全。
- 已启动 stream 不在 terminal 前 `StreamClose`。
- 修复不造成不可接受的吞吐、CPU、内存或 tunnel 建立延迟回归。

非目标：

- 不验证 MsQuic 协议栈自身的 QUIC 正确性。
- 不保证进程崩溃后恢复已有内存中 tunnel；重启后只恢复新连接服务。
- 独立 speed-test data stream 只有在复用公共 owner时才纳入本轮生命周期迁移。

## 2. 功能与非功能目标

| 类别 | 暂定门禁 |
|---|---|
| 内存安全 | ASan/Application Verifier 下 0 UAF、0 double free、0 invalid handle call |
| 生命周期 | 测试结束后 active owner、terminal retention、send registry、precommit queue 全部归零 |
| terminal 延迟 | 正常 shutdown 后 retained owner p99 在 5 秒内回收；最迟 30 秒归零 |
| graceful stop | 停止新入口后 60 秒内完成 stop；不得通过 pre-terminal `StreamClose` 达成 |
| 正确性 | 成功 workload tunnel 成功率 >= 99.9%，payload 校验 100% |
| 错误隔离 | fault case 只失败对应 tunnel；进程非预期退出为 0 |
| 性能 | 相同构建/环境下吞吐回归 <= 3%，worker CPU 增幅 <= 5% |
| 建立延迟 | k6 CONNECT/HTTPS request p95、p99 相对改造前基线回归 <= 5% |
| 内存 | 每 active stream增量内存先记录基线；发布门禁暂定 <= 基线 + 1 KiB |
| 容量 | 100、1,024、4,096 active tunnels按主机能力逐级通过，无 owner/registry 无界增长 |
| 可观测性 | terminal owner count、oldest age、route handoff failure、registry unknown key可查询 |
| RPO | 无持久业务数据，RPO=0；允许故障时在途 tunnel丢失 |
| RTO | 单进程重启后 60 秒内恢复新 tunnel；peer reconnect 后 30 秒内入口可用 |

数值是发布前暂定门禁。若现有生产 SLO 更严格，以生产 SLO 为准，并在测试报告记录替换值。

## 3. 系统级端到端功能图

```text
k6/curl/iperf/SOCKS client
  -> client ingress (HTTP CONNECT/SOCKS5 auth/port-forward validation)
  -> TqTunnelContext + TqStreamLifetime + stable router
  -> MsQuic TLS/peer-auth connection/stream
  -> server incoming router/dispatcher
  -> protocol validation + target ACL/DNS
  -> tunnel target 或 speed-control target
  -> platform relay prepare
  -> router PublishTarget
  -> relay commit + TCP target
  -> bidirectional payload
  -> shutdown/connection failure/worker stop
  -> owner TerminalPublished
  -> logical detach + operation cleanup
  -> wrapper Closed + owner/registry count zero
```

| 链路 | 关键断言 | 证据 |
|---|---|---|
| ingress -> outgoing factory | wrapper manual、owner `Starting/Started`、router固定 | trace、owner counters |
| ingress/auth/validation | SOCKS认证、CONNECT target解析和非法请求行为不变 | response code、ingress logs |
| QUIC peer boundary | 正确证书成功、错误peer身份拒绝且不创建stream owner | TLS logs、owner counters |
| server ACL/DNS | allow target成功，deny/解析失败不进入relay prepare | OPEN error、ACL trace |
| START -> callback | START返回前 callback仍可安全发布 terminal | fault hook、ASan |
| server accepted -> dispatcher | handler安装前 owner/router完整，accepted owner retained | trace、factory counters |
| dispatcher -> tunnel | route generation单调，旧 target callback完成后退役 | handoff trace |
| dispatcher -> speed control | 不写 wrapper handler，terminal仍命中同一 owner | speed-test assertions |
| tunnel -> prepared relay | fd未 arm、public handle未发布、precommit queue有界 | worker snapshot |
| PublishTarget -> CommitRelay | publish后 callback只到 prepared target；commit后才激活 | route/worker trace |
| TCP <-> QUIC | payload一致，send completion按 registry typed record claim | payload hash、registry metrics |
| shutdown request | 并发请求合并，API失败不提前释放 retention | injected status、counter |
| terminal callback | owner terminal与target take线性化，不等待 worker | callback duration、trace |
| late TCP event | 不取得新 lease，不调用 terminal stream API | fake API count、error metric |
| worker/runtime stop | route切 shutdown sink，worker销毁后 terminal仍安全 | stop trace、owner age |
| final retire | wrapper析构一次，所有 owner/queue/registry归零 | ASan、admin metrics |

## 4. 测试策略与覆盖矩阵

| ID | 场景 | Linux | Windows | Darwin | 层级 |
|---|---|---:|---:|---:|---|
| F01 | HTTP CONNECT 正常短 tunnel | Y | Y | Y | E2E |
| F02 | SOCKS5 和 port-forward payload | Y | Y | Y | E2E |
| F03 | dispatcher -> tunnel handoff | Y | Y | Y | integration |
| F04 | dispatcher -> speed-control handoff | Y | Y | Y | integration |
| F05 | START返回前 terminal | Y | Y | Y | deterministic unit |
| F06 | terminal与 PublishTarget竞争 | Y | Y | Y | concurrency |
| F07 | terminal位于 publish/commit之间 | Y | Y | Y | concurrency |
| F08 | publish后 activation failure | Y | Y | Y | fault injection |
| F09 | tunnel SEND_COMPLETE晚于 target切换 | Y | Y | Y | integration |
| F10 | unknown/duplicate send registry key | Y | Y | Y | negative |
| F11 | pending RECEIVE + terminal discard | Y | Y | Y | integration |
| F12 | active TCP reset与 terminal 后 TCP reset | Y | Y | Y | regression |
| F13 | shutdown API失败/重复请求/terminal wins | Y | Y | Y | state machine |
| F14 | worker stop后 terminal callback | Y | Y | Y | resilience |
| F15 | connection shutdown全部 stream terminal | Y | Y | Y | E2E |
| F16 | stale/duplicate terminal worker event | Y | Y | Y | idempotency |
| F17 | 30 分钟 churn/soak | Y | Y | Y | system |
| F18 | ASan/page heap 生命周期 | ASan | App Verifier/ASan | ASan | memory safety |

确定性并发测试使用 barrier/latch，不使用依赖 sleep概率的竞态测试。每个 injected point 都要
保存 owner phase、route generation、binding state、operation registry和析构计数。

## 5. k6 性能基线

拓扑：

```text
k6 -> HTTP_PROXY=http://127.0.0.1:18080
   -> tcpquic-proxy client
   -> QUIC
   -> tcpquic-proxy server
   -> HTTPS origin
```

使用 HTTPS origin确保 k6 经 HTTP CONNECT。脚本设置 `noConnectionReuse: true`，使每次迭代
建立新的代理 tunnel；另设 keep-alive scenario验证长连接。脚本和 summary JSON应提交到
`docs/test/k6/` 或测试产物目录。

| Scenario | 模型 | 时长/负载 | 目的 |
|---|---|---|---|
| baseline-short | constant-arrival-rate | 20 req/s，5m | 改造前后基线 |
| peak-short | ramping-arrival-rate | 20 -> 100 -> 300 req/s，各5m | 预期峰值与扩容趋势 |
| spike | constant-arrival-rate | 500 req/s，60s | handoff/registry突发 |
| steady-active | constant-vus keep-alive | 100/1,024 VU，10m | active owner容量 |
| churn-soak | constant-arrival-rate | 100 req/s，30m | owner/retention泄漏 |
| breaking-point | 分级加压 | 每级+250 req/s，直到错误率>1% | 饱和点 |

请求 mix：80% 1 KiB response、15% 64 KiB、5% 1 MiB；50% compress off、50% zstd分轮执行。
QUIC connection pool分别为 1、4、16。origin、证书、proxy配置和二进制 SHA256必须固定。

k6 thresholds：

```javascript
thresholds: {
  http_req_failed: ['rate<0.001'],
  checks: ['rate>0.999'],
  http_req_duration: ['p(95)<500', 'p(99)<1000'],
  dropped_iterations: ['count==0'],
}
```

最终门禁同时比较改造前同机 baseline：p95/p99 回归 <= 5%，吞吐回归 <= 3%。k6之外用
`scripts/bench-tcpquic-multi-curl.sh` 或 iperf3 HTTP proxy模式测大流量吞吐。

## 6. 容量与伸缩验证

| 维度 | 级别 | 断言 |
|---|---|---|
| active tunnels | 100、1,024、4,096 | owner/target/relay数量一致，内存线性有界 |
| handoff rate | 100、500、1,000/s | precommit queue无溢出，publish failure可解释 |
| QUIC connections | 1、4、16、128 | route generation不串 connection/stream |
| pending sends | 每 stream 1、8、32 | registry claim一次，terminal后清零 |
| pending receives | 小/大 payload + backpressure | budget有界，terminal discard一次 |
| worker count | 1、CPU/2、CPU数 | 无 owner跨 worker误投递 |

breaking point必须表现为受控拒绝、queue-full或 tunnel失败，不能是进程崩溃、无界 owner age
或 silent payload corruption。

## 7. 异常条件与灾难恢复

| 场景 | 注入故障 | 用户影响 | 检测 | 缓解/恢复 | 验收 |
|---|---|---|---|---|---|
| START失败 | fake MsQuic status | 单 tunnel失败 | start failure counter | 未启动 owner直接回收 | 无 terminal等待、析构一次 |
| accepted owner分配失败 | allocator fault | 单 incoming stream拒绝 | emergency cleanup counter | 静态callback abort，terminal close | 不提前close、handle一次释放 |
| shutdown失败 | fake status | tunnel关闭延迟 | shutdown failure/owner age | retry budget或等连接 terminal | 不提前 close，进程存活 |
| target publish失败 | router fault hook | open失败 | handoff failure | rollback prepared relay，旧 target处理 | fd/owner无泄漏 |
| commit失败 | worker fault hook | open失败 | activation failure | request shutdown，清 precommit queue | 不恢复旧 target、不丢 ownership |
| event queue/IOCP post失败 | queue full/invalid handle | 对应 tunnel关闭 | post failure metric | shutdown sink/local cleanup | worker pointer无UAF |
| TCP reset | RST/ECONNRESET | 单 tunnel失败 | platform TCP error | active abort或terminal local cleanup | stream API调用符合 phase |
| QUIC connection reset | kill peer/drop UDP | 在途 tunnel丢失 | connection/terminal metrics | reconnect，新 tunnel恢复 | 30s内入口恢复 |
| worker thread stop | test hook/runtime stop | active tunnels关闭 | stop trace | target切 shutdown sink | 60s stop，owner最终归零 |
| client/server SIGKILL | kill -9 | 当前 tunnel丢失 | process monitor | service restart | 60s内新 tunnel成功，RPO=0 |
| CPU饱和 | stress-ng | 延迟升高 | CPU、callback duration | admission/backpressure | 无 deadlock，恢复后归零 |
| 内存压力 | 限制RSS/分配失败 | 部分新 tunnel拒绝 | alloc failure | rollback/abort | 无 double free/无界 retention |
| 网络分区 | drop QUIC UDP 60s | tunnel超时 | reconnect/owner age | QUIC timeout/reconnect | 分区解除后新流量恢复 |
| DNS/target依赖失败 | NXDOMAIN、timeout、RST | 对应open失败 | dial/OPEN error | 有界timeout、无relay commit | owner/route最终归零 |
| ACL拒绝/热更新 | deny target、更新规则 | 请求被明确拒绝 | ACL audit/Admin | 不创建active relay | 0目标TCP连接、无owner泄漏 |
| 滚动升级 | 依次重启 client/server | 短暂失败 | health/connection metrics | drain + restart | 无旧版本 owner格式依赖 |

每次故障恢复后执行：Admin health、10个新 HTTP CONNECT、owner/registry/queue归零检查，以及
一次 payload hash校验。stuck owner超过30秒触发诊断告警和 dump，不通过强制 close掩盖。

## 8. 可观测性与测试证据

至少提供以下 metrics/trace：

- managed stream owners by phase；
- terminal retention count和 oldest age；
- router publish/terminal/rollback/commit及 generation mismatch；
- prepared relay和 precommit receive bytes/depth；
- send registry active/claim/unknown/duplicate；
- shutdown requested/failure/coalesced；
- terminal event post failure和 shutdown sink completion；
- wrapper created/closed和 invariant violation。

每次系统测试保存配置、平台信息、git SHA、binary SHA256、原始日志、Admin JSON、trace、k6
summary、CPU/RSS、ASan/Application Verifier输出和 fault timeline。

## 9. 进入、退出与发布门禁

进入条件：

- common owner/router/state-machine unit tests通过；
- 三端编译通过，平台 worker focused tests通过；
- 所有新增 fault hooks默认关闭且仅测试构建可控。

退出条件：

- F01-F18按平台矩阵通过；
- 0 sanitizer/verifier错误，0 crash，0 payload corruption；
- 每个 case结束 owner/retention/registry/precommit queue归零；
- k6、吞吐和CPU满足第2/5节门禁；
- abnormal scenarios恢复后新 tunnel成功；
- 文档、metrics字段和运行手册同步。

阻断发布：任何 pre-terminal `StreamClose`、unknown send key被解引用、terminal owner永久增长、
worker退出后 callback命中失效 pointer、或性能回归超过门禁且无批准例外。

## 10. 风险、假设与开放问题

- 暂定 owner内存增量 1 KiB需以实现后的 sizeof/heap profile确认。
- Windows ASan可用性依赖工具链；不可用时必须用 Application Verifier page heap补足。
- k6短请求不能替代 raw TCP长流，必须保留 iperf/curl benchmark。
- terminal retention依赖 MsQuic 对 started stream交付 terminal callback；需要实测 connection
  shutdown、进程 drain和所有 start failure分支。
- stable router target adapter是否将 `TqTunnelContext`改为 shared owner，还是保留 deferred
  self-delete adapter，需要在实现前定案并用并发删除测试锁定。
- send registry首版使用 mutex还是 intrusive concurrent table取决于基准；正确性门禁不变。
- precommit queue上限应复用 relay receive budget还是设独立小上限，需要通过 burst测试定值。
