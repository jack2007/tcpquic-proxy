# 内置 Admin Console 设计

日期：2026-06-29

## 背景

当前项目已经提供 Admin API，公开路径前缀为 `/api/v1`，由 `--admin-listen` 或配置 `admin.listen` 开启。API 使用 Bearer token 鉴权，token 写入 `--admin-token-file` 指定的 JSON 文件；未指定时使用运行时默认路径。

本设计在现有 Admin HTTP server 上内置一个浏览器管理页面，用于通过 Admin API 管理 client/router/server 的运行状态、peer、connection、tunnel、relay、诊断和配置。Console 不新增独立进程、不新增第二个监听端口，也不引入新的账号体系。

## 目标

- 只要启用 `--admin-listen`，同一个 Admin HTTP server 同时提供 Admin API 和 Console 页面。
- 通过 `--admin-listen` 的绑定地址决定访问范围，例如 `127.0.0.1:18080`、`172.16.10.80:18080`、`0.0.0.0:18080`。
- Console 只支持内置账号 `raypx2`；密码使用 admin token 文件中的 `token`。
- Console 所有管理操作都调用现有 `/api/v1/*` Admin API。
- 首版提供资源管理型页面：常用字段表单 + JSON 高级编辑。
- 保持 API 鉴权语义：登录页和静态资源可未认证访问，API 仍强制 Bearer token。

## 非目标

- 不新增 `--admin-console` 开关。
- 不新增服务端 session、用户表、密码文件、RBAC 或多用户权限模型。
- 不新增 CORS 支持；Console 与 API 同源访问。
- 不绕过或复制 Admin API 的业务校验。
- 不在首版实现尚未存在的 Admin API 能力，例如 server 配置热更新、诊断开关热更新、Linux/macOS relay worker 明细。

## 启动和暴露范围

`--admin-listen` 是唯一开关：

| 示例 | 暴露范围 |
|---|---|
| `--admin-listen 127.0.0.1:18080` | 仅本机访问 |
| `--admin-listen 172.16.10.80:18080` | 仅绑定到该管理网 IP |
| `--admin-listen 0.0.0.0:18080` | 所有 IPv4 网卡 |

当前 `TqValidateAdminListen()` 的 loopback-only 限制需要调整为允许显式绑定地址。启动日志和文档必须提示：非 loopback 或 `0.0.0.0` 会把 Admin API 与 Console 暴露到对应网络，必须保护 token 文件并限制网络访问。

推荐本机访问仍使用 `127.0.0.1`。`127.0.0.0` 是 loopback 网段地址，不作为文档中的推荐监听地址。

## HTTP 路由设计

Admin HTTP server 统一提供：

| 路径 | 行为 | 鉴权 |
|---|---|---|
| `/` | 重定向到 `/console/` | 不要求 |
| `/console/` | Console HTML 入口 | 不要求 |
| `/console/app.js` | Console 前端逻辑 | 不要求 |
| `/console/style.css` | Console 样式 | 不要求 |
| `/api/v1/*` | 现有 Admin API | Bearer token |

Console 静态资源可以内嵌到 C++ 源码中，也可以使用构建期生成的嵌入资源。首版建议内嵌单页 HTML/CSS/JS，减少安装部署时的文件路径依赖。

## 认证设计

登录页字段：

- 用户名：必须为 `raypx2`。
- 密码：admin token 文件 JSON 中的 `token`。

登录流程：

1. 用户打开 `/console/`。
2. 输入 `raypx2` 和 token。
3. 前端调用 `GET /api/v1/admin`，请求头带 `Authorization: Bearer <token>`。
4. 返回 `200` 表示登录成功；前端将 token 存入 `sessionStorage`。
5. 后续 API 请求统一带 `Authorization: Bearer <token>`。
6. 退出登录时清理 `sessionStorage` 并回到登录页。

不使用 `localStorage`，避免 token 长期持久化。不新增 cookie 或服务端 session，避免引入 CSRF 状态面。

## 页面信息架构

首版采用资源管理型 Console：

