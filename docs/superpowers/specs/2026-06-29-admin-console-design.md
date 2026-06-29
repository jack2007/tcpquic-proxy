# 内置 Admin Console 设计

日期：2026-06-29

## UI 参照冻结

后续开发必须严格以 [2026-06-29-admin-console-interaction-v4.html](/home/jack/src/tcpquic-proxy/docs/superpowers/specs/2026-06-29-admin-console-interaction-v4.html) 作为 UI 参照，不允许在开发过程中随意发挥页面结构、导航、表格字段、表单顺序、按钮能力或状态展示。

本设计文档中的字段约束和页面行为是实现边界；HTML 快照是布局、交互层级和视觉密度参照。若开发时发现必须偏离 v4 参照，需要先更新设计文档和 UI 快照，并重新确认后再实现。

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
- 不为了页面展示修改 tcpquic-proxy 的核心通信协议或核心通信流程。页面字段必须来自当前 Admin API 已经直接返回的数据，或由这些返回数据在前端做简单聚合得到。

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

Console 根据运行进程的 role 展示不同业务导航。`Overview` 不是跨角色公共页面，而是 role 内的首页。

2026-06-29 使用反馈修订：

- 登录成功后，Console 必须以 `/api/v1/admin` 返回的 `role` 为准固定展示当前进程角色，不在侧栏提供 client/server 手动切换。client 进程只显示 client 页面；server 进程只显示 server 页面。
- role 只作为顶部状态展示，不作为交互控件。前端不再保存或恢复用户手动选择的 role。

client 业务页面：

- `Overview`
- `Peers`
- `Connections`
- `Tunnels`

server 业务页面：

- `Overview`
- `Peers`
- `Connections`
- `Tunnels`
- `ACL`

共享辅助页面：

- `Relay`
- `Config`
- `Diagnostics`

左侧导航只显示当前 role 可用页面。server 导航项不带 `Server` 前缀，即显示为 `Overview`、`Peers`、`Connections`、`Tunnels`、`ACL`。顶部状态条只展示 role、health、刷新状态、最后成功刷新时间、退出登录，不展示 `admin.listen` 绑定地址。主区展示资源表格、详情抽屉、表单或 JSON viewer。

所有页面遵守 API 字段约束：

- 首版页面只展示当前 Admin API 已直接返回的字段。
- 允许前端基于 API 响应做简单聚合，例如计数、按 `remote_address` 分组、按 `peer_id` 汇总 tunnel 数。
- 不展示需要新增核心通信协议字段、修改 QUIC/TCP 数据面或修改 tunnel 控制协议才能获得的信息。
- 如果后续 Admin API 新增非数据面字段，页面设计需要单独更新，不能在本设计中预设展示。

首版页面数据来源：

| 页面 | 数据来源 | 约束 |
|---|---|---|
| client Overview | `/api/v1/health`、`/api/v1/metrics`、`/api/v1/peers`、`/api/v1/tunnels` | 只做计数和求和聚合 |
| client Peers | `/api/v1/peers`、`/api/v1/peers/{peer_id}`、`/api/v1/peers/{peer_id}/config` | Create/Edit 只提交现有 peer JSON 字段 |
| client Connections | `/api/v1/peers`、`/api/v1/peers/{peer_id}/connections`、`/api/v1/peers/{peer_id}/connections/{connection_id}` | 前端聚合所有 peer 的连接；不展示连接成功时间、断开时间、连接累计字节数 |
| client Tunnels | `/api/v1/tunnels` | 不展示 source |
| server Overview | `/api/v1/health`、`/api/v1/metrics`、`/api/v1/server/connections`、`/api/v1/server/tunnels` | peer 只按 `remote_address` 聚合 |
| server Peers | `/api/v1/server/connections`、`/api/v1/server/tunnels` | 不展示 remote identity、first_seen、last_seen、transferred bytes |
| server Connections | `/api/v1/server/connections` | 不展示 Reconnecting、连接时间或字节字段 |
| server Tunnels | `/api/v1/server/tunnels` | 不展示 remote source、ingress、bytes、pending 或 last_error |
| server ACL | `/api/v1/server/config`、`/api/v1/metrics` | 不展示逐条命中历史或最近拒绝列表 |

