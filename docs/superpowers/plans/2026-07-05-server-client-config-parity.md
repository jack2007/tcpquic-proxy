# Server Client Config Parity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 server 配置文件规则对齐 client 当前实际行为：可使用默认运行时路径，可在缺失时生成配置文件，已存在但非法时启动失败。

**Architecture:** 在 `src/config/config.cpp` 中抽出 role-aware 默认配置路径和 runtime config JSON 生成函数，复用 admin token 默认目录规则的运行时目录选择。`TqParseArgs()` 先尝试读取显式 `--config`；文件缺失时保留 CLI/default 配置并在最终校验后写出配置文件。client 保持现有 legacy `ClientConfigPath` 行为不破坏，同时 server 增加与 client 持久化体验一致的 `ConfigPath`。

**Tech Stack:** C++17、`std::filesystem`、`nlohmann::json`、现有 `src/unittest/config_router_test.cpp`。

---

## 文件结构

- Modify: `src/config/config.cpp`
  - 增加默认 runtime config 路径生成：`client-config-<pid>.json` / `server-config-<pid>.json`，目录规则与 admin token 默认路径一致。
  - 增加 runtime config JSON 序列化，server 只生成 server 可用字段，client 生成当前推荐 runtime schema。
  - 调整 `TqParseArgs()` 的 `--config` 缺失处理：缺失不立即失败，最终校验通过后写文件。
- Modify: `src/config/config.h`
  - 如测试需要直接调用 helper，可声明最小内部接口；首选保持 helper 在 `.cpp` 匿名 namespace 中。
- Modify: `src/unittest/config_router_test.cpp`
  - 新增 server 默认配置路径、显式缺失 config 生成、已存在非法 config 报错、缺少必填项不生成文件等测试。
  - 保留现有 client 行为测试，确认本次不回退 legacy client semantics。
- Modify: `docs/config_guide_cn.md`
  - 记录 server 对齐 client 的配置文件生命周期规则。

---

### Task 1: 用测试锁定 server 配置文件生成规则

**Files:**
- Modify: `src/unittest/config_router_test.cpp`

- [ ] **Step 1: 添加文件读取辅助函数**

在 `src/unittest/config_router_test.cpp` 的现有 temp config helper 附近添加：

```cpp
static std::string ReadTextFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
```

- [ ] **Step 2: 添加显式缺失 `--config` 生成 server 配置测试**

在现有 server `--config` 解析测试附近添加：

```cpp
{
    const std::string file = TempConfigPath("tcpquic-missing-server-runtime-config");
    std::filesystem::remove(file);
    const char* args[] = {
        "tcpquic-proxy",
        "server",
        "--config",
        file.c_str(),
        "--listen",
        "0.0.0.0:4433",
        "--cert",
        "server.crt",
        "--key",
        "server.key",
        "--allow-targets",
        "127.0.0.1/32",
        "--admin-listen",
        "127.0.0.1:19092",
        "--admin-threads",
        "3",
        "--compress",
        "zstd",
        "--compress-level",
        "2",
        "--trace-interval",
        "7"
    };
    TqConfig cfg;
    std::string err;
    if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) {
        std::fprintf(stderr, "missing server --config should generate config: %s\n", err.c_str());
        return 340;
    }
    if (cfg.ConfigPath != file) return 341;
    if (!std::filesystem::exists(file)) return 342;

    const std::string body = ReadTextFile(file);
    nlohmann::json root = nlohmann::json::parse(body);
    if (root["tls"]["cert"] != "server.crt") return 343;
    if (root["tls"]["key"] != "server.key") return 344;
    if (root["server"]["proto_listen"] != "0.0.0.0:4433") return 345;
    if (root["server"]["allow_targets"] != nlohmann::json::array({"127.0.0.1/32"})) return 346;
    if (root["admin"]["listen"] != "127.0.0.1:19092") return 347;
    if (root["admin"]["threads"] != 3) return 348;
    if (root["compression"]["mode"] != "zstd") return 349;
    if (root["compression"]["level"] != 2) return 350;
    if (root["trace"]["interval_sec"] != 7) return 351;
    if (root.contains("peers")) return 352;
    std::filesystem::remove(file);
}
```