- 左侧导航：`Overview`、`Peers`、`Connections`、`Tunnels`、`Relay`、`Config`、`Diagnostics`。
- 顶部状态条：role、listen、health、刷新状态、最后成功刷新时间、退出登录。
- 主区：资源表格、详情抽屉、表单、JSON 编辑器。
- 危险动作：删除、drain、abort、allocator dump 等必须弹出确认。

### Overview

调用接口：

- `GET /api/v1/admin`
- `GET /api/v1/health`
- `GET /api/v1/metrics`

展示内容：

- role、status、uptime、listen、auth 状态、admin threads、max body bytes。
- client/router 模式展示 peer 数、connection 数、active streams、total streams、last_error。
- server 模式展示 listen、resolved listens、accepted connections、active streams、acl_denied、tcp_dialing、last_error。
- 快捷入口：Peers、Tunnels、Config、Allocator dump。

### Peers

调用接口：

- `GET /api/v1/peers`
- `POST /api/v1/peers`
- `GET /api/v1/peers/{peer_id}`
- `GET /api/v1/peers/{peer_id}/config`
- `PUT /api/v1/peers/{peer_id}`
- `PATCH /api/v1/peers/{peer_id}`
- `DELETE /api/v1/peers/{peer_id}`
- `POST /api/v1/peers/{peer_id}:enable`
- `POST /api/v1/peers/{peer_id}:disable`
- `POST /api/v1/peers/{peer_id}:drain`
- `POST /api/v1/peers/{peer_id}:abort-tunnels`

表格列：

- `peer_id`
- `state`
- `enabled`
- `quic_peer` 或 paths 摘要
- `socks_listen`
- `http_listen`
- `connection_count`
- `connected_connections`
- `active_streams`
- `last_error`

编辑能力：

- 常用字段表单：`peer_id`、`quic_peer`、`socks_listen`、`http_listen`、`port_forwards`、`paths`、`quic_connections`、`compress`、`enabled`。
- 高级 JSON 编辑：用于完整替换 peer 或复制当前配置。
- 删除 active peer 默认使用 `reject-if-active`；用户可显式选择 `drain` 或 `abort`。

### Connections

调用接口：

- `GET /api/v1/peers/{peer_id}/connections`
- `POST /api/v1/peers/{peer_id}/connections`
- `GET /api/v1/peers/{peer_id}/connections/{connection_id}`
- `DELETE /api/v1/peers/{peer_id}/connections/{connection_id}`
- `POST /api/v1/peers/{peer_id}/connections/{connection_id}:reconnect`
- `POST /api/v1/peers/{peer_id}/connections/{connection_id}:abort-tunnels`

页面行为：

- 先选择 peer，再展示 connection 列表。
- 删除前提示：只能删除最高 slot，且不能删除最后一个 connection。
- reconnect 和 abort tunnels 必须确认。

### Tunnels

client/router 模式调用：

- `GET /api/v1/tunnels`
- `GET /api/v1/tunnels/{tunnel_id}`
- `DELETE /api/v1/tunnels/{tunnel_id}`
- `POST /api/v1/tunnels/{tunnel_id}:abort`
- `POST /api/v1/tunnels/{tunnel_id}:drain`

server 模式调用：

- `GET /api/v1/server/tunnels`
- `GET /api/v1/server/tunnels/{tunnel_id}`
- `DELETE /api/v1/server/tunnels/{tunnel_id}`
- `POST /api/v1/server/tunnels/{tunnel_id}:abort`
- `POST /api/v1/server/tunnels/{tunnel_id}:drain`

表格列：

- `tunnel_id`
- `peer_id`
- `connection_id`
- `state`
- `target`
- `role`
- `duration_ms`
- bytes/pending bytes
- `last_error`

### Relay

调用接口：

- `GET /api/v1/relay/metrics`
- `GET /api/v1/relay/active-relays`
- `GET /api/v1/relay/workers`
- `GET /api/v1/relay/workers/{worker_id}`

展示内容：

