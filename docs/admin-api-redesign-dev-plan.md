# tcpquic-proxy Admin API 重构开发计划

> 状态：开发计划草案  
> 日期：2026-06-23  
> 关联设计：`docs/admin-api-redesign-design.md`

---

## 1. 原则

Admin API 重构要分阶段落地。第一阶段先替换 HTTP 基础设施和补齐鉴权，保证现有功能不退化；后续再扩展 peer CRUD、connection slot 管理、tunnel/relay 状态。不要在同一个变更里同时改 HTTP server、配置语义、QUIC connection 生命周期和 relay worker snapshot。

每个阶段必须满足：

- 有单元测试。
- 有最小端到端验证。
- README 或 docs 同步更新。
- 失败路径明确，不留下半应用配置。
- 不影响数据面热路径性能。

---

## 2. 阶段总览

| 阶段 | 目标 | 主要产物 |
|------|------|----------|
| Phase 0 | 准备依赖和兼容壳 | vendored `cpp-httplib`、CMake target、兼容 `TqAdminHttpServer` |
| Phase 1 | token auth + 现有 API 迁移 | token 文件、鉴权中间件、`/api/v1/health/metrics/config` |
| Phase 2 | peer CRUD | peer create/update/delete/patch/enable/disable/drain |
| Phase 3 | connection 状态与控制 | connection snapshot、desired slot count 调整、reconnect、abort connection tunnels |
| Phase 4 | tunnel/relay 状态与控制 | tunnel registry snapshot、tunnel abort/drain、relay worker snapshot |
| Phase 5 | server 侧扩展 | server connection/tunnel list、server connection abort |
| Phase 6 | 文档、兼容、清理 | README、旧接口 deprecation、移除旧 parser/server 死代码 |

---

## 3. Phase 0：cpp-httplib 基础集成

### 3.1 任务

1. 引入 `third_party/cpp-httplib/httplib.h`。
2. CMake 增加 `cpp_httplib` interface target。
3. 记录 vendored 版本和许可证，保留 upstream LICENSE 或在 `third_party/cpp-httplib/` 下放置许可说明。
4. 保留 `src/runtime/admin_http.h` 的外部生命周期接口，内部实现切换到 `httplib::Server`。
5. 添加 `--admin-threads <n>` 配置，默认 2，范围 1..32。
6. 设置 read/write timeout、max body、keep-alive 策略。
7. 移除或隔离旧手写 HTTP request parser 的生产路径。

### 3.2 设计注意

- `cpp-httplib` 使用阻塞 socket I/O，必须设置固定 thread pool，避免当前 per-client detached thread 模型继续存在。
- 保留 `TqAdminHttpServer::ListenAddress()`，用于打印实际端口，并写入 token 文件的 `listen` 元数据。
- `listen host:port` 解析仍需保留 loopback 校验。
- Phase 0 不写 token 文件；如果内部实现需要先改构造函数签名，应预留 Phase 1 的 auth hook，避免 Phase 1 再大改 server 生命周期。
- 路由注册不能直接使用文档里的 `{peer_id}` 字面量；实现时用 `cpp-httplib` 正则路由或封装 matcher，并为 path variable 解码和校验补测试。

### 3.3 测试

- `tcpquic_admin_http_test` 改成测试 httplib server 生命周期。
- 验证 loopback 地址允许，`0.0.0.0` 默认拒绝。
- 验证并发请求受 thread pool 正常处理。
- 验证 oversize body 返回 `413` 或统一错误。
- 验证 regex 路由能正确提取 peer_id、connection_id、tunnel_id，且非法 path variable 返回 `400` 或 `404`。

### 3.4 验收

- 旧 `/health`、`/metrics` 测试通过。
- Admin 启停不会泄漏线程。
- `Stop()` 能中断 listen 并等待 worker 退出。

---

## 4. Phase 1：Token Auth 和现有 API 迁移

### 4.1 任务

1. 新增 `TqAdminAuth`：
   - token 生成。
   - token 文件路径选择。
   - token 文件原子写入。
   - token 文件清理。
   - `Authorization: Bearer` 校验。
2. 新增参数：
   - `--admin-token-file <path>`
   - `--admin-allow-unauthenticated-legacy`
3. 新增 `/api/v1` 路由：
   - `GET /api/v1/health`
   - `GET /api/v1/metrics`
   - `GET /api/v1/config`
   - `PUT /api/v1/config`
4. 旧路径转发到新 service。
5. README 更新 token 使用方法。
6. Admin listen 成功但 token 文件写入失败时，必须停止 Admin server 并让进程启动失败。

### 4.2 Token 文件权限

POSIX：