### client Overview

调用接口：

- `GET /api/v1/admin`
- `GET /api/v1/health`
- `GET /api/v1/metrics`
- `GET /api/v1/peers`
- `GET /api/v1/tunnels`

展示内容：

- role、status、uptime_seconds。
- peer 数、enabled peer 数、connection_count/connected_connections 汇总、active_streams、total_streams、reconnects、active tunnel 数、last_error。
- peer 摘要表，按 `peer -> connection -> tunnel` 层级汇总。
- peer state 使用 API 返回的 `healthy`、`connecting`、`down`、`disabled` 等值，不在前端改名。
- 快捷入口：Peers、Connections、Tunnels、Config。

### client Peers

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

表格列：

- `peer_id`
- `state`
- `enabled`
- `quic_peer`
- `paths` 数量或摘要
- `socks_listen`
- `http_listen`
- `connection_count`
- `connected_connections`
- `active_streams`
- `total_streams`
- `reconnects`
- `last_error`
- `actions`

页面行为：

- 列表 actions 只提供 `Edit` 和 `Delete`。
- 不在 peer 行提供 `Drain`、`Abort tunnels` 或其他运行时动作。
- 页面采用上下布局：先展示当前所有 peers 列表；列表下方展示 `Create/Edit Peer` 表单；页面最下方放置 `Create Peer` 按钮。
- `Edit` 将选中 peer 的当前字段填入下方表单并进入 edit mode。
- 最下方 `Create Peer` 按钮只清空表单并进入 create mode，不立即提交 API 请求。
- `Create/Edit Peer` 表单字段顺序固定为：`peer_id`、peer endpoint、`socks_listen`、`http_listen`、`port_forwards`、`enabled`。
- peer endpoint 支持两种互斥表达：
  - 对端地址列表模式：填写 `quic_peer`，例如 `10.201.1.2:4433,10.201.1.3:4433`。
  - paths 模式：填写 `paths` 数组，每个 path 包含 `name`、`local`、`peer`、`connections`。
- `socks_listen` 默认展示 `127.0.0.1:1080`。
- `http_listen` 默认展示 `127.0.0.1:8080`。
- 高级 JSON 编辑使用当前 API 已支持的 peer JSON 字段：`peer_id`、`quic_peer`、`socks_listen`、`http_listen`、`port_forwards`、`paths`、`quic_connections`、`compress`、`enabled`，用于完整替换 peer 或复制当前配置。
- 删除 active peer 默认依赖 API 返回 `409` 或相应错误，页面展示错误原因，不提供 drain/abort 替代按钮。

### client Connections

调用接口：

- `GET /api/v1/peers`
- `GET /api/v1/peers/{peer_id}/connections`
- `GET /api/v1/peers/{peer_id}/connections/{connection_id}`

页面行为：

- 只提供信息查询展示，不提供 actions。
- 右上角不提供 `Add highest slot`。
- 不提供 Delete、Reconnect、Abort tunnels 等连接操作。
- 页面进入时直接展示所有 peer 的连接信息。当前没有独立的 `/api/v1/connections`，因此前端先读取 `/api/v1/peers`，再对每个 peer 调用 `/api/v1/peers/{peer_id}/connections` 聚合成全量连接列表。
- 每条连接提供一个 `Detail` 按钮。点击后调用 `/api/v1/peers/{peer_id}/connections/{connection_id}`，在页面下方详情区展示该连接 JSON。
- 状态使用 API 返回的 `state`；布尔状态使用 `connected` 和 `retry_scheduled`。

表格列：

- `connection_id`
- `peer_id`
- `slot_index`
- `generation`
- `connected`
- `retry_scheduled`
- `state`
- `path`
- `local`
- `peer`
- `active_tunnels`
- `last_error`