- backend、active_relays、pending_bytes、tcp_read_bytes、tcp_write_bytes、errors。
- capability：active relay detail、worker detail。
- worker 列表和 aggregate worker。

`503 not_supported` 不作为全局错误弹窗，而是在对应面板显示“当前平台不支持该明细”。

### Config

只读接口：

- `GET /api/v1/runtime/config`
- `GET /api/v1/client/config`
- `GET /api/v1/server/config`

可写接口：

- `GET /api/v1/config`
- `PUT /api/v1/config`

页面行为：

- runtime/client/server 配置以只读 JSON viewer 展示。
- router config 支持 JSON 编辑、格式化、校验和提交。
- 提交前展示变更摘要。
- `400/409` 等失败原因直接展示 API 返回 message，不清空编辑内容。

### Diagnostics

调用接口：

- `GET /api/v1/diagnostics`
- `POST /api/v1/memory/allocator:dump`

展示内容：

- trace、diag-stats 配置状态。
- allocator dump 结果。

allocator dump 是显式按钮动作，执行前确认，执行后展示响应 JSON，并提示统计已写入日志。

## 自动刷新

默认刷新策略：

- `Overview`、`Peers`、`Connections`、`Tunnels`、`Relay` 每 3 秒刷新。
- 页面不可见时暂停或降频。
- 表单编辑、JSON 编辑、确认弹窗打开时暂停当前页刷新。
- 保存、取消或关闭编辑后恢复刷新。
- 请求失败时保留旧数据，在顶部显示错误状态和最后成功刷新时间。

## 前端错误处理

统一处理：

- `401`：清理 token，回到登录页。
- `400`：展示请求体或字段错误。
- `404`：展示资源不存在并提示刷新列表。
- `409`：展示冲突原因，例如 active peer 删除或 connection slot 删除限制。
- `413`：提示请求体过大。
- `503 not_supported`：在功能面板内展示平台不支持。
- 网络错误：保留旧数据，顶部显示连接失败。

## 系统测试设计

### 1. 范围和目标

验证 Console 与 Admin API 共用监听、共用鉴权、资源管理操作正确映射到 `/api/v1/*`，并在不同绑定地址、不同角色和异常状态下保持可用、安全、可观测。

### 2. 功能和非功能目标

功能目标：

- 启用 `--admin-listen` 后 `/console/` 可访问。
- 未认证用户只能加载登录页和静态资源，不能访问 API。
- 使用账号 `raypx2` 和正确 token 可登录。
- Console 能完成 peer、connection、tunnel、relay、config、diagnostics 的首版功能。

非功能目标：

- Console 静态资源首屏加载 p95 小于 500 ms，API 刷新 p95 小于 300 ms，测试环境为本机或同管理网。
- 3 秒自动刷新不应造成 admin worker 长期饱和。
- 单浏览器会话连续运行 8 小时不应出现明显内存增长或请求堆积。
- 非 loopback 绑定必须在启动日志和文档中清楚提示风险。

### 3. 端到端功能路径

| 路径 | 入口 | 下游 | 断言 |
|---|---|---|---|
| 登录成功 | `/console/` -> `GET /api/v1/admin` | Admin auth | 正确 token 返回 200，进入 Overview |
| 登录失败 | `/console/` -> `GET /api/v1/admin` | Admin auth | 错误 token 返回 401，停留登录页 |
| peer 创建 | Peers 表单 -> `POST /api/v1/peers` | Router runtime | 返回 201，列表出现新 peer |
| peer 更新 | Peers 表单/JSON -> `PATCH/PUT /api/v1/peers/{id}` | Router runtime | 返回 200，详情和配置一致 |
| active peer 删除冲突 | `DELETE /api/v1/peers/{id}` | Router runtime | 返回 409，页面展示冲突原因 |
| connection reconnect | `POST /api/v1/peers/{peer_id}/connections/{id}:reconnect` | Client peer runtime | 返回 202，列表状态可刷新观察 |
| tunnel abort | `POST /api/v1/tunnels/{id}:abort` 或 `POST /api/v1/server/tunnels/{id}:abort` | Tunnel registry/runtime | 返回 202，后续列表状态变化或消失 |
| relay 不支持明细 | `GET /api/v1/relay/active-relays/{id}` | Relay metrics | 返回 503 时页面内提示平台不支持 |
| config 提交失败 | `PUT /api/v1/config` | Router config parser | 返回 400/409，编辑内容保留 |
| allocator dump | `POST /api/v1/memory/allocator:dump` | Memory stats | 返回 200，页面展示 JSON，日志有记录 |