- [ ] **Step 3: 添加缺少 server 必填项时不生成文件测试**

继续添加：

```cpp
{
    const std::string file = TempConfigPath("tcpquic-missing-server-runtime-config-required");
    std::filesystem::remove(file);
    const char* args[] = {
        "tcpquic-proxy",
        "server",
        "--config",
        file.c_str(),
        "--listen",
        "0.0.0.0:4433"
    };
    TqConfig cfg;
    std::string err;
    if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 353;
    if (err.find("--cert") == std::string::npos) return 354;
    if (std::filesystem::exists(file)) return 355;
}
```

- [ ] **Step 4: 添加未指定 `--config` 时 server 默认路径测试**

继续添加：

```cpp
{
    const char* args[] = {
        "/tmp/tcpquic-proxy-test-bin",
        "server",
        "--listen",
        "0.0.0.0:4433",
        "--cert",
        "server.crt",
        "--key",
        "server.key"
    };
    TqConfig cfg;
    std::string err;
    if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) {
        std::fprintf(stderr, "default server config path parse failed: %s\n", err.c_str());
        return 356;
    }
    if (cfg.ConfigPath.empty()) return 357;
    if (cfg.ConfigPath.find("tcpquic-proxy-test-bin") == std::string::npos) return 358;
    if (cfg.ConfigPath.find("server-config-") == std::string::npos) return 359;
    if (!std::filesystem::exists(cfg.ConfigPath)) return 360;
    std::filesystem::remove(cfg.ConfigPath);
}
```

- [ ] **Step 5: 添加已存在非法 `--config` 直接报错且不覆盖测试**

继续添加：

```cpp
{
    const std::string file = WriteTempConfig(R"json({"tls":{"cert":"server.crt"}})json");
    const std::string before = ReadTextFile(file);
    const char* args[] = {"tcpquic-proxy", "server", "--config", file.c_str()};
    TqConfig cfg;
    std::string err;
    if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 361;
    if (err.find("--listen") == std::string::npos) return 362;
    if (ReadTextFile(file) != before) return 363;
    std::filesystem::remove(file);
}
```

- [ ] **Step 6: 运行测试确认失败**

Run:

```bash
rtk cmake --build build --target tcpquic_config_router_test -j2
rtk ./build/bin/Release/tcpquic_config_router_test
```

Expected: 编译通过；测试失败在新增用例，原因是 server 缺失 `--config` 当前不会生成，未指定 `--config` 当前 `ConfigPath` 为空。

---

### Task 2: 实现默认 runtime config 路径

**Files:**
- Modify: `src/config/config.cpp`

- [ ] **Step 1: 替换/泛化默认路径 helper**

把现有 `DefaultClientConfigPath(const char* argv0)` 替换为 role-aware helper：

```cpp
std::filesystem::path DefaultRuntimeBaseDir(const char* argv0) {
    const std::string runtimeName = BaseNameFromArgv0(argv0);
    std::filesystem::path base;
#if defined(_WIN32)
    char localAppData[MAX_PATH]{};
    const DWORD envLen = GetEnvironmentVariableA("LOCALAPPDATA", localAppData, MAX_PATH);
    base = envLen > 0 && envLen < MAX_PATH
        ? std::filesystem::path(localAppData) / runtimeName
        : std::filesystem::temp_directory_path() / runtimeName;
#else
    const char* runtimeDir = std::getenv("XDG_RUNTIME_DIR");
    if (runtimeDir != nullptr && runtimeDir[0] != '\0') {
        base = std::filesystem::path(runtimeDir) / runtimeName;
    } else {
        base = std::filesystem::temp_directory_path() /
            (runtimeName + "-" + std::to_string(static_cast<unsigned long>(::getuid())));
    }
#endif
    return base;
}

std::string DefaultRoleConfigPath(const char* argv0, TqMode mode) {
    const char* role = mode == TqMode::Client ? "client" : "server";
    return (DefaultRuntimeBaseDir(argv0) /
        (std::string(role) + "-config-" + std::to_string(CurrentPid()) + ".json")).string();
}
```