当前 connection API 不返回连接成功时间、断开时间或连接累计传输字节数，首版页面不展示这些字段。

### client Tunnels

调用接口：

- `GET /api/v1/tunnels`
- `GET /api/v1/tunnels/{tunnel_id}`

页面行为：

- 只提供信息查询展示，不提供 actions。
- 不提供 Delete、Drain、Abort 等 tunnel 操作。
- 不提供 Client/Router API/Server API 下拉框；一个进程内不会同时存在这几个 role，页面根据当前 role 决定 API 和列。

表格列：

- `tunnel_id`
- `peer_id`
- `connection_id`
- `target`
- `state`
- `role`
- `ingress`
- `compress`
- `created_at`
- `duration_ms`
- `tcp_read_bytes`
- `tcp_write_bytes`
- `pending_bytes`
- `relay_backend`
- `worker_index`
- `last_error`

当前 `TqTunnelSnapshot` 和 `/api/v1/tunnels` 不返回 client source。首版页面不展示 `source`，也不为了展示该字段修改核心通信代码。

### server Overview

调用接口：

- `GET /api/v1/admin`
- `GET /api/v1/health`
- `GET /api/v1/metrics`
- `GET /api/v1/server/connections`
- `GET /api/v1/server/tunnels`

展示内容：

- role、status、uptime_seconds、listen、resolved_listens、accepted_connections、active_streams、total_streams、acl_denied、tcp_dialing、last_error。
- peer 数、connection 数、active tunnel 数、ACL denied 统计。
- server peer 是运行时聚合概念，不是配置资源；用于表示当前有多少远端 peer 与本 server 有连接。
- 层级仍按 `peer -> connection -> tunnel` 展示。
- 当前 server connection API 没有明确 peer identity，首版使用 connection `remote_address` 归组，并在页面中标注该来源。

### server Peers

调用接口：

- `GET /api/v1/server/connections`
- `GET /api/v1/server/tunnels`

页面行为：

- 只读运行时视图，不提供 Create、Edit、Delete。
- 展示当前与 server 有连接的 peer 数，以及每个 peer 下的 connection/tunnel 汇总。
- 可按 remote address、connection 数、active tunnel 数排序。

表格列：

- peer 显示名，使用 `peer-<remote_address>`。
- `remote_address`
- connection 数，按 `remote_address` 聚合。
- `active_streams` 汇总。
- `total_streams` 汇总。
- `active_tunnels` 汇总。
- `last_error`

### server Connections

调用接口：

- `GET /api/v1/server/connections`
- `GET /api/v1/server/connections/{connection_id}`

页面行为：

- 只提供信息查询展示，不提供 actions。
- server 端不展示 `Reconnecting` 状态。
- 第一版不区分正在连接中和已经连接成功的连接状态，统一展示当前 server 已登记且可观测的连接。

表格列：

- `connection_id`
- peer 显示名或 remote address 归组。
- `remote_address`
- `state`
- `active_streams`
- `total_streams`
- `active_tunnels`
- `last_error`

当前 `TqServerConnectionSnapshot` 已包含 `remote_address`、`state`、`active_streams`、`total_streams`、`active_tunnels`、`last_error`。页面不展示未返回的时间或字节字段。

### server Tunnels

调用接口：

- `GET /api/v1/server/tunnels`
- `GET /api/v1/server/tunnels/{tunnel_id}`

页面行为：

- 只提供信息查询展示，不提供 actions。
- 不提供 Delete、Drain、Abort 等 tunnel 操作。
- server tunnel 页面不强行展示 remote source。当前 server tunnel JSON 只包含 tunnel、peer、connection、state、target、role、duration、active 等字段，没有 remote source 字段；因此首版不展示 remote source。
- 如需定位来源，可通过 `connection_id` 跳转到 server Connections 页面查看 connection `remote_address`。
- 不为展示 remote source 修改当前通信协议。

表格列：

- `tunnel_id`
- `peer_id`
- `connection_id`
- `state`
- `target`
- `role`
- `duration_ms`
- `active`