- parent dir：`0700`
- token file：`0600`
- 使用 tmp file + rename。

Windows：

- 使用当前用户 LocalAppData。
- 最低要求是不写入共享目录。
- 后续可补 DACL 限制。

### 4.3 测试

- token 文件生成、JSON 字段正确。
- 默认 token 文件名包含 pid；自定义 `--admin-token-file` 能覆盖默认路径。
- POSIX 文件权限正确。
- 进程退出清理 token 文件前会校验 token 或 pid 匹配当前进程，不误删新实例文件。
- 无 Authorization 返回 `401`。
- 错 token 返回 `401`。
- 正 token 返回 `200`。
- 旧路径默认也要求 token。
- 打开 `--admin-allow-unauthenticated-legacy` 后旧路径可无 token 访问。
- `--admin-allow-unauthenticated-legacy` 只能在 loopback 绑定下生效，且必须打印高风险警告。

### 4.4 验收

- 当前 Admin 功能完全可用。
- 所有新 `/api/v1/*` 接口默认需要 token。
- 日志不打印 token。
- token 文件不存在时，本地自动化脚本可以明确判断 Admin 尚未可用，而不是收到无鉴权服务。

---

## 5. Phase 2：Peer CRUD

### 5.1 任务

1. 在 `TqRouterRuntime` 增加显式 peer 操作：
   - `ListPeers`
   - `GetPeer`
   - `CreatePeer`
   - `ReplacePeer`
   - `PatchPeer`
   - `DeletePeer`
   - `EnablePeer`
   - `DisablePeer`
   - `DrainPeer`
   - `AbortPeerTunnels`
2. 把当前 `PUT /config` 中隐含的 peer apply 逻辑抽成可复用函数。
3. 新增路由：
   - `GET /api/v1/peers`
   - `POST /api/v1/peers`
   - `GET /api/v1/peers/{peer_id}`
   - `PUT /api/v1/peers/{peer_id}`
   - `PATCH /api/v1/peers/{peer_id}`
   - `DELETE /api/v1/peers/{peer_id}`
   - `POST /api/v1/peers/{peer_id}:enable`
   - `POST /api/v1/peers/{peer_id}:disable`
   - `POST /api/v1/peers/{peer_id}:drain`
   - `POST /api/v1/peers/{peer_id}:abort-tunnels`
4. 增加 peer status 中 listener 状态字段。
5. peer status 中的 `active_tunnels`、`total_tunnels` 先标记为 Phase 4 字段；Phase 2 不用 relay 聚合指标推导 peer 级 tunnel 数。

### 5.2 数据一致性

- 所有 peer 写操作在 `TqRouterRuntime` mutex 下更新 config。
- runtime 启动失败时返回清晰状态。
- `strict=true` 请求失败必须回滚 config。
- 删除 peer 默认 `reject-if-active`，支持 body 指定 `drain` 或 `abort`。

### 5.3 测试

- 创建 peer 后 `GET /peers/{id}` 可见。
- 重复创建返回 `409`。
- 修改 data-plane 字段触发 old runtime drain 和 new runtime start。
- disable 后 listener 关闭。
- delete enabled peer 按模式处理 active tunnels。
- invalid port forward / duplicate listen 返回 `400`。
- 在 Phase 4 之前，delete enabled peer 的 active 判断只能基于当前可用 runtime 状态；如果无法精确判断 active tunnel，默认要求调用方显式指定 `mode=drain` 或 `mode=abort`。

### 5.4 验收

- 不再需要通过整体 `PUT /config` 完成 peer 新增/删除。
- 现有 `PUT /config` 行为保持兼容。

---

## 6. Phase 3：Connection 状态与控制

### 6.1 任务

1. 在 `QuicClientSession` 增加 connection snapshot：

```cpp
struct TqConnectionSnapshot {
    std::string ConnectionId;
    uint32_t SlotIndex;
    uint64_t Generation;
    bool Connected;
    bool RetryScheduled;
    std::string State;
    uint64_t ActiveTunnels;
    uint64_t TotalTunnels;
    std::string LastError;
};
```

2. 给 slot 分配稳定 `ConnectionId`。
3. 增加连接控制方法：
   - `SnapshotConnections`
   - `SetDesiredConnectionCount`
   - `StopHighestConnection`
   - `ReconnectConnection`
   - `AbortConnectionTunnels`
