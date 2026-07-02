# Client Admin API curl 测试命令

本文档用于测试 client/router 模式 Admin API。命令默认 Admin API 地址为 `127.0.0.1:2345`，Token 为用户指定的 Bearer Token。

```bash
ADMIN='http://127.0.0.1:2345/api/v1'
TOKEN='8a1ec69bc4011aee5c7cc75e8817b43ed1629c11a8fa3247031169cbea67d258'
AUTH="Authorization: Bearer ${TOKEN}"
JSON='Content-Type: application/json'
```

## 基础查询

```bash
# 查询 Admin HTTP server 自身状态
curl -sS -H "$AUTH" "$ADMIN/admin"

# 健康检查
curl -sS -H "$AUTH" "$ADMIN/health"

# client/router 指标
curl -sS -H "$AUTH" "$ADMIN/metrics"

# 当前 router 配置
curl -sS -H "$AUTH" "$ADMIN/config"

# 完整运行时配置快照
curl -sS -H "$AUTH" "$ADMIN/runtime/config"

# client 推荐 schema 配置快照
curl -sS -H "$AUTH" "$ADMIN/client/config"

# 诊断状态
curl -sS -H "$AUTH" "$ADMIN/diagnostics"
```

## Peer 增删改查

```bash
# 查询所有 peer
curl -sS -H "$AUTH" "$ADMIN/peers"

# 新增 peer
curl -sS -X POST -H "$AUTH" -H "$JSON" \
  -d '{
    "peer_id": "agent-curl",
    "quic_peer": "127.0.0.1:14444",
    "socks_listen": "127.0.0.1:11080",
    "http_listen": "127.0.0.1:18080",
    "port_forwards": [
      {"listen": "127.0.0.1:15432", "target": "127.0.0.1:5432"}
    ],
    "paths": [
      {"name": "default", "local": "0.0.0.0", "peer": "127.0.0.1:14444", "connections": 1}
    ],
    "quic_connections": 1,
    "compress": "off",
    "enabled": true
  }' \
  "$ADMIN/peers"

# 查询单个 peer 运行时指标
curl -sS -H "$AUTH" "$ADMIN/peers/agent-curl"

# 查询单个 peer 配置
curl -sS -H "$AUTH" "$ADMIN/peers/agent-curl/config"

# 全量替换 peer 配置。请求体内 peer_id 应与路径一致。
curl -sS -X PUT -H "$AUTH" -H "$JSON" \
  -d '{
    "peer_id": "agent-curl",
    "quic_peer": "127.0.0.1:14444",
    "socks_listen": "127.0.0.1:11081",
    "http_listen": "127.0.0.1:18081",
    "port_forwards": [
      {"listen": "127.0.0.1:15433", "target": "127.0.0.1:5432"}
    ],
    "paths": [
      {"name": "default", "local": "0.0.0.0", "peer": "127.0.0.1:14444", "connections": 1}
    ],
    "quic_connections": 2,
    "compress": "off",
    "enabled": true
  }' \
  "$ADMIN/peers/agent-curl"

# 部分修改 peer 配置，未出现字段保持不变。
curl -sS -X PATCH -H "$AUTH" -H "$JSON" \
  -d '{
    "socks_listen": "127.0.0.1:11082",
    "http_listen": "127.0.0.1:18082",
    "quic_connections": 2,
    "compress": "off",
    "enabled": true
  }' \
  "$ADMIN/peers/agent-curl"

# 修改 peer 路径配置
curl -sS -X PATCH -H "$AUTH" -H "$JSON" \
  -d '{
    "paths": [
      {"name": "default", "local": "0.0.0.0", "peer": "127.0.0.1:14444", "connections": 2}
    ]
  }' \
  "$ADMIN/peers/agent-curl"

# 禁用 peer
curl -sS -X POST -H "$AUTH" "$ADMIN/peers/agent-curl:disable"

# 启用 peer
curl -sS -X POST -H "$AUTH" "$ADMIN/peers/agent-curl:enable"

# drain peer：停止接收新流量并等待已有 tunnel 退出
curl -sS -X POST -H "$AUTH" -H "$JSON" \
  -d '{"mode":"drain","grace_seconds":10}' \
  "$ADMIN/peers/agent-curl:drain"

# 中止 peer 下所有 active tunnels
curl -sS -X POST -H "$AUTH" "$ADMIN/peers/agent-curl:abort-tunnels"

# 删除非活跃 peer。默认 mode 为 reject-if-active，活跃 peer 会返回 409。
curl -sS -X DELETE -H "$AUTH" "$ADMIN/peers/agent-curl"

# 强制删除活跃 peer：先中止 tunnel，再删除 peer。
curl -sS -X DELETE -H "$AUTH" -H "$JSON" \
  -d '{"mode":"abort"}' \
  "$ADMIN/peers/agent-curl"
```