### server ACL

调用接口：

- `GET /api/v1/server/config`
- `GET /api/v1/metrics`
- `GET /api/v1/admin`

页面行为：

- server 独有页面。
- 首版只读展示 server config 中的 `allow_targets`、`deny_targets`，以及 metrics 中的 aggregate `acl_denied` 和 `last_error`。
- 当前 API 不提供逐条规则命中历史或最近拒绝列表，页面不展示 `matches`、`last_match`、`time`、`reason` 等字段，不新增数据面协议字段。

表格和面板：

- 规则摘要：`allow_targets`、`deny_targets`。
- 拒绝统计：`acl_denied`、`last_error`。
- 当前配置 JSON viewer：展示 `/api/v1/server/config` 中 ACL 相关片段。

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
- Console 能完成 client peer 配置管理、client/server connection 查询、client/server tunnel 查询、server ACL 只读查看、relay、config、diagnostics 的首版功能。

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
| client connection 查询 | Connections 页面 -> `GET /api/v1/peers/{peer_id}/connections` | Client peer runtime | 展示当前 connection API 返回字段，不出现 actions |
| client tunnel 查询 | Tunnels 页面 -> `GET /api/v1/tunnels` | Tunnel registry/runtime | 展示当前 tunnel API 返回字段，不出现 source，不出现 actions |
| server peer 聚合 | Server Peers 页面 -> `GET /api/v1/server/connections`、`GET /api/v1/server/tunnels` | Server runtime | 按 peer/remote_address 汇总 connection 和 tunnel |
| server connection 查询 | Server Connections 页面 -> `GET /api/v1/server/connections` | Server runtime | 不展示 Reconnecting，不出现 actions |
| server tunnel 查询 | Server Tunnels 页面 -> `GET /api/v1/server/tunnels` | Tunnel registry/runtime | 不展示 remote source，提供 connection_id 关联 |
| server ACL 查看 | Server ACL 页面 -> `GET /api/v1/server/config`、`GET /api/v1/metrics` | Server config/metrics | 展示 ACL 配置摘要和 acl_denied 聚合指标 |
| relay 不支持明细 | `GET /api/v1/relay/active-relays/{id}` | Relay metrics | 返回 503 时页面内提示平台不支持 |
| config 提交失败 | `PUT /api/v1/config` | Router config parser | 返回 400/409，编辑内容保留 |
| allocator dump | `POST /api/v1/memory/allocator:dump` | Memory stats | 返回 200，页面展示 JSON，日志有记录 |

### 4. k6 性能基线

建议 k6 场景：

- `static_console_load`：并发加载 `/console/`、`/console/app.js`、`/console/style.css`。
- `overview_refresh`：模拟 20 个浏览器每 3 秒调用 `/api/v1/admin`、`/api/v1/health`、`/api/v1/metrics`。
- `resource_refresh`：模拟 10 个浏览器刷新 peers、connections、tunnels。
- `operator_actions`：低频执行 client peer create/update/delete、config submit、allocator dump，使用测试 peer 和配置数据。

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
- Console 登录、刷新、client peer CRUD、client/server connection 查询、client/server tunnel 查询、server ACL 查看、config JSON 提交流程可演示。
- 非 loopback 绑定安全提示已进入 README 和 admin-api 文档。
- 未启用 `--admin-listen` 时不启动 Admin HTTP，也不提供 Console。

## 风险与约束

- 允许 `0.0.0.0` 会扩大管理面暴露范围，需要用户通过网络 ACL、防火墙或 SSH tunnel 控制访问。
- 首版 token 存在 `sessionStorage`，浏览器会话内 XSS 风险依赖 Console 静态资源自身安全；页面必须避免渲染未转义 HTML。
- JSON 编辑器如果完全手写，体验有限；首版可使用 `<textarea>` 加格式化校验，后续再评估是否引入第三方编辑器。
- 部分 Admin API 当前只读或不支持平台明细，Console 需要明确显示能力边界，不能展示不可用的编辑按钮。
