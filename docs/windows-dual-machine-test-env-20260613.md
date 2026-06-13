# Windows 双机测试环境准备

> 日期：2026-06-13  
> 适用场景：Windows 平台下进行 tcpquic-proxy 双机吞吐、延迟、ETW/WPR 性能采集与自动化验证。Linux 双机测试通常可通过 SSH 免密完成；Windows 推荐同样启用 OpenSSH Server，以便复用“控制机远程执行 + 分发二进制 + 拉取日志/trace”的测试模型。

## 1. 推荐方案：Windows OpenSSH 免密

Windows 10/11 和 Windows Server 均支持 OpenSSH Server。该方案最接近 Linux 下的 SSH 免密流程，也最方便与现有自动化脚本对齐。

### 1.1 在两台 Windows 测试机启用 OpenSSH Server

在每台机器使用管理员 PowerShell 执行：

```powershell
Get-WindowsCapability -Online | Where-Object Name -like 'OpenSSH.Server*'
Add-WindowsCapability -Online -Name OpenSSH.Server~~~~0.0.1.0

Start-Service sshd
Set-Service -Name sshd -StartupType Automatic
```

放通 SSH 防火墙端口：

```powershell
New-NetFirewallRule `
  -Name sshd `
  -DisplayName "OpenSSH Server (sshd)" `
  -Enabled True `
  -Direction Inbound `
  -Protocol TCP `
  -Action Allow `
  -LocalPort 22
```

验证 SSH 登录：

```powershell
ssh username@windows-host
```

### 1.2 配置 SSH key 免密

在控制机生成测试专用 key：

```powershell
ssh-keygen -t ed25519 -f $env:USERPROFILE\.ssh\tcpquic_test_ed25519
```

将公钥内容追加到目标机测试用户的：

```text
C:\Users\<target-user>\.ssh\authorized_keys
```

如果目标用户属于 Administrators 组，Windows OpenSSH 默认还可能要求使用：

```text
C:\ProgramData\ssh\administrators_authorized_keys
```

并设置 ACL：

```powershell
icacls C:\ProgramData\ssh\administrators_authorized_keys /inheritance:r
icacls C:\ProgramData\ssh\administrators_authorized_keys /grant "Administrators:F" /grant "SYSTEM:F"
```

控制机可配置 `~\.ssh\config`：

```sshconfig
Host tq-win-a
    HostName 192.168.1.10
    User tcpquic-test
    IdentityFile ~/.ssh/tcpquic_test_ed25519

Host tq-win-b
    HostName 192.168.1.11
    User tcpquic-test
    IdentityFile ~/.ssh/tcpquic_test_ed25519
```

验证远程执行和文件分发：

```powershell
ssh tq-win-a hostname
ssh tq-win-b hostname
scp .\build-win\bin\Release\tcpquic.exe tq-win-a:C:/tcpquic-test/bin/
```

## 2. 测试端口与防火墙

双机测试至少需要放通：

- SSH 端口：TCP 22。
- tcpquic-proxy 监听端口：通常包含 TCP 端口和 QUIC/UDP 端口。
- 后端 echo/server 端口。
- 管理或 metrics 端口，如果测试会读取 admin HTTP / metrics。
- ETW trace、日志回传依赖 SSH/SCP，无需额外端口。

示例：

```powershell
New-NetFirewallRule -DisplayName "tcpquic test TCP" -Direction Inbound -Action Allow -Protocol TCP -LocalPort 10000-10100
New-NetFirewallRule -DisplayName "tcpquic test UDP" -Direction Inbound -Action Allow -Protocol UDP -LocalPort 10000-10100
```

如测试网络有安全要求，建议把规则限制到测试网段或指定远端 IP。

## 3. 测试用户与权限

建议创建专门的测试用户，例如 `tcpquic-test`：

```powershell
New-LocalUser -Name tcpquic-test -Password (Read-Host -AsSecureString "Password")
Add-LocalGroupMember -Group "Users" -Member tcpquic-test
```

权限建议：

- 普通测试进程优先使用普通用户运行。
- 初始化防火墙、安装 OpenSSH、采集部分 ETW/WPR profile 时使用管理员 PowerShell。
- 如果必须用管理员账号 SSH，建议仅允许测试网段访问 TCP 22，并避免复用个人账号。

## 4. 统一运行目录

建议两台机器使用一致目录结构，便于脚本化部署和采集：

```text
C:\tcpquic-test\
  bin\
  config\
  logs\
  traces\
  scripts\