- [ ] **Step 2: 更新 legacy client 默认路径调用**

把：

```cpp
cfg.ClientConfigPath = DefaultClientConfigPath(argc > 0 ? argv[0] : nullptr);
```

替换为：

```cpp
cfg.ClientConfigPath = DefaultRoleConfigPath(argc > 0 ? argv[0] : nullptr, TqMode::Client);
```

- [ ] **Step 3: 为 server 未指定 `--config` 设置默认路径**

在最终 server 分支校验完成后、返回 `true` 前设置：

```cpp
if (cfg.Mode == TqMode::Server && cfg.ConfigPath.empty()) {
    cfg.ConfigPath = DefaultRoleConfigPath(argc > 0 ? argv[0] : nullptr, TqMode::Server);
}
```

注意：这一步先只设置路径，写文件在 Task 3 处理。

- [ ] **Step 4: 运行测试确认仍失败但路径相关断言推进**

Run:

```bash
rtk cmake --build build --target tcpquic_config_router_test -j2
rtk ./build/bin/Release/tcpquic_config_router_test
```

Expected: server default path 不再为空；仍因文件未生成而失败。

---

### Task 3: 实现 runtime config JSON 生成和缺失文件写出

**Files:**
- Modify: `src/config/config.cpp`

- [ ] **Step 1: 增加 JSON array helper**

在匿名 namespace 中添加：

```cpp
nlohmann::json StringArrayJson(const std::vector<std::string>& values) {
    nlohmann::json out = nlohmann::json::array();
    for (const auto& value : values) {
        out.push_back(value);
    }
    return out;
}
```

- [ ] **Step 2: 增加 server runtime config JSON 序列化**

添加：