### 4. k6 性能基线

建议 k6 场景：

- `static_console_load`：并发加载 `/console/`、`/console/app.js`、`/console/style.css`。
- `overview_refresh`：模拟 20 个浏览器每 3 秒调用 `/api/v1/admin`、`/api/v1/health`、`/api/v1/metrics`。
- `resource_refresh`：模拟 10 个浏览器刷新 peers、connections、tunnels。
- `operator_actions`：低频执行 create/update/delete/reconnect/abort，使用测试 peer 和 tunnel 数据。

基线负载：

- baseline：5 个并发浏览器，持续 5 分钟。
- expected peak：20 个并发浏览器，持续 15 分钟。
- stress：50 个并发浏览器，持续 10 分钟。
- soak：10 个并发浏览器，持续 8 小时。

阈值：

- 静态资源 p95 < 500 ms。
- API p95 < 300 ms，p99 < 1000 ms。
- HTTP error rate < 1%，预期的 401/404/409/503 不计入系统错误。
- admin worker 无持续队列堆积，进程 CPU 和内存无持续异常增长。

### 5. 异常和恢复

| 场景 | 注入故障 | 期望行为 | 验收 |
|---|---|---|---|
| token 错误 | 使用错误 token 登录 | 返回 401 | 页面不进入管理区 |
| token 文件不可读 | 启动时 token 写入失败 | Admin server 启动失败 | 进程输出明确错误 |
| API 网络中断 | 停止进程或断开连接 | 页面保留旧数据并提示连接失败 | 恢复后可重新刷新 |
| 绑定地址冲突 | 端口已占用 | 启动失败 | stderr 输出 bind failed |
| 非 loopback 暴露 | `0.0.0.0:18080` | 启动成功并打印风险提示 | 文档和日志可见 |
| 请求体过大 | 提交超过限制的 JSON | 返回 413 | 页面展示请求体过大 |
| 并发编辑冲突 | 多页面同时修改 peer/config | API 返回 409 或后一提交覆盖 | 页面展示 API 结果，不伪造成功 |

### 6. 可观测性和发布门禁

测试证据：

- 单元测试覆盖路由、鉴权、静态资源、非 loopback listen 校验。
- Admin HTTP 集成测试覆盖 `/console/`、`/api/v1/*` 鉴权隔离。
- router/server 端到端测试覆盖关键操作链路。
- k6 报告保存 p50/p95/p99、错误率、吞吐和资源占用。
- 手工浏览器验证覆盖桌面和窄屏宽度，确认表格、抽屉、弹窗无文字重叠。

发布门禁：

- 所有新增单元和集成测试通过。
- Console 登录、刷新、peer CRUD、connection 操作、tunnel 操作、config JSON 提交流程可演示。
- 非 loopback 绑定安全提示已进入 README 和 admin-api 文档。
- 未启用 `--admin-listen` 时不启动 Admin HTTP，也不提供 Console。

## 风险与约束

- 允许 `0.0.0.0` 会扩大管理面暴露范围，需要用户通过网络 ACL、防火墙或 SSH tunnel 控制访问。
- 首版 token 存在 `sessionStorage`，浏览器会话内 XSS 风险依赖 Console 静态资源自身安全；页面必须避免渲染未转义 HTML。
- JSON 编辑器如果完全手写，体验有限；首版可使用 `<textarea>` 加格式化校验，后续再评估是否引入第三方编辑器。
- 部分 Admin API 当前只读或不支持平台明细，Console 需要明确显示能力边界，不能展示不可用的编辑按钮。