```

初始化目录：

```powershell
ssh tq-win-a "New-Item -ItemType Directory -Force C:\tcpquic-test\bin, C:\tcpquic-test\config, C:\tcpquic-test\logs, C:\tcpquic-test\traces, C:\tcpquic-test\scripts"
ssh tq-win-b "New-Item -ItemType Directory -Force C:\tcpquic-test\bin, C:\tcpquic-test\config, C:\tcpquic-test\logs, C:\tcpquic-test\traces, C:\tcpquic-test\scripts"
```

分发二进制和配置：

```powershell
scp .\build-win\bin\Release\*.exe tq-win-a:C:/tcpquic-test/bin/
scp .\build-win\bin\Release\*.exe tq-win-b:C:/tcpquic-test/bin/
scp .\config\*.json tq-win-a:C:/tcpquic-test/config/
scp .\config\*.json tq-win-b:C:/tcpquic-test/config/
```

如果程序依赖额外 DLL，需要一并分发到 `bin`，或确保测试机已经安装相应运行时。

## 5. ETW / WPR 性能采集准备

Windows 性能验证建议提前确认 Windows Performance Recorder 可用：

```powershell
wpr -help
```

如果不可用，需要安装 Windows ADK 中的 Windows Performance Toolkit。

典型采集流程：

```powershell
wpr -start CPU -start Network -filemode
# 运行双机测试
wpr -stop C:\tcpquic-test\traces\trace.etl
```

回传 trace：

```powershell
scp tq-win-a:C:/tcpquic-test/traces/trace.etl .\artifacts\tq-win-a-trace.etl
scp tq-win-b:C:/tcpquic-test/traces/trace.etl .\artifacts\tq-win-b-trace.etl
```

建议每次测试同时保存：

- `tcpquic` 日志。
- 测试配置文件。
- ETW/WPR trace。
- `Get-NetAdapter` / `Get-NetIPConfiguration` 输出。
- 测试命令和参数。

## 6. 时间同步与网络检查

双机吞吐与日志对齐依赖时间同步和稳定网络。建议测试前检查：

```powershell
w32tm /query /status
ping <peer-host>
Test-NetConnection <peer-host> -Port 22
```

记录网卡、IP、速率与 MTU：

```powershell
Get-NetAdapter
Get-NetIPConfiguration
netsh interface ipv4 show subinterfaces
```

建议：

- 使用固定 IP 或 DHCP reservation。
- 确认测试流量走目标网卡。
- 关闭不相关 VPN、代理、流量整形工具。
- 如需要对比 Linux 结果，记录 CPU 电源策略、网卡 offload、MTU、RSS 等环境信息。

## 7. 可选方案：PowerShell Remoting / WinRM

如果环境不允许 SSH，也可以使用 PowerShell Remoting：

```powershell
Enable-PSRemoting -Force
```

远程执行示例：

```powershell
Invoke-Command -ComputerName win-a -ScriptBlock { hostname }
```

但 WinRM 在跨域、工作组、证书、TrustedHosts 配置上通常比 OpenSSH 更复杂。对于 tcpquic-proxy 双机性能测试，优先推荐 OpenSSH。

## 8. 最小验收清单

Windows 双机测试环境准备完成前，建议确认以下项目：

- [ ] 两台测试机均可通过 SSH key 免密登录。
- [ ] 控制机可通过 `scp` 向两台机器分发二进制、配置，并拉取日志和 trace。
- [ ] TCP/UDP 测试端口已按测试范围放通。
- [ ] 两台机器运行目录结构一致。
- [ ] Visual C++ runtime 和程序依赖 DLL 可用。
- [ ] ETW/WPR 可采集并能回传 `.etl` 文件。
- [ ] 时间同步正常。
- [ ] 测试用户权限明确，管理员权限仅用于初始化和采集。
- [ ] 网络路径、网卡、MTU、IP、端口范围已记录。
- [ ] 防火墙规则限制在测试所需范围内。

## 9. 结论

Windows 下的双机测试可以通过 OpenSSH Server 建立与 Linux 类似的免密远程执行环境。推荐把环境标准化为：SSH 免密、SCP 分发与回收、统一运行目录、防火墙端口白名单、ETW/WPR 采集、时间同步和网络参数记录。这样后续 Windows 性能测试脚本可以尽量复用 Linux 双机测试的控制流程，只在命令、路径和性能采集工具上做平台差异适配。