```cpp
std::string RuntimeConfigJson(const TqConfig& cfg) {
    nlohmann::json root;
    root["tls"] = nlohmann::json::object();
    if (!cfg.QuicCert.empty()) root["tls"]["cert"] = cfg.QuicCert;
    if (!cfg.QuicKey.empty()) root["tls"]["key"] = cfg.QuicKey;
    if (!cfg.QuicCa.empty()) root["tls"]["ca"] = cfg.QuicCa;

    if (!cfg.AdminListen.empty() || !cfg.AdminTokenFile.empty() || cfg.AdminThreads != 2) {
        root["admin"] = nlohmann::json::object();
        if (!cfg.AdminListen.empty()) root["admin"]["listen"] = cfg.AdminListen;
        if (!cfg.AdminTokenFile.empty()) root["admin"]["token_file"] = cfg.AdminTokenFile;
        root["admin"]["threads"] = cfg.AdminThreads;
    }

    root["proto"] = {
        {"profile", cfg.QuicProfile == TqQuicProfile::LowLatency ? "low-latency" : "max-throughput"},
        {"disable_1rtt_encryption", cfg.QuicDisable1RttEncryption},
        {"connection_stream_count", cfg.QuicConnectionStreamCount},
        {"keepalive_ms", cfg.QuicKeepAliveIntervalMs},
    };
    if (cfg.Mode == TqMode::Client) {
        root["proto"]["connections"] = cfg.QuicConnections;
    }
    if (cfg.TuningOverrideQuicIw != 0) root["proto"]["iw"] = cfg.TuningOverrideQuicIw;
    if (cfg.TuningOverrideQuicInitRttMs != 0) root["proto"]["initrtt_ms"] = cfg.TuningOverrideQuicInitRttMs;

    root["compression"] = {
        {"mode", cfg.Compress},
        {"level", cfg.CompressLevel},
    };

    root["tuning"] = {
        {"mode", cfg.TuningMode == TqTuningMode::Auto ? "auto" :
            (cfg.TuningMode == TqTuningMode::Lan ? "lan" : "wan")},
    };

    root["relay"] = nlohmann::json::object();
    if (cfg.TuningOverrideRelayIoSize != 0) root["relay"]["io_size"] = cfg.TuningOverrideRelayIoSize;
    nlohmann::json linuxRelay = nlohmann::json::object();
    if (cfg.TuningOverrideLinuxRelayReadChunkSize != 0) linuxRelay["read_chunk_size"] = cfg.TuningOverrideLinuxRelayReadChunkSize;
    if (cfg.TuningOverrideLinuxRelayTcpWriteMaxBytes != 0) linuxRelay["tcp_write_max_bytes"] = cfg.TuningOverrideLinuxRelayTcpWriteMaxBytes;
    if (cfg.TuningOverrideLinuxRelayTcpWriteBurstBytes != 0) linuxRelay["tcp_write_burst_bytes"] = cfg.TuningOverrideLinuxRelayTcpWriteBurstBytes;
    if (cfg.TuningOverrideLinuxRelayEventQueueCapacity != 0) linuxRelay["event_queue_capacity"] = cfg.TuningOverrideLinuxRelayEventQueueCapacity;
    if (cfg.TuningOverrideLinuxRelayWorkerCount != 0) linuxRelay["worker_count"] = cfg.TuningOverrideLinuxRelayWorkerCount;
    if (!linuxRelay.empty()) root["relay"]["linux"] = std::move(linuxRelay);
    nlohmann::json windowsRelay = nlohmann::json::object();
    if (cfg.TuningOverrideWindowsRelayWorkerCount != 0) windowsRelay["worker_count"] = cfg.TuningOverrideWindowsRelayWorkerCount;
    if (!windowsRelay.empty()) root["relay"]["windows"] = std::move(windowsRelay);
    if (root["relay"].empty()) root.erase("relay");

    root["trace"] = {
        {"enabled", cfg.Trace},
        {"interval_sec", cfg.TraceIntervalSec},
        {"level", cfg.TraceLogLevel == TqConfig::TraceLevel::Debug ? "debug" : "info"},
    };

    if (cfg.Mode == TqMode::Server) {
        root["server"] = {
            {"proto_listen", cfg.QuicListen},
            {"allow_targets", StringArrayJson(cfg.AllowTargets)},
            {"deny_targets", StringArrayJson(cfg.DenyTargets)},
        };
    }

    return root.dump(2) + "\n";
}
```

- [ ] **Step 3: 增加原子写文件 helper**

添加：

```cpp
bool WriteTextFileAtomically(const std::string& path, const std::string& body, std::string& err) {
    std::error_code ec;
    const std::filesystem::path target(path);
    const std::filesystem::path parent = target.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            err = "failed to create config directory: " + ec.message();
            return false;
        }
    }

    const std::filesystem::path tmp = target.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            err = "failed to open config temp file: " + tmp.string();
            return false;
        }
        out << body;
        out.close();
        if (!out) {
            err = "failed to write config temp file: " + tmp.string();
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }

    std::filesystem::rename(tmp, target, ec);
    if (ec) {
        err = "failed to publish config file: " + ec.message();
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}
```

- [ ] **Step 4: 记录显式 `--config` 文件是否存在**

在 `TqParseArgs()` 中新增局部变量：

```cpp
bool configSpecified = false;
bool configFileMissing = false;
```

在第一次扫描 `--config` 时：

```cpp
configSpecified = true;
cfg.ConfigPath = value;
if (std::filesystem::exists(cfg.ConfigPath)) {
    if (!LoadRuntimeConfigFile(cfg.ConfigPath, cfg, err)) {
        return false;
    }
} else {
    configFileMissing = true;
}
```