4. 在 `TqClientPeerRuntime` 和 `TqClientRuntimeManager` 暴露 peer-scoped connection API。
5. 新增路由：
   - `GET /api/v1/peers/{peer_id}/connections`
   - `GET /api/v1/peers/{peer_id}/connections/{connection_id}`
   - `POST /api/v1/peers/{peer_id}/connections`
   - `DELETE /api/v1/peers/{peer_id}/connections/{connection_id}`
   - `POST /api/v1/peers/{peer_id}/connections/{connection_id}:reconnect`
   - `POST /api/v1/peers/{peer_id}/connections/{connection_id}:abort-tunnels`

### 6.2 风险

- `QuicClientSession::StartSlot` 当前是 private，重连逻辑与 reconnect callback 紧密耦合。
- 第一版不允许删除中间 slot；删除非最高序号 slot 返回 `409 Conflict`。任意 slot 删除需要先把 config 从 `quic_connections` 数量升级为显式 slot 列表。
- `quic_connections` 是 config 字段，动态增删 connection 需要同步 config，避免 GET config 与 runtime 不一致。

### 6.3 测试

- list connections 返回 slot 数。
- 断开 server 后 connection state 变 reconnecting/connecting。
- POST connection 增加最高序号 slot 并更新 config。
- DELETE 最高序号 connection 后该 slot 不再 pick 新 tunnel。
- DELETE 中间 slot 返回 `409 Conflict`。
- reconnect 不影响其他 slot。

### 6.4 验收

- Admin 能看到每条 QUIC connection 的状态。
- 能单独重连一条 connection，能按最高序号缩容 connection slot。
- peer 级 metrics 与 connection list 聚合一致。

---

## 7. Phase 4：Tunnel/Relay 状态与控制

### 7.1 任务

1. 扩展 `tunnel_registry`：
   - 为每个 tunnel 分配 stable `tunnel_id`。
   - 保存 peer_id、connection_id、target、role、ingress、created_at。
   - 保存 abort/drain callback。
   - 支持 snapshot list。
2. 扩展 relay worker snapshot：
   - Linux worker 暴露 per-relay state。
   - Darwin/Windows 先暴露可用子集。
3. 新增 tunnel API：
   - `GET /api/v1/tunnels`
   - `GET /api/v1/tunnels/{tunnel_id}`
   - `DELETE /api/v1/tunnels/{tunnel_id}`
   - `POST /api/v1/tunnels/{tunnel_id}:drain`
   - `POST /api/v1/tunnels/{tunnel_id}:abort`
4. 新增 relay API：
   - `GET /api/v1/relay/metrics`
   - `GET /api/v1/relay/workers`
   - `GET /api/v1/relay/workers/{worker_id}`

### 7.2 Snapshot 字段

最低可交付字段：

- tunnel_id
- peer_id
- connection_id
- state
- target
- compress
- created_at
- duration_ms
- tcp_read_bytes
- tcp_write_bytes
- pending bytes
- relay backend
- worker index
- last_error

### 7.3 风险

- tunnel context 生命周期复杂，snapshot 不能持有悬空指针。
- relay worker 状态由 worker 线程拥有，snapshot 需要无锁原子字段或短锁复制。
- abort/drain 回调不能在 registry 锁内执行，避免死锁。

### 7.4 测试

- 建立 HTTP CONNECT tunnel 后 list tunnels 可见。
- 按 peer_id/connection_id filter 可用。
- abort tunnel 后客户端连接关闭。
- relay metrics 与现有 `/metrics` 字段一致。
- 并发 tunnel 创建/关闭时 snapshot 不崩溃。

### 7.5 验收

- 能定位单条活跃 relay/tunnel。
- 能通过 Admin 终止指定 tunnel。
- 聚合 relay metrics 与 per-tunnel snapshot 不冲突。

---

## 8. Phase 5：Server 侧扩展

### 8.1 任务

1. 新增 server connection registry：
   - conn_id
   - remote address
   - state
   - active streams
   - total streams
   - last_error
2. 将 server incoming stream 注册为 tunnel。
3. 新增 server API：
   - `GET /api/v1/server`
   - `GET /api/v1/server/metrics`
   - `GET /api/v1/server/connections`
   - `GET /api/v1/server/connections/{connection_id}`
   - `GET /api/v1/server/tunnels`
   - `POST /api/v1/server/connections/{connection_id}:abort-tunnels`

### 8.2 测试

- client 连接 server 后 server connection list 可见。
- 建立 tunnel 后 server tunnel list 可见。
- abort server connection tunnels 后客户端连接失败或关闭。

### 8.3 验收

- server 模式不再只有聚合 metrics。
- server 侧能按 connection/tunnel 定位问题。

---

## 9. Phase 6：文档、兼容和清理

### 9.1 任务

1. README 更新：
   - token 文件路径。
   - curl 示例。
   - API 表格。
   - 旧 API deprecation。
