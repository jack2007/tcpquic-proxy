# SOCKS5 / HTTP CONNECT 代理多用户鉴权设计

> 日期：2026-06-16  
> 状态：已评审

## 1. 目标

为 client 侧本地 ingress 代理（SOCKS5、HTTP CONNECT）增加可选的用户名/密码鉴权：

- 凭证在 `--client-config` JSON **顶层**配置，与 `peers` 解耦
- 支持**多组** username/password，任意一组匹配即通过
- 所有 peer 的 SOCKS5 / HTTP CONNECT listener **共享**同一用户表
- 未配置或空数组时保持现有无鉴权行为
- 纯 CLI 单 client（无 `--client-config`）不支持鉴权

## 2. 配置

```json
{
  "version": 1,
  "proxy_auth": [
    { "username": "alice", "password": "secret-a" },
    { "username": "bob", "password": "secret-b" }
  ],
  "peers": [
    {
      "peer_id": "local",
      "quic_peer": "127.0.0.1:14444",
      "socks_listen": "127.0.0.1:1080",
      "http_listen": "127.0.0.1:8080"
    }
  ]
}
```

### 校验

| 规则 | 说明 |
|------|------|
| 缺省 / `[]` | 不启用鉴权 |
| 非空 | 所有 listener 强制鉴权 |
| 每条 | `username`、`password` 均非空 |
| 唯一性 | `username` 不可重复（大小写敏感） |
| 长度 | 各 ≤ 255 字节（SOCKS5 RFC 1929） |
| 数量 | 最多 64 条 |

## 3. 协议行为

### SOCKS5（RFC 1929）

- 启用鉴权：method 协商仅提供 `0x02`（username/password）
- 子协商：读取 ver/ulen/user/plen/pass，调用共享校验表
- 失败：回复 status `0x01`，关闭连接
- 成功：继续原有 CONNECT 流程

### HTTP CONNECT（Proxy-Authorization: Basic）

- 启用鉴权：解析 `Proxy-Authorization: Basic <base64>`
- 解码 `username:password`（password 可含 `:`，按首个 `:` 分割）
- 失败：`407 Proxy Authentication Required`，头 `Proxy-Authenticate: Basic realm="tcpquic-proxy"`
- 成功：返回 `200 Connection Established`

## 4. 实现结构

```
TqRouterConfig.ProxyAuth[]     ← JSON 解析
        ↓
TqProxyAuthTable               ← ingress/proxy_auth.{h,cpp}
        ↓
TqSocks5Server / TqHttpConnectServer（构造时传入 shared_ptr<const TqProxyAuthTable>）
```

多用户校验：遍历全部条目，常数时间比较 username 与 password，任一完全匹配即成功；不对单条提前 return。

## 5. 非目标

- per-peer 凭证、CLI 传参、Digest/NTLM/GSSAPI
- QUIC 隧道层鉴权、角色/ACL

## 6. 测试

- `proxy_auth_test.cpp`：Basic 解析、多用户匹配、常数时间比较边界
- `config_router_test.cpp`：`proxy_auth` 解析与校验
- `http_connect_server_test.cpp` / `socks5_server_test.cpp`：集成 helper（如适用）