如果路径存在但打不开，`LoadRuntimeConfigFile()` 会报错；如果不存在，不立即报错。

- [ ] **Step 5: 最终校验后写出 server config**

在 server 分支校验通过后、函数返回前添加：

```cpp
if (cfg.Mode == TqMode::Server) {
    if (cfg.ConfigPath.empty()) {
        cfg.ConfigPath = DefaultRoleConfigPath(argc > 0 ? argv[0] : nullptr, TqMode::Server);
        configFileMissing = !std::filesystem::exists(cfg.ConfigPath);
    }
    if (configFileMissing || !configSpecified) {
        if (!WriteTextFileAtomically(cfg.ConfigPath, RuntimeConfigJson(cfg), err)) {
            return false;
        }
    }
}
```

注意：如果 `--config` 指向已存在文件，绝不写回；如果最终校验失败，执行不到这里，因此不会生成无效配置文件。

- [ ] **Step 6: 运行 config router 测试**

Run:

```bash
rtk cmake --build build --target tcpquic_config_router_test -j2
rtk ./build/bin/Release/tcpquic_config_router_test
```

Expected: `tcpquic_config_router_test` PASS。

---

### Task 4: 文档同步

**Files:**
- Modify: `docs/config_guide_cn.md`

- [ ] **Step 1: 更新配置文件生命周期说明**

在开头 `--config` 使用说明后补充：

```markdown
配置文件生命周期规则：

1. `--config <path>` 指定且文件存在时，程序直接读取该文件；JSON 格式或字段 schema 不符合当前模式要求时启动失败，不修复、不覆盖。
2. `--config <path>` 指定但文件不存在时，server 会按当前 CLI 参数和默认值生成该配置文件，然后继续启动。生成前仍会校验必填项，例如 `server` 模式必须提供 `--listen`、`--cert`、`--key`。
3. 未指定 `--config` 时，server 使用与 Admin token 默认路径一致的运行时目录规则生成 `server-config-<pid>.json`，并以该文件作为后续 Admin ACL 写回目标。
4. client 当前仍保留 legacy router 配置持久化行为：未指定 `--client-config` 时会使用默认 `client-config-<pid>.json`；Admin/router 配置变更会写回该文件。
```

- [ ] **Step 2: 运行文档 grep 检查**

Run:

```bash
rtk rg -n "配置文件生命周期|server-config|client-config" docs/config_guide_cn.md
```

Expected: 能看到新增 lifecycle 说明。

---

### Task 5: 全量相关验证

**Files:**
- No code changes.

- [ ] **Step 1: 构建相关测试**

Run:

```bash
rtk cmake --build build --target tcpquic_config_router_test tcpquic_runtime_config_file_store_test tcpquic_server_admin_test -j2
```

Expected: build succeeds.

- [ ] **Step 2: 运行相关测试**

Run:

```bash
rtk ./build/bin/Release/tcpquic_config_router_test
rtk ./build/bin/Release/tcpquic_runtime_config_file_store_test
rtk ./build/bin/Release/tcpquic_server_admin_test
```

Expected: all commands exit 0.

- [ ] **Step 3: 检查工作区变更**

Run:

```bash
rtk git diff -- src/config/config.cpp src/config/config.h src/unittest/config_router_test.cpp docs/config_guide_cn.md
rtk git status --short
```

Expected: diff 只包含 server/client config parity 相关变更；已有用户改动不被回退。

---

## Self-Review

- Spec coverage: 覆盖了 server 对齐 client 的默认路径、缺失文件生成、已存在非法文件报错、必填项缺失不生成、文档说明。
- Placeholder scan: 没有 TBD/TODO/fill in later。
- Type consistency: 使用现有 `TqConfig`、`TqMode`、`TqQuicProfile`、`TqTuningMode`、`nlohmann::json`，字段名与 `JsonParser::ParseRuntimeConfig()` 当前 schema 一致。
