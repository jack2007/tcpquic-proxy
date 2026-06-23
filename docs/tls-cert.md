# TLS Certificate Modes

本文说明 tcpquic-proxy 基于 msquic/QUIC 的证书配置选择。QUIC 数据面始终通过 TLS 1.3 派生的密钥加密；证书配置决定的是身份认证方式，而不是是否加密。

## 背景

msquic 的 `QUIC_CREDENTIAL_TYPE_NONE` 只适用于客户端，表示客户端不提供客户端证书。服务端仍然需要配置证书和私钥，否则无法作为 TLS 服务端完成正常握手。

`QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION` 表示客户端不验证服务端证书。该模式下数据仍然加密，但客户端无法确认对端身份，存在中间人攻击风险，不应作为生产默认配置。

## TLS 后端

tcpquic-proxy 在 Linux、Windows、macOS 三个平台统一使用仓库内 vendored 的 msquic + quictls TLS 后端。

构建和运行约束：

- 不使用系统 OpenSSL/libcrypto。
- 不使用 Windows Schannel。
- 不依赖操作系统证书存储完成本项目的私有 CA 校验。
- 三个平台均通过 quictls 路径处理 PEM 证书文件。

因此，方案 2 中客户端的 `ca.crt` 应作为 PEM 文件传给 msquic/quictls，用于验证服务端证书；服务端使用 PEM 格式的 `server.crt` 和 `server.key`。

## 方案 1: 双向证书认证

这是当前项目早期默认配置方式，也就是 mTLS。

```text
client:
  client.crt
  client.key
  ca.crt

server:
  server.crt
  server.key
  ca.crt
```

认证关系：

- 客户端使用 `ca.crt` 验证服务端证书。
- 服务端使用 `ca.crt` 验证客户端证书。
- 客户端和服务端都需要各自的证书和私钥。

msquic 配置要点：

```text
client:
  Type  = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE
  Flags = QUIC_CREDENTIAL_FLAG_CLIENT
        | QUIC_CREDENTIAL_FLAG_SET_CA_CERTIFICATE_FILE

server:
  Type  = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE
  Flags = QUIC_CREDENTIAL_FLAG_REQUIRE_CLIENT_AUTHENTICATION
        | QUIC_CREDENTIAL_FLAG_SET_CA_CERTIFICATE_FILE
```

适用场景：

- 服务端需要强认证客户端身份。
- 双方都能安全分发和轮换证书及私钥。

代价：

- 客户端部署需要携带 `client.crt` 和 `client.key`。
- 私钥分发面更大，证书运维成本更高。

## 方案 2: 客户端单向验证服务端证书

这是本项目最终选择的证书模型。客户端不提供客户端证书，服务端不验证客户端证书；客户端仍然验证服务端证书。

```text
client:
  ca.crt

server:
  server.crt
  server.key
```

认证关系：

- 客户端使用 `ca.crt` 验证服务端证书。
- 服务端不要求客户端证书，也不验证客户端证书。
- 客户端不需要 `client.crt` 和 `client.key`。
- 服务端在该模式下不需要 `ca.crt`，除非未来重新启用客户端证书认证。

msquic 配置要点：

```text
client:
  Type  = QUIC_CREDENTIAL_TYPE_NONE
  Flags = QUIC_CREDENTIAL_FLAG_CLIENT
        | QUIC_CREDENTIAL_FLAG_SET_CA_CERTIFICATE_FILE
  CaCertificateFile = ca.crt

server:
  Type  = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE
  Flags = QUIC_CREDENTIAL_FLAG_NONE
```

适用场景：

- 只需要客户端确认自己连到正确的服务端。
- 客户端身份通过其他机制控制，例如本地代理鉴权、网络 ACL、部署侧访问控制、预共享配置或应用层令牌。
- 希望减少客户端证书和私钥分发。

安全边界：

- 数据仍然加密。
- 服务端身份由 `ca.crt` 验证。
- 服务端不能通过 TLS 客户端证书确认客户端身份。

证书要求：

- `server.crt` 必须由客户端信任的私有 CA 签发。
- `server.crt` 的 SAN 应包含客户端连接使用的 DNS 名称或 IP 地址。
- 客户端必须携带签发服务端证书的 CA 公钥证书 `ca.crt`。

## 方案 3: 客户端不验证服务端证书

这是测试或受控实验模式，不作为生产选择。

```text
client:
  no crt
  no key
  no ca.crt

server:
  server.crt
  server.key
```

认证关系：

- 客户端不提供客户端证书。
- 客户端不验证服务端证书。
- 服务端仍然需要 `server.crt` 和 `server.key` 完成 TLS 服务端握手。
- 服务端在该模式下不需要 `ca.crt`，除非要求客户端证书认证。

msquic 配置要点：

```text
client:
  Type  = QUIC_CREDENTIAL_TYPE_NONE
  Flags = QUIC_CREDENTIAL_FLAG_CLIENT
        | QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION

server:
  Type  = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE
  Flags = QUIC_CREDENTIAL_FLAG_NONE
```

安全边界：

- 数据仍然加密。
- 客户端无法确认服务端身份。
- 中间人可以使用自己的证书完成伪装连接，因此该模式不应进入生产默认配置。

## 方案对比

| 方案 | 客户端文件 | 服务端文件 | 客户端验证服务端 | 服务端验证客户端 | 生产建议 |
|------|------------|------------|------------------|------------------|----------|
| 方案 1: mTLS | `client.crt`, `client.key`, `ca.crt` | `server.crt`, `server.key`, `ca.crt` | 是 | 是 | 适合需要强客户端身份认证的部署 |
| 方案 2: 单向验证 | `ca.crt` | `server.crt`, `server.key` | 是 | 否 | 本项目最终选择 |
| 方案 3: 不验证服务端 | 无 | `server.crt`, `server.key` | 否 | 否 | 仅测试/临时诊断 |

## 项目选择

tcpquic-proxy 采用方案 2：

```text
客户端单向验证服务端证书，服务端不验证客户端证书。
```

目标是保留 QUIC/TLS 加密和服务端身份校验，同时避免在所有客户端节点分发客户端证书和私钥。

实现侧应满足：

- Linux、Windows、macOS 都使用 vendored quictls，不使用系统 crypto/TLS 后端。
- client credential 使用 `QUIC_CREDENTIAL_TYPE_NONE`。
- client flags 包含 `QUIC_CREDENTIAL_FLAG_CLIENT` 和 `QUIC_CREDENTIAL_FLAG_SET_CA_CERTIFICATE_FILE`。
- client 配置 `CaCertificateFile = ca.crt`。
- client 不设置 `QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION`。
- server credential 使用 `QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE`。
- server flags 不设置 `QUIC_CREDENTIAL_FLAG_REQUIRE_CLIENT_AUTHENTICATION`。
- server 不依赖 `ca.crt` 验证客户端证书。