## Connection 增删改查

Admin API 当前支持 connection 查询、增加目标 connection、删除最高 slot connection、重连、以及中止指定 connection 下的 tunnels。没有独立的 `PATCH /connections/{connection_id}`；修改 connection 数量可用 `POST /connections`、`DELETE /connections/{connection_id}`，或修改 peer 的 `quic_connections`。

```bash
# 查询 peer 下所有 QUIC connections
curl -sS -H "$AUTH" "$ADMIN/peers/agent-curl/connections"

# 增加一个目标 connection
curl -sS -X POST -H "$AUTH" "$ADMIN/peers/agent-curl/connections"

# 查询单个 connection。connection_id 从上一条 list 响应中获取，例如 conn-1。
curl -sS -H "$AUTH" "$ADMIN/peers/agent-curl/connections/conn-1"

# 重连指定 connection
curl -sS -X POST -H "$AUTH" "$ADMIN/peers/agent-curl/connections/conn-1:reconnect"

# 中止指定 connection 下的 tunnels
curl -sS -X POST -H "$AUTH" "$ADMIN/peers/agent-curl/connections/conn-1:abort-tunnels"

# 删除指定 connection。只能删除最高 slot，且不能删除最后一个 connection。
curl -sS -X DELETE -H "$AUTH" "$ADMIN/peers/agent-curl/connections/conn-1"

# 通过 PATCH peer 修改目标 connection 数量
curl -sS -X PATCH -H "$AUTH" -H "$JSON" \
  -d '{"quic_connections": 3}' \
  "$ADMIN/peers/agent-curl"
```

## Tunnel 和 Relay 辅助查询

```bash
# 查询所有 tunnel
curl -sS -H "$AUTH" "$ADMIN/tunnels"

# 查询单个 tunnel。tunnel_id 从 /tunnels 响应中获取。
curl -sS -H "$AUTH" "$ADMIN/tunnels/tun-1"

# drain tunnel
curl -sS -X POST -H "$AUTH" "$ADMIN/tunnels/tun-1:drain"

# 中止 tunnel
curl -sS -X POST -H "$AUTH" "$ADMIN/tunnels/tun-1:abort"

# DELETE tunnel 等价于 abort
curl -sS -X DELETE -H "$AUTH" "$ADMIN/tunnels/tun-1"

# relay 聚合指标
curl -sS -H "$AUTH" "$ADMIN/relay/metrics"

# active relay capability 和列表
curl -sS -H "$AUTH" "$ADMIN/relay/active-relays"

# worker 列表
curl -sS -H "$AUTH" "$ADMIN/relay/workers"

# aggregate worker 指标
curl -sS -H "$AUTH" "$ADMIN/relay/workers/aggregate"
```

## 全量替换 router 配置

`PUT /api/v1/config` 会替换当前 router peer 列表。测试时谨慎使用，避免误删当前已有 peer。

```bash
curl -sS -X PUT -H "$AUTH" -H "$JSON" \
  -d '{
    "version": 1,
    "peers": [
      {
        "peer_id": "agent-curl",
        "quic_peer": "127.0.0.1:14444",
        "socks_listen": "127.0.0.1:11080",
        "http_listen": "127.0.0.1:18080",
        "port_forwards": [],
        "paths": [
          {"name": "default", "local": "0.0.0.0", "peer": "127.0.0.1:14444", "connections": 1}
        ],
        "quic_connections": 1,
        "compress": "off",
        "enabled": true
      }
    ]
  }' \
  "$ADMIN/config"
```

## 常见错误验证

```bash
# 不带 token，应返回 401 unauthorized
curl -sS "$ADMIN/health"

# 使用错误 token，应返回 401 unauthorized
curl -sS -H 'Authorization: Bearer wrong-token' "$ADMIN/health"

# legacy slash action 公开入口不应使用，预期 404
curl -sS -X POST -H "$AUTH" "$ADMIN/peers/agent-curl/enable"

# 删除不存在的 peer，预期 404
curl -sS -X DELETE -H "$AUTH" "$ADMIN/peers/not-exist"
```