2. `docs/config_guide_cn.md` 更新 Admin 配置。
3. `src/docs/thread_model_cn.md` 更新 Admin thread pool 模型。
4. 清理旧 `TqParseHttpRequest` 等只为旧 server 存在的代码。
5. 确认 Windows build include 顺序，`httplib.h` 与 Windows headers 不冲突。

### 9.2 curl 示例

```bash
TOKEN_FILE="$XDG_RUNTIME_DIR/tcpquic-proxy/admin-token.json"
# 启动 tcpquic-proxy 时显式传入：
#   --admin-token-file "$TOKEN_FILE"
TOKEN=$(jq -r .token "$TOKEN_FILE")
curl -H "Authorization: Bearer ${TOKEN}" \
  http://127.0.0.1:19091/api/v1/health
```

### 9.3 验收

- 用户可以只按 README 完成 token 读取和 API 调用。
- 旧 API 标注 deprecated。
- 所有 Admin 单测和 smoke test 通过。

---

## 10. 测试矩阵

| 类别 | 测试 |
|------|------|
| Auth | no token、wrong token、correct token、legacy unauth switch |
| Token file | default path、custom path、permission、cleanup、restart overwrite |
| HTTP server | start/stop、thread pool、large body、bad JSON、404 |
| Config | GET/PUT roundtrip、invalid config rollback |
| Peer | create、duplicate、update、patch、delete、enable、disable、drain |
| Connection | list、add、delete、reconnect、abort tunnels |
| Tunnel | list、filter、abort、drain、concurrent close |
| Relay | aggregate metrics、worker metrics、per-tunnel state |
| Server | health、metrics、connection list、tunnel list |
| Compatibility | old routes with token、old routes unauth with explicit switch |

---

## 11. 建议提交拆分

1. `vendor: add cpp-httplib`
2. `admin: replace socket http server with httplib wrapper`
3. `admin: add bearer token file auth`
4. `admin: add v1 health metrics config routes`
5. `router: add peer CRUD service methods`
6. `admin: expose peer CRUD routes`
7. `quic: add client connection snapshots`
8. `admin: expose connection routes`
9. `tunnel: add tunnel snapshots and abort by id`
10. `admin: expose tunnel and relay routes`
11. `server: add server connection snapshots`
12. `docs: document admin v1 API and token workflow`

---

## 12. 开发检查清单

### Phase 0/1

- [ ] `cpp-httplib` vendored。
- [ ] Admin server 使用固定 thread pool。
- [ ] token 文件生成。
- [ ] `/api/v1/health` 需要 token。
- [ ] 旧接口兼容。

### Phase 2

- [ ] peer CRUD service 方法完成。
- [ ] peer CRUD routes 完成。
- [ ] config apply rollback 语义明确。
- [ ] peer CRUD 单测完成。

### Phase 3

- [ ] connection id 模型确定。
- [ ] connection snapshot 完成。
- [ ] connection add/delete/reconnect 完成。
- [ ] connection API 单测完成。

### Phase 4

- [ ] tunnel registry snapshot 完成。
- [ ] tunnel abort/drain by id 完成。
- [ ] relay per-tunnel snapshot 完成。
- [ ] tunnel/relay API 单测完成。

### Phase 5/6

- [ ] server connection registry 完成。
- [ ] server routes 完成。
- [ ] README 更新。
- [ ] 线程模型文档更新。
- [ ] smoke test 更新。

---

## 13. 主要风险与缓解

| 风险 | 缓解 |
|------|------|
| Admin 重构影响现有脚本 | 保留旧路径并提供迁移期 |
| token 文件权限跨平台差异 | POSIX 先严格落地，Windows 单独补 DACL 测试 |
| connection slot 控制破坏 reconnect 逻辑 | 先做 readonly snapshot，再做最高序号 slot scale up/down 和 reconnect |
| tunnel snapshot 悬空指针 | registry 存稳定 metadata，snapshot 复制值，不暴露裸指针 |
| relay worker snapshot 增加热路径开销 | 使用原子计数和按需短锁，不在数据转发路径构造 JSON |
| HTTP handler 阻塞 runtime 锁 | service 层快照后释放锁，再序列化 JSON |

---

## 14. 首个可交付版本定义

最小可交付版本建议包含 Phase 0、Phase 1 和 Phase 2：

- 基于 `cpp-httplib` 的 Admin server。
- token 文件鉴权。
- `/api/v1/health`、`/api/v1/metrics`、`/api/v1/config`。
- peer 增删改查、enable/disable/drain/abort-tunnels。
- 旧接口兼容。

Connection slot 控制和 tunnel/relay 的完整状态作为后续版本迭代，避免第一版重构范围过大。
