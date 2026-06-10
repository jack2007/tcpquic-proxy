# 内部测试 mTLS 证书

本目录存放 **tcpquic-proxy 内部联调与自动化测试** 使用的自签名 mTLS 材料，默认有效期 **10 年**（3650 天）。

> **仅限开发/测试环境。** 私钥随仓库提交，便于双机脚本与本地冒烟测试直接引用；**禁止**用于生产或对公网暴露的服务。

## 目录结构

```text
cert/
├── README.md
├── generate-certs.sh      # Linux / macOS / WSL 生成脚本
├── generate-certs.ps1     # Windows PowerShell 生成脚本
├── ca.crt                 # 测试 CA 证书（双方 --quic-ca 共用）
├── ca.key                 # 测试 CA 私钥
├── server/
│   ├── server.crt
│   └── server.key
└── client/
    ├── client.crt
    └── client.key
```

## 生成证书

需要已安装 **OpenSSL**（`openssl` 在 `PATH` 中）。可选环境变量 `CERT_DAYS` 覆盖有效天数（默认 `3650`）。

### Linux / macOS / WSL

```bash
cd /path/to/tcpquic-proxy/cert
chmod +x generate-certs.sh
./generate-certs.sh
```

### Windows（PowerShell）

```powershell
cd C:\path\to\tcpquic-proxy\cert
.\generate-certs.ps1
```

若提示无法执行脚本，可先运行：

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
```

Windows 上若未单独安装 OpenSSL，可使用 Git for Windows 自带的 `openssl.exe`，确保其所在目录在 `PATH` 中。

脚本会覆盖 `ca.*`、`server/*`、`client/*` 下已有 PEM 文件。

## 证书属性

| 角色 | CN | EKU | SAN |
|------|-----|-----|-----|
| CA | `tcpquic-internal-test-ca` | 自签名根 | — |
| Server | `tcpquic-server` | `serverAuth` | `localhost`、`tcpquic-server`、`127.0.0.1`、`169.254.250.230`、`169.254.59.196` |
| Client | `tcpquic-client` | `clientAuth` | `localhost`、`tcpquic-client`、`127.0.0.1` |

算法：RSA 2048，签名 SHA-256。

## 启动示例

仓库根目录下：

```bash
CERT="$PWD/cert"

# B 节点 — server
./build/bin/Release/tcpquic-proxy server \
  --quic-listen 0.0.0.0:4433 \
  --allow-targets 0.0.0.0/0 \
  --quic-cert "$CERT/server/server.crt" \
  --quic-key  "$CERT/server/server.key" \
  --quic-ca   "$CERT/ca.crt"

# A 节点 — client
./build/bin/Release/tcpquic-proxy client \
  --quic-peer <server-host>:4433 \
  --http-listen 127.0.0.1:8080 \
  --socks-listen 127.0.0.1:1080 \
  --quic-cert "$CERT/client/client.crt" \
  --quic-key  "$CERT/client/client.key" \
  --quic-ca   "$CERT/ca.crt"
```

Windows 上将 `./build/bin/Release/tcpquic-proxy` 换为 `.\build-x64\bin\Release\tcpquic-proxy.exe`（或你的构建输出路径），`$PWD/cert` 换为 `.\cert`。

### Windows Schannel 补充说明

使用 msquic Schannel 时，除 PEM 参数外还需将 `ca.crt` 导入当前用户 **受信任的根证书颁发机构**；server/client 叶子证书须含 `serverAuth` / `clientAuth` EKU（本目录已配置）。详见仓库根 `README.md` Windows 章节。

## 与临时测试证书的区别

`scripts/test-tcpquic-proxy.sh` 等脚本在 `/tmp` 生成 **1 天有效** 临时证书，与本目录互不影响。`cert/` 用于长期联调、手工启动与固定路径文档示例。
