# Server Admin Console Config Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 server admin-console 支持修改 ACL，修改后立即影响新建 tunnel，并把配置持久化写回 server JSON 配置文件。

**Architecture:** 先抽出 server 配置文件写回组件，使用 `nlohmann::json` 做 JSON AST 读改写和原子写入；再引入 server runtime config state，作为 admin handler、配置文件和 dial reactor 之间的一致性边界。最后新增 `PATCH /api/v1/server/config`，按“构造 next config/ACL -> 写配置文件 -> 更新 dial reactor -> 提交 runtime state”的顺序应用，并更新 admin-console ACL 页面。

**Tech Stack:** C++17、CMake、`nlohmann::json`、cpp-httplib admin server、现有 `TqAcl` / `TqServerDialReactor` / `TqServerAdmin` 测试体系。

---

## 功能开发顺序

1. **配置文件写回基础设施**：先做可独立测试的 JSON 文件读改写和原子写回，不接入 server runtime。
2. **server runtime config state**：建立当前生效配置和 ACL 快照，保证 patch 可以先构造 next state，再提交。
3. **dial reactor ACL 热更新**：让新请求使用最新 ACL，确认 pending 语义和并发安全。
4. **Admin API 接入**：新增 `PATCH /server/config`，串起 state、文件写回、reactor 更新和错误响应。
5. **Admin HTTP 路由和 console**：开放 `/api/v1/server/config` PATCH，改造 ACL 页面为可编辑表单。
6. **文档和端到端验证**：更新 API 文档，跑目标测试和短连接热更新验证。

## 文件结构

- Create: `src/runtime/runtime_config_file_store.h`
  配置文件持久化接口，隐藏 JSON 读写、临时文件和原子替换细节。
- Create: `src/runtime/runtime_config_file_store.cpp`
  使用 `nlohmann::json` 实现 server ACL 写回，禁止手工拼接 JSON。
- Create: `src/runtime/server_runtime_config.h`
  server 当前生效配置状态、ACL patch parser、快照和提交接口。
- Create: `src/runtime/server_runtime_config.cpp`
  CIDR 校验、ACL next state 构造、运行期状态提交。
- Modify: `src/tunnel/server_dial_reactor.h`
  增加 `UpdateAcl(TqAcl acl)`。
- Modify: `src/tunnel/server_dial_reactor.cpp`
  在 reactor 锁内替换 ACL，并让 `OnResolved()` 使用当前 ACL 快照。
- Modify: `src/runtime/server_admin.h`
  新增接收 server runtime state、file store、dial reactor updater 的 handler overload。
- Modify: `src/runtime/server_admin.cpp`
  新增 `PATCH /server/config`，接入 state/file/reactor 应用顺序。
- Modify: `src/runtime/admin_http.cpp`
  放行 `/api/v1/server/config` 的 `PATCH`。
- Modify: `src/runtime/admin_console.cpp`
  server ACL 页面从只读摘要改为可编辑表单和保存状态。
- Modify: `src/main.cpp`
  server 启动时创建 runtime state/file store，并传入 admin handler。
- Modify: `src/CMakeLists.txt`
  把新增 runtime 源文件加入主程序和相关测试 target；新增文件 store 单元测试 target。
- Create: `src/unittest/runtime_config_file_store_test.cpp`
  覆盖 JSON AST 写回、保留其他字段、原子写失败、非法 JSON 等。
- Modify: `src/unittest/server_admin_test.cpp`
  覆盖 server config PATCH 成功、失败、无 config path、文件写回和 state 提交。
- Modify: `src/unittest/server_dial_reactor_test.cpp`
  覆盖 `UpdateAcl()` 后新请求使用新 ACL。
- Modify: `src/unittest/admin_http_test.cpp`
  覆盖 PATCH path whitelist 和 console 静态结构。
- Modify: `docs/admin-api/interface.md`、`docs/config_guide_cn.md`
  记录 server ACL 热更新和配置文件写回语义。

---

### Task 1: 配置文件写回组件

**Files:**
- Create: `src/runtime/runtime_config_file_store.h`
- Create: `src/runtime/runtime_config_file_store.cpp`
- Create: `src/unittest/runtime_config_file_store_test.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: 写失败测试，验证 JSON AST 写回 server ACL**

Create `src/unittest/runtime_config_file_store_test.cpp`:

```cpp
#include "runtime_config_file_store.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::filesystem::path TempPath(const std::string& name) {
    static unsigned counter = 0;
    return std::filesystem::temp_directory_path() /
        ("tcpquic-runtime-config-file-store-" + name + "-" + std::to_string(counter++) + ".json");
}

void WriteText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

nlohmann::json ReadJson(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    nlohmann::json root;
    in >> root;
    return root;
}

int TestPatchServerAclPreservesOtherFields() {
    const auto path = TempPath("preserve");
    WriteText(path, R"({
      "tls": {"cert": "certs/server.crt", "key": "certs/server.key"},
      "server": {
        "proto_listen": "0.0.0.0:4433",
        "allow_targets": ["10.0.0.0/8"],
        "deny_targets": []
      },
      "proto": {"profile": "max-throughput"}
    })");

    TqRuntimeConfigFileStore store(path.string());
    std::string err;
    if (!store.PatchServerAcl({"127.0.0.1/32"}, {"169.254.0.0/16"}, err)) return 1;

    const nlohmann::json root = ReadJson(path);
    if (root["server"]["allow_targets"] != nlohmann::json::array({"127.0.0.1/32"})) return 2;
    if (root["server"]["deny_targets"] != nlohmann::json::array({"169.254.0.0/16"})) return 3;
    if (root["server"]["proto_listen"] != "0.0.0.0:4433") return 4;
    if (root["tls"]["cert"] != "certs/server.crt") return 5;
    if (root["proto"]["profile"] != "max-throughput") return 6;
    std::filesystem::remove(path);
    return 0;
}

int TestPatchServerAclCreatesServerObject() {
    const auto path = TempPath("create-server");
    WriteText(path, R"({"tls":{"cert":"server.crt"}})");

    TqRuntimeConfigFileStore store(path.string());
    std::string err;
    if (!store.PatchServerAcl({"0.0.0.0/0"}, {}, err)) return 10;

    const nlohmann::json root = ReadJson(path);
    if (!root.contains("server") || !root["server"].is_object()) return 11;
    if (root["server"]["allow_targets"] != nlohmann::json::array({"0.0.0.0/0"})) return 12;
    if (root["server"]["deny_targets"] != nlohmann::json::array()) return 13;
    std::filesystem::remove(path);
    return 0;
}

int TestRejectsMalformedJsonWithoutChangingFile() {
    const auto path = TempPath("malformed");
    const std::string original = R"({"server":)";
    WriteText(path, original);

    TqRuntimeConfigFileStore store(path.string());
    std::string err;
    if (store.PatchServerAcl({"127.0.0.1/32"}, {}, err)) return 20;

    std::ifstream in(path, std::ios::binary);
    const std::string actual((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (actual != original) return 21;
    if (err.find("parse") == std::string::npos && err.find("JSON") == std::string::npos) return 22;
    std::filesystem::remove(path);
    return 0;
}

int TestRejectsNonObjectRootWithoutChangingFile() {
    const auto path = TempPath("array-root");
    const std::string original = R"(["not-object"])";
    WriteText(path, original);

    TqRuntimeConfigFileStore store(path.string());
    std::string err;
    if (store.PatchServerAcl({"127.0.0.1/32"}, {}, err)) return 30;

    std::ifstream in(path, std::ios::binary);
    const std::string actual((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (actual != original) return 31;
    if (err.find("object") == std::string::npos) return 32;
    std::filesystem::remove(path);
    return 0;
}

} // namespace

int main() {
    if (int code = TestPatchServerAclPreservesOtherFields()) return code;
    if (int code = TestPatchServerAclCreatesServerObject()) return code;
    if (int code = TestRejectsMalformedJsonWithoutChangingFile()) return code;
    if (int code = TestRejectsNonObjectRootWithoutChangingFile()) return code;
    return 0;
}
```

- [ ] **Step 2: 把测试 target 加到 CMake**

Modify `src/CMakeLists.txt` near other runtime/admin tests:

```cmake
add_executable(tcpquic_runtime_config_file_store_test
    unittest/runtime_config_file_store_test.cpp
    runtime/runtime_config_file_store.cpp
)
tcpquic_target_include_dirs(tcpquic_runtime_config_file_store_test)
target_link_libraries(tcpquic_runtime_config_file_store_test PRIVATE nlohmann_json::nlohmann_json)
set_property(TARGET tcpquic_runtime_config_file_store_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_runtime_config_file_store_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")
```

Append `tcpquic_runtime_config_file_store_test` to `TCPQUIC_TEST_TARGETS`.

- [ ] **Step 3: 运行测试，确认失败**

Run:

```bash
rtk cmake --build build --target tcpquic_runtime_config_file_store_test -j2
```

Expected: build fails because `runtime_config_file_store.h` does not exist.

- [ ] **Step 4: 实现最小接口**

Create `src/runtime/runtime_config_file_store.h`:

```cpp
#pragma once

#include <string>
#include <vector>

class TqRuntimeConfigFileStore {
public:
    explicit TqRuntimeConfigFileStore(std::string path);

    bool PatchServerAcl(
        const std::vector<std::string>& allowTargets,
        const std::vector<std::string>& denyTargets,
        std::string& err) const;

private:
    std::string Path;
};
```

Create `src/runtime/runtime_config_file_store.cpp`:

```cpp
#include "runtime_config_file_store.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

bool ReadJsonFile(const std::string& path, nlohmann::json& root, std::string& err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err = "failed to open config file: " + path;
        return false;
    }
    try {
        in >> root;
    } catch (const nlohmann::json::exception& ex) {
        err = std::string("failed to parse JSON config: ") + ex.what();
        return false;
    }
    if (!root.is_object()) {
        err = "config root must be a JSON object";
        return false;
    }
    return true;
}

bool WriteTextFileAtomically(const std::string& path, const std::string& body, std::string& err) {
    const std::filesystem::path target(path);
    const std::filesystem::path dir = target.parent_path().empty()
        ? std::filesystem::current_path()
        : target.parent_path();
    const std::filesystem::path temp = dir / (target.filename().string() + ".tmp");

    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            err = "failed to create temp config file: " + temp.string();
            return false;
        }
        out << body;
        out.flush();
        if (!out) {
            err = "failed to write temp config file: " + temp.string();
            std::filesystem::remove(temp);
            return false;
        }
    }

#if !defined(_WIN32)
    const int fd = ::open(temp.string().c_str(), O_RDONLY);
    if (fd >= 0) {
        (void)::fsync(fd);
        (void)::close(fd);
    }
#endif

    std::error_code ec;
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        std::filesystem::remove(target, ec);
        ec.clear();
        std::filesystem::rename(temp, target, ec);
    }
    if (ec) {
        err = "failed to replace config file: " + ec.message();
        std::filesystem::remove(temp);
        return false;
    }
    return true;
}

} // namespace

TqRuntimeConfigFileStore::TqRuntimeConfigFileStore(std::string path)
    : Path(std::move(path)) {
}

bool TqRuntimeConfigFileStore::PatchServerAcl(
    const std::vector<std::string>& allowTargets,
    const std::vector<std::string>& denyTargets,
    std::string& err) const {
    err.clear();
    if (Path.empty()) {
        err = "server config file path is empty";
        return false;
    }

    nlohmann::json root;
    if (!ReadJsonFile(Path, root, err)) {
        return false;
    }
    if (!root.contains("server") || root["server"].is_null()) {
        root["server"] = nlohmann::json::object();
    }
    if (!root["server"].is_object()) {
        err = "server config section must be a JSON object";
        return false;
    }

    root["server"]["allow_targets"] = allowTargets;
    root["server"]["deny_targets"] = denyTargets;
    return WriteTextFileAtomically(Path, root.dump(2) + "\n", err);
}
```

- [ ] **Step 5: 运行文件 store 测试**

Run:

```bash
rtk cmake --build build --target tcpquic_runtime_config_file_store_test -j2
rtk ./build/bin/Release/tcpquic_runtime_config_file_store_test
```

Expected: build succeeds and test exits 0.

- [ ] **Step 6: Commit**

```bash
rtk git add src/runtime/runtime_config_file_store.h src/runtime/runtime_config_file_store.cpp src/unittest/runtime_config_file_store_test.cpp src/CMakeLists.txt
rtk git commit -m "feat: add runtime config file store"
```

---

### Task 2: Server Runtime Config State

**Files:**
- Create: `src/runtime/server_runtime_config.h`
- Create: `src/runtime/server_runtime_config.cpp`
- Modify: `src/unittest/server_admin_test.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: 写 server runtime state 单元覆盖**

Append to `src/unittest/server_admin_test.cpp` before `main()`:

```cpp
int TestServerRuntimeConfigStateBuildsAclPatch() {
    TqConfig cfg;
    cfg.Mode = TqMode::Server;
    cfg.ConfigPath = "server.json";
    cfg.QuicListen = "0.0.0.0:4433";
    cfg.AllowTargets = {"10.0.0.0/8"};
    cfg.DenyTargets = {};

    TqServerRuntimeConfigState state(cfg);
    TqServerConfigPatch patch;
    patch.HasAllowTargets = true;
    patch.AllowTargets = {"127.0.0.1/32"};
    patch.HasDenyTargets = true;
    patch.DenyTargets = {"169.254.0.0/16"};

    TqConfig next;
    TqAcl acl;
    std::string err;
    if (!state.BuildAclPatch(patch, next, acl, err)) return 500;
    if (next.AllowTargets.size() != 1 || next.AllowTargets[0] != "127.0.0.1/32") return 501;
    if (next.DenyTargets.size() != 1 || next.DenyTargets[0] != "169.254.0.0/16") return 502;
    if (!acl.IsAllowed("127.0.0.1", 80)) return 503;
    if (acl.IsAllowed("169.254.1.1", 80)) return 504;

    state.Commit(next, acl);
    const TqConfig snapshot = state.SnapshotConfig();
    if (snapshot.AllowTargets != next.AllowTargets) return 505;
    if (snapshot.DenyTargets != next.DenyTargets) return 506;
    return 0;
}

int TestServerRuntimeConfigStateRejectsInvalidCidr() {
    TqConfig cfg;
    cfg.Mode = TqMode::Server;
    cfg.AllowTargets = {"10.0.0.0/8"};

    TqServerRuntimeConfigState state(cfg);
    TqServerConfigPatch patch;
    patch.HasAllowTargets = true;
    patch.AllowTargets = {"bad-cidr"};

    TqConfig next;
    TqAcl acl;
    std::string err;
    if (state.BuildAclPatch(patch, next, acl, err)) return 510;
    if (err.find("invalid CIDR") == std::string::npos) return 511;
    if (state.SnapshotConfig().AllowTargets != cfg.AllowTargets) return 512;
    return 0;
}
```

Add calls near the start of `main()`:

```cpp
    if (int code = TestServerRuntimeConfigStateBuildsAclPatch()) return code;
    if (int code = TestServerRuntimeConfigStateRejectsInvalidCidr()) return code;
```

Include the new header:

```cpp
#include "server_runtime_config.h"
```

- [ ] **Step 2: Run test to verify it fails**

Run:

```bash
rtk cmake --build build --target tcpquic_server_admin_test -j2
```

Expected: build fails because `server_runtime_config.h` does not exist.

- [ ] **Step 3: Implement server runtime state**

Create `src/runtime/server_runtime_config.h`:

```cpp
#pragma once

#include "acl.h"
#include "config.h"

#include <mutex>
#include <string>
#include <vector>

struct TqServerConfigPatch {
    bool HasAllowTargets{false};
    bool HasDenyTargets{false};
    std::vector<std::string> AllowTargets;
    std::vector<std::string> DenyTargets;
};

class TqServerRuntimeConfigState {
public:
    explicit TqServerRuntimeConfigState(const TqConfig& initial);

    TqConfig SnapshotConfig() const;
    TqAcl SnapshotAcl() const;
    bool BuildAclPatch(
        const TqServerConfigPatch& patch,
        TqConfig& nextConfig,
        TqAcl& nextAcl,
        std::string& err) const;
    void Commit(const TqConfig& nextConfig, const TqAcl& nextAcl);

private:
    mutable std::mutex Lock;
    TqConfig Config;
    TqAcl Acl;
};
```

Create `src/runtime/server_runtime_config.cpp`:

```cpp
#include "server_runtime_config.h"

TqServerRuntimeConfigState::TqServerRuntimeConfigState(const TqConfig& initial)
    : Config(initial) {
    Acl.AllowCidrs = initial.AllowTargets;
    Acl.DenyCidrs = initial.DenyTargets;
}

TqConfig TqServerRuntimeConfigState::SnapshotConfig() const {
    std::lock_guard<std::mutex> guard(Lock);
    return Config;
}

TqAcl TqServerRuntimeConfigState::SnapshotAcl() const {
    std::lock_guard<std::mutex> guard(Lock);
    return Acl;
}

bool TqServerRuntimeConfigState::BuildAclPatch(
    const TqServerConfigPatch& patch,
    TqConfig& nextConfig,
    TqAcl& nextAcl,
    std::string& err) const {
    {
        std::lock_guard<std::mutex> guard(Lock);
        nextConfig = Config;
    }
    if (patch.HasAllowTargets) {
        nextConfig.AllowTargets = patch.AllowTargets;
    }
    if (patch.HasDenyTargets) {
        nextConfig.DenyTargets = patch.DenyTargets;
    }

    if (!TqValidateCidrList(nextConfig.AllowTargets, err)) {
        return false;
    }
    if (!TqValidateCidrList(nextConfig.DenyTargets, err)) {
        return false;
    }

    nextAcl.AllowCidrs = nextConfig.AllowTargets;
    nextAcl.DenyCidrs = nextConfig.DenyTargets;
    err.clear();
    return true;
}

void TqServerRuntimeConfigState::Commit(const TqConfig& nextConfig, const TqAcl& nextAcl) {
    std::lock_guard<std::mutex> guard(Lock);
    Config = nextConfig;
    Acl = nextAcl;
}
```

Modify `src/CMakeLists.txt`:

- Add `runtime/server_runtime_config.cpp` to `TCPQUIC_PROXY_SOURCES`.
- Add `runtime/server_runtime_config.cpp` and `acl/acl.cpp` to `tcpquic_server_admin_test` sources if not already linked through another source list.

- [ ] **Step 4: Run server admin test**

Run:

```bash
rtk cmake --build build --target tcpquic_server_admin_test -j2
rtk ./build/bin/Release/tcpquic_server_admin_test
```

Expected: build succeeds and test exits 0.

- [ ] **Step 5: Commit**

```bash
rtk git add src/runtime/server_runtime_config.h src/runtime/server_runtime_config.cpp src/unittest/server_admin_test.cpp src/CMakeLists.txt
rtk git commit -m "feat: add server runtime config state"
```

---

### Task 3: Dial Reactor ACL 热更新

**Files:**
- Modify: `src/tunnel/server_dial_reactor.h`
- Modify: `src/tunnel/server_dial_reactor.cpp`
- Modify: `src/unittest/server_dial_reactor_test.cpp`

- [ ] **Step 1: 写失败测试，确认更新后新请求被拒绝**

Append to `src/unittest/server_dial_reactor_test.cpp`:

```cpp
int TestUpdateAclAffectsNewLiteralRequests() {
    TqSocketStartup startup;
    if (!startup.Ok()) return 800;

    ScopedSocket listener;
    uint16_t port = 0;
    if (!MakeLoopbackListener(listener, port)) return 801;

    TqServerDialReactor reactor(AllowAllAcl());
    if (!reactor.Start()) return 802;

    TqAcl denyLoopback;
    denyLoopback.AllowCidrs = {"10.0.0.0/8"};
    denyLoopback.DenyCidrs = {"127.0.0.0/8"};
    reactor.UpdateAcl(denyLoopback);

    bool completed = false;
    TqServerDialResult observed{};
    TqServerDialRequest request;
    request.Host = "127.0.0.1";
    request.Port = port;
    request.Complete = [&](const TqServerDialResult& result) {
        completed = true;
        observed = result;
    };

    if (reactor.Submit(std::move(request)) == 0) {
        reactor.Stop();
        return 803;
    }
    if (!RunUntil(completed, reactor, 3000)) {
        reactor.Stop();
        return 804;
    }
    reactor.Stop();
    if (observed.Error != TqOpenError::AclDenied) return 805;
    return 0;
}
```

Call it from `main()` after existing literal tests:

```cpp
    if (int code = TestUpdateAclAffectsNewLiteralRequests()) return code;
```

- [ ] **Step 2: Run test to verify it fails**

Run:

```bash
rtk cmake --build build --target tcpquic_server_dial_reactor_test -j2
```

Expected: build fails because `TqServerDialReactor::UpdateAcl` is not declared.

- [ ] **Step 3: Add UpdateAcl declaration**

Modify `src/tunnel/server_dial_reactor.h`:

```cpp
    void UpdateAcl(TqAcl acl);
```

Place it near `PendingCount() const`.

- [ ] **Step 4: Implement locked ACL replacement**

Modify `src/tunnel/server_dial_reactor.cpp`:

```cpp
    void UpdateAcl(TqAcl acl) {
        std::lock_guard<std::mutex> guard(Lock);
        Acl = std::move(acl);
    }
```

Add public wrapper near `PendingCount()`:

```cpp
void TqServerDialReactor::UpdateAcl(TqAcl acl) {
    State->UpdateAcl(std::move(acl));
}
```

In `OnResolved()`, keep using `Acl` while `Lock` is held. No extra lock is required because `OnResolved()` already runs under `Impl::Lock` in the reactor path.

- [ ] **Step 5: Run dial reactor tests**

Run:

```bash
rtk cmake --build build --target tcpquic_server_dial_reactor_test -j2
rtk ./build/bin/Release/tcpquic_server_dial_reactor_test
```

Expected: build succeeds and test exits 0.

- [ ] **Step 6: Commit**

```bash
rtk git add src/tunnel/server_dial_reactor.h src/tunnel/server_dial_reactor.cpp src/unittest/server_dial_reactor_test.cpp
rtk git commit -m "feat: allow server dial ACL updates"
```

---

### Task 4: Server Admin PATCH /server/config

**Files:**
- Modify: `src/runtime/server_admin.h`
- Modify: `src/runtime/server_admin.cpp`
- Modify: `src/unittest/server_admin_test.cpp`

- [ ] **Step 1: 写失败测试，覆盖 PATCH 成功并写文件**

Append helper functions to `src/unittest/server_admin_test.cpp`:

```cpp
std::filesystem::path TempServerConfigPath(const std::string& name) {
    static unsigned counter = 0;
    return std::filesystem::temp_directory_path() /
        ("tcpquic-server-admin-" + name + "-" + std::to_string(counter++) + ".json");
}

void WriteText(const std::filesystem::path& path, const std::string& body) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << body;
}
```

Add includes:

```cpp
#include "runtime_config_file_store.h"
#include "server_runtime_config.h"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
```

Append test:

```cpp
int TestPatchServerConfigUpdatesStateFileAndReactor() {
    const auto path = TempServerConfigPath("patch");
    WriteText(path, R"({
      "tls": {"cert": "server.crt", "key": "server.key"},
      "server": {"proto_listen": "0.0.0.0:4433", "allow_targets": ["10.0.0.0/8"], "deny_targets": []}
    })");

    TqConfig cfg;
    cfg.Mode = TqMode::Server;
    cfg.ConfigPath = path.string();
    cfg.QuicListen = "0.0.0.0:4433";
    cfg.AllowTargets = {"10.0.0.0/8"};
    cfg.DenyTargets = {};

    TqServerRuntimeConfigState state(cfg);
    TqRuntimeConfigFileStore store(path.string());
    TqAcl observedAcl;
    bool reactorUpdated = false;
    auto updateAcl = [&](const TqAcl& acl) {
        observedAcl = acl;
        reactorUpdated = true;
        return true;
    };

    TqServerMetrics metrics;
    metrics.ResolvedListens = {"0.0.0.0:4433"};
    const std::string response = TqHandleServerAdmin(
        Request("PATCH", "/server/config", "{\"allow_targets\":[\"127.0.0.1/32\"],\"deny_targets\":[\"169.254.0.0/16\"]}"),
        metrics,
        10,
        state,
        store,
        updateAcl);

    if (response.find("HTTP/1.1 200 OK") == std::string::npos) return 600;
    if (!reactorUpdated) return 601;
    if (!observedAcl.IsAllowed("127.0.0.1", 80)) return 602;
    if (observedAcl.IsAllowed("169.254.1.1", 80)) return 603;
    if (state.SnapshotConfig().AllowTargets != std::vector<std::string>{"127.0.0.1/32"}) return 604;

    std::ifstream in(path, std::ios::binary);
    nlohmann::json root;
    in >> root;
    if (root["server"]["allow_targets"] != nlohmann::json::array({"127.0.0.1/32"})) return 605;
    if (root["server"]["deny_targets"] != nlohmann::json::array({"169.254.0.0/16"})) return 606;
    std::filesystem::remove(path);
    return 0;
}
```

Call it in `main()`:

```cpp
    if (int code = TestPatchServerConfigUpdatesStateFileAndReactor()) return code;
```

- [ ] **Step 2: Run test to verify it fails**

Run:

```bash
rtk cmake --build build --target tcpquic_server_admin_test -j2
```

Expected: build fails because the new `TqHandleServerAdmin` overload does not exist.

- [ ] **Step 3: Define admin overload and updater type**

Modify `src/runtime/server_admin.h`:

```cpp
#include "runtime_config_file_store.h"
#include "server_runtime_config.h"

#include <functional>

using TqServerAclUpdater = std::function<bool(const TqAcl&)>;

std::string TqHandleServerAdmin(
    const TqHttpRequest& req,
    TqServerMetrics& metrics,
    uint64_t uptimeSeconds,
    TqServerRuntimeConfigState& runtimeState,
    const TqRuntimeConfigFileStore& fileStore,
    const TqServerAclUpdater& updateAcl);
```

- [ ] **Step 4: Implement PATCH parser and handler path**

Modify `src/runtime/server_admin.cpp`:

```cpp
bool ParseStringListJson(const nlohmann::json& value, std::vector<std::string>& out) {
    out.clear();
    if (value.is_array()) {
        for (const auto& item : value) {
            if (!item.is_string()) {
                return false;
            }
            out.push_back(item.get<std::string>());
        }
        return true;
    }
    if (value.is_string()) {
        std::string text = value.get<std::string>();
        size_t start = 0;
        while (start <= text.size()) {
            size_t comma = text.find(',', start);
            if (comma == std::string::npos) comma = text.size();
            std::string item = text.substr(start, comma - start);
            const size_t begin = item.find_first_not_of(" \t");
            const size_t end = item.find_last_not_of(" \t");
            if (begin != std::string::npos) {
                out.push_back(item.substr(begin, end - begin + 1));
            }
            start = comma + 1;
        }
        return true;
    }
    return false;
}

bool ParseServerConfigPatch(const std::string& body, TqServerConfigPatch& patch, std::string& err, bool& unsupported) {
    patch = {};
    unsupported = false;
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(body);
    } catch (const nlohmann::json::exception&) {
        err = "malformed server config patch object";
        return false;
    }
    if (!root.is_object()) {
        err = "server config patch must be an object";
        return false;
    }
    for (const auto& item : root.items()) {
        const std::string& key = item.key();
        if (key == "allow_targets") {
            patch.HasAllowTargets = true;
            if (!ParseStringListJson(item.value(), patch.AllowTargets)) {
                err = "invalid allow_targets";
                return false;
            }
        } else if (key == "deny_targets") {
            patch.HasDenyTargets = true;
            if (!ParseStringListJson(item.value(), patch.DenyTargets)) {
                err = "invalid deny_targets";
                return false;
            }
        } else if (key == "listen" || key == "resolved_listens" || key == "tls" ||
                   key == "quic" || key == "relay" || key == "admin") {
            unsupported = true;
            err = "runtime field " + key + " requires process restart";
            return false;
        } else {
            err = "unknown server config field: " + key;
            return false;
        }
    }
    if (!patch.HasAllowTargets && !patch.HasDenyTargets) {
        err = "server config patch must include allow_targets or deny_targets";
        return false;
    }
    return true;
}
```

In the new overload, add `PATCH /server/config` before the legacy const-config path:

```cpp
    if (req.Method == "GET" && req.Path == "/server/config") {
        std::vector<std::string> resolved;
        {
            std::lock_guard<std::mutex> guard(metrics.Lock);
            resolved = metrics.ResolvedListens;
        }
        return TqJsonResponse(200, TqServerRuntimeConfigJson(runtimeState.SnapshotConfig(), resolved, false));
    }
    if (req.Method == "PATCH" && req.Path == "/server/config") {
        TqServerConfigPatch patch;
        std::string err;
        bool unsupported = false;
        if (!ParseServerConfigPatch(req.Body, patch, err, unsupported)) {
            return TqJsonResponse(
                unsupported ? 503 : 400,
                unsupported ? TqStructuredErrorJson("not_supported", err) : ErrorJson(err));
        }

        TqConfig nextConfig;
        TqAcl nextAcl;
        if (!runtimeState.BuildAclPatch(patch, nextConfig, nextAcl, err)) {
            return TqJsonResponse(400, ErrorJson(err));
        }
        if (nextConfig.ConfigPath.empty()) {
            return TqJsonResponse(
                503,
                TqStructuredErrorJson("not_supported", "server config file path is empty"));
        }
        if (!fileStore.PatchServerAcl(nextConfig.AllowTargets, nextConfig.DenyTargets, err)) {
            return TqJsonResponse(500, ErrorJson(err));
        }
        if (!updateAcl(nextAcl)) {
            return TqJsonResponse(
                503,
                TqStructuredErrorJson("not_supported", "failed to apply server ACL runtime state"));
        }
        runtimeState.Commit(nextConfig, nextAcl);
        std::vector<std::string> resolved;
        {
            std::lock_guard<std::mutex> guard(metrics.Lock);
            resolved = metrics.ResolvedListens;
        }
        return TqJsonResponse(200, TqServerRuntimeConfigJson(nextConfig, resolved, false));
    }
```

Keep the existing const `TqConfig&` overload for tests and legacy call sites by delegating GET-only behavior to a temporary state with a dummy file store only when no PATCH is used.

- [ ] **Step 5: Run server admin tests**

Run:

```bash
rtk cmake --build build --target tcpquic_server_admin_test -j2
rtk ./build/bin/Release/tcpquic_server_admin_test
```

Expected: build succeeds and test exits 0.

- [ ] **Step 6: Commit**

```bash
rtk git add src/runtime/server_admin.h src/runtime/server_admin.cpp src/unittest/server_admin_test.cpp
rtk git commit -m "feat: patch server ACL through admin API"
```

---

### Task 5: Main Server Wiring

**Files:**
- Modify: `src/main.cpp`
- Modify: `src/runtime/server_admin.cpp`
- Modify: `src/unittest/server_admin_test.cpp`

- [ ] **Step 1: Add a rollback test for reactor update failure**

Append to `src/unittest/server_admin_test.cpp`:

```cpp
int TestPatchServerConfigDoesNotCommitWhenReactorUpdateFails() {
    const auto path = TempServerConfigPath("reactor-fail");
    WriteText(path, R"({
      "server": {"proto_listen": "0.0.0.0:4433", "allow_targets": ["10.0.0.0/8"], "deny_targets": []}
    })");

    TqConfig cfg;
    cfg.Mode = TqMode::Server;
    cfg.ConfigPath = path.string();
    cfg.QuicListen = "0.0.0.0:4433";
    cfg.AllowTargets = {"10.0.0.0/8"};

    TqServerRuntimeConfigState state(cfg);
    TqRuntimeConfigFileStore store(path.string());
    auto failUpdate = [](const TqAcl&) { return false; };

    TqServerMetrics metrics;
    const std::string response = TqHandleServerAdmin(
        Request("PATCH", "/server/config", "{\"allow_targets\":[\"127.0.0.1/32\"]}"),
        metrics,
        10,
        state,
        store,
        failUpdate);

    if (response.find("HTTP/1.1 503 Service Unavailable") == std::string::npos) return 620;
    if (state.SnapshotConfig().AllowTargets != std::vector<std::string>{"10.0.0.0/8"}) return 621;
    std::filesystem::remove(path);
    return 0;
}
```

Call it in `main()`:

```cpp
    if (int code = TestPatchServerConfigDoesNotCommitWhenReactorUpdateFails()) return code;
```

- [ ] **Step 2: Run test and confirm current behavior**

Run:

```bash
rtk cmake --build build --target tcpquic_server_admin_test -j2
rtk ./build/bin/Release/tcpquic_server_admin_test
```

Expected: if Task 4 did not guard commit correctly, this fails with code 621; otherwise it passes.

- [ ] **Step 3: Wire runtime state and file store in RunServer**

Modify `src/main.cpp` inside `RunServer()` after metrics initialization:

```cpp
    auto runtimeState = std::make_shared<TqServerRuntimeConfigState>(cfg);
    auto configFileStore = std::make_shared<TqRuntimeConfigFileStore>(cfg.ConfigPath);
```

Add includes:

```cpp
#include "runtime_config_file_store.h"
#include "server_runtime_config.h"
```

Change admin handler capture:

```cpp
        admin.reset(new TqAdminHttpServer(cfg.AdminListen, [metrics, started, &serverDial, runtimeState, configFileStore](const TqHttpRequest& req) {
            const uint64_t uptimeSeconds = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count());
            metrics->TcpDialing.store(serverDial.PendingCount());
            if (req.Method == "GET" && req.Path == "/health") {
                return TqJsonResponse(200, TqServerHealthJson(*metrics, uptimeSeconds));
            }
            if (req.Method == "GET" && req.Path == "/metrics") {
                return TqJsonResponse(200, TqServerMetricsJson(*metrics, uptimeSeconds));
            }
            return TqHandleServerAdmin(
                req,
                *metrics,
                uptimeSeconds,
                *runtimeState,
                *configFileStore,
                [&serverDial](const TqAcl& acl) {
                    serverDial.UpdateAcl(acl);
                    return true;
                });
        }, adminOptions));
```

- [ ] **Step 4: Add rollback attempt after reactor update failure**

In `src/runtime/server_admin.cpp`, before writing new config, capture current state:

```cpp
        const TqConfig previousConfig = runtimeState.SnapshotConfig();
```

If `updateAcl(nextAcl)` fails after file write, call:

```cpp
        (void)fileStore.PatchServerAcl(previousConfig.AllowTargets, previousConfig.DenyTargets, err);
```

Then return 503 without committing state.

- [ ] **Step 5: Build main and server admin tests**

Run:

```bash
rtk cmake --build build --target tcpquic-proxy tcpquic_server_admin_test -j2
rtk ./build/bin/Release/tcpquic_server_admin_test
```

Expected: both build targets succeed and test exits 0.

- [ ] **Step 6: Commit**

```bash
rtk git add src/main.cpp src/runtime/server_admin.cpp src/unittest/server_admin_test.cpp
rtk git commit -m "feat: wire server ACL runtime updates"
```

---

### Task 6: Admin HTTP route whitelist

**Files:**
- Modify: `src/runtime/admin_http.cpp`
- Modify: `src/unittest/admin_http_test.cpp`

- [ ] **Step 1: Add HTTP route test for PATCH /api/v1/server/config**

In `src/unittest/admin_http_test.cpp`, find the fake handler block that handles `/server/config`. Add:

```cpp
            if (req.Method == "PATCH" && req.Path == "/server/config") {
                return TqJsonResponse(200, "{\"role\":\"server\",\"allow_targets\":[\"127.0.0.1/32\"],\"deny_targets\":[]}");
            }
```

In the authorized request section, add:

```cpp
        std::string serverConfigPatchResponse;
        if (const int code = sendAuthorized(
                "PATCH",
                "/api/v1/server/config",
                serverConfigPatchResponse,
                "{\"allow_targets\":[\"127.0.0.1/32\"]}")) return code;
        if (!TqHttpStatusIs(serverConfigPatchResponse, 200)) return 1291;
        if (serverConfigPatchResponse.find("\"allow_targets\"") == std::string::npos) return 1292;
```

- [ ] **Step 2: Run test to verify it fails**

Run:

```bash
rtk cmake --build build --target tcpquic_admin_http_test -j2
rtk ./build/bin/Release/tcpquic_admin_http_test
```

Expected: fails because `PATCH /api/v1/server/config` is not allowed by the v1 whitelist.

- [ ] **Step 3: Allow PATCH on server config**

Modify `src/runtime/admin_http.cpp` in the v1 path allowlist. Keep `/api/v1/server/config` allowed for GET and PATCH. If the current allowlist checks only path, no change is needed; if method checks are added nearby, include:

```cpp
path == "/api/v1/server/config"
```

and do not restrict it to GET.

- [ ] **Step 4: Run admin HTTP test**

Run:

```bash
rtk cmake --build build --target tcpquic_admin_http_test -j2
rtk ./build/bin/Release/tcpquic_admin_http_test
```

Expected: test exits 0.

- [ ] **Step 5: Commit**

```bash
rtk git add src/runtime/admin_http.cpp src/unittest/admin_http_test.cpp
rtk git commit -m "feat: allow server config patch route"
```

---

### Task 7: Admin Console ACL 编辑页面

**Files:**
- Modify: `src/runtime/admin_console.cpp`
- Modify: `src/unittest/admin_http_test.cpp`

- [ ] **Step 1: Add static console assertions**

In `src/unittest/admin_http_test.cpp` static console section, add assertions:

```cpp
        if (html.find("id=\"server-acl-allow\"") == std::string_view::npos) return 564;
        if (html.find("id=\"server-acl-deny\"") == std::string_view::npos) return 565;
        if (html.find("id=\"server-acl-save\"") == std::string_view::npos) return 566;
        if (html.find("id=\"server-acl-status\"") == std::string_view::npos) return 567;
        if (js.find("async function saveServerAcl()") == std::string_view::npos) return 568;
        if (js.find("api('/server/config', { method: 'PATCH', body: payload })") == std::string_view::npos) return 569;
        if (js.find("配置文件已写回") == std::string_view::npos) return 570;
```

- [ ] **Step 2: Run test and confirm it fails**

Run:

```bash
rtk cmake --build build --target tcpquic_admin_http_test -j2
rtk ./build/bin/Release/tcpquic_admin_http_test
```

Expected: fails with one of the new return codes 564-570.

- [ ] **Step 3: Replace ACL page markup**

Modify `src/runtime/admin_console.cpp` server ACL section:

```html
        <section id="server-acl" class="page">
          <div class="title-row"><div><h2>ACL - server</h2><p class="subtitle">server 独有页面。修改后立即影响新建 tunnel，并写回 server JSON 配置文件。</p></div><div class="actions"><button class="btn" id="server-acl-reload">Reload</button><button class="btn primary" id="server-acl-save">Save ACL</button></div></div>
          <div class="grid">
            <div class="card span-5"><h3>allow_targets</h3><textarea id="server-acl-allow" spellcheck="false"></textarea><p class="subtitle">每行一个 CIDR；空 allow 表示拒绝所有普通目标。</p></div>
            <div class="card span-5"><h3>deny_targets</h3><textarea id="server-acl-deny" spellcheck="false"></textarea><p class="subtitle">deny 优先于 allow。</p></div>
            <div class="card span-2"><h3>统计</h3><div class="metric"><span class="label">acl_denied</span><span class="value" id="server-acl-denied">0</span><span class="note">from /api/v1/metrics</span></div></div>
            <div class="card span-12"><div class="callout" id="server-acl-status">当前值来自 /api/v1/server/config。</div></div>
            <div class="card span-12 table-scroll"><h3>规则摘要</h3><table><thead><tr><th>type</th><th>targets</th></tr></thead><tbody id="server-acl-rules"></tbody></table></div>
          </div>
        </section>
```

- [ ] **Step 4: Add JS helpers and save function**

In `TqAdminConsoleJs()`, add:

```js
    function aclTextToList(value) {
      return String(value || '').split(/[\n,]/).map(v => v.trim()).filter(Boolean).filter((v, i, a) => a.indexOf(v) === i);
    }
    function aclListToText(value) {
      return Array.isArray(value) ? value.join('\n') : '';
    }
```

Update `renderServerAcl()` to populate textareas:

```js
      const allowInput = document.getElementById('server-acl-allow');
      const denyInput = document.getElementById('server-acl-deny');
      if (allowInput) allowInput.value = aclListToText(allowTargets);
      if (denyInput) denyInput.value = aclListToText(denyTargets);
      setElementText('server-acl-status', '当前 ACL 已加载；保存会立即生效并写回配置文件。');
```

Add:

```js
    async function saveServerAcl() {
      const payload = {
        allow_targets: aclTextToList(document.getElementById('server-acl-allow').value),
        deny_targets: aclTextToList(document.getElementById('server-acl-deny').value)
      };
      const data = await api('/server/config', { method: 'PATCH', body: payload });
      setElementText('server-acl-status', '保存成功：运行中已生效，配置文件已写回。');
      await renderServerAcl();
      return data;
    }
```

Wire buttons in `wireActions()`:

```js
      const serverAclSave = document.getElementById('server-acl-save');
      if (serverAclSave) serverAclSave.onclick = () => runClientAction(saveServerAcl);
      const serverAclReload = document.getElementById('server-acl-reload');
      if (serverAclReload) serverAclReload.onclick = () => runClientAction(renderServerAcl);
```

- [ ] **Step 5: Run admin HTTP console test**

Run:

```bash
rtk cmake --build build --target tcpquic_admin_http_test -j2
rtk ./build/bin/Release/tcpquic_admin_http_test
```

Expected: test exits 0.

- [ ] **Step 6: Commit**

```bash
rtk git add src/runtime/admin_console.cpp src/unittest/admin_http_test.cpp
rtk git commit -m "feat: edit server ACL in admin console"
```

---

### Task 8: Docs and API contract

**Files:**
- Modify: `docs/admin-api/interface.md`
- Modify: `docs/config_guide_cn.md`
- Modify: `docs/server-admin-console.md`

- [ ] **Step 1: Update Admin API docs**

In `docs/admin-api/interface.md`, update server route table:

```markdown
| `PATCH` | `/api/v1/server/config` | `200`/`400`/`500`/`503` | 热更新 server ACL；成功后立即影响新建 tunnel，并写回 server JSON 配置文件。 |
```

Add request body under Server section:

```markdown
`PATCH /api/v1/server/config`：

```json
{
  "allow_targets": ["127.0.0.1/32", "10.0.0.0/8"],
  "deny_targets": ["169.254.0.0/16"]
}
```

- `allow_targets` 和 `deny_targets` 支持数组或逗号分隔字符串。
- 未出现字段保持当前值；至少需要出现一个字段。
- 空 `allow_targets` 表示拒绝所有普通目标。
- 成功后更新运行中 ACL，并写回 `--config` 指定的 JSON 配置文件。
- 配置文件必须是严格 JSON；写回使用 JSON AST 重新格式化，不保留注释。
```

- [ ] **Step 2: Update config guide**

In `docs/config_guide_cn.md` Server section, add after ACL description:

```markdown
server 模式启用 admin 后，可以通过 `PATCH /api/v1/server/config` 或 admin-console 修改 `allow_targets` / `deny_targets`。修改成功后立即影响新建 tunnel，并写回 `--config` 指定的 JSON 配置文件。配置文件必须是严格 JSON；写回会重新格式化文件，不保留注释或字段顺序。
```

- [ ] **Step 3: Run documentation grep checks**

Run:

```bash
rtk rg -n "PATCH.*/api/v1/server/config|写回|严格 JSON|allow_targets" docs/admin-api/interface.md docs/config_guide_cn.md docs/server-admin-console.md
```

Expected: output contains the new PATCH route and persistence semantics in all three docs.

- [ ] **Step 4: Commit**

```bash
rtk git add docs/admin-api/interface.md docs/config_guide_cn.md docs/server-admin-console.md
rtk git commit -m "docs: describe server ACL config persistence"
```

---

### Task 9: Focused verification and E2E smoke

**Files:**
- No source edits unless verification exposes a bug.

- [ ] **Step 1: Build focused targets**

Run:

```bash
rtk cmake --build build --target \
  tcpquic_runtime_config_file_store_test \
  tcpquic_server_admin_test \
  tcpquic_server_dial_reactor_test \
  tcpquic_admin_http_test \
  tcpquic-proxy \
  -j2
```

Expected: all targets build successfully.

- [ ] **Step 2: Run focused unit tests**

Run:

```bash
rtk ./build/bin/Release/tcpquic_runtime_config_file_store_test
rtk ./build/bin/Release/tcpquic_server_admin_test
rtk ./build/bin/Release/tcpquic_server_dial_reactor_test
rtk ./build/bin/Release/tcpquic_admin_http_test
```

Expected: each command exits 0.

- [ ] **Step 3: Run existing ACL and config tests**

Run:

```bash
rtk cmake --build build --target tcpquic_acl_test tcpquic_acl_filter_test tcpquic_config_router_test -j2
rtk ./build/bin/Release/tcpquic_acl_test
rtk ./build/bin/Release/tcpquic_acl_filter_test
rtk ./build/bin/Release/tcpquic_config_router_test
```

Expected: each command exits 0.

- [ ] **Step 4: Manual admin API smoke with temp config**

Create a temporary server config with existing cert paths from the local test scripts or skip this step when cert files are unavailable:

```bash
rtk ./build/bin/Release/raypx2 server --config /tmp/tcpquic-server-admin-smoke.json
```

Then use the admin token file printed by the process:

```bash
AUTH="Authorization: Bearer $(jq -r .token /tmp/tcpquic-proxy-admin/admin-token.json)"
rtk curl -sS -H "$AUTH" http://127.0.0.1:18081/api/v1/server/config
rtk curl -sS -X PATCH -H "$AUTH" -H 'Content-Type: application/json' \
  --data '{"allow_targets":["127.0.0.1/32"],"deny_targets":["169.254.0.0/16"]}' \
  http://127.0.0.1:18081/api/v1/server/config
rtk rg -n '"allow_targets"|"deny_targets"' /tmp/tcpquic-server-admin-smoke.json
```

Expected: PATCH response includes updated ACL and the config file contains the same arrays. If local cert fixtures are unavailable, record that only unit/admin HTTP tests were run.

- [ ] **Step 5: Final diff review**

Run:

```bash
rtk git diff --stat
rtk git diff -- src/runtime src/tunnel src/unittest src/CMakeLists.txt docs/admin-api/interface.md docs/config_guide_cn.md docs/server-admin-console.md
```

Expected: diff only contains server admin config persistence work and planned docs.

---

## Self-Review

Spec coverage:

- ACL admin-console edit: Task 7.
- Immediate effect on new tunnel: Task 3, Task 4, Task 5.
- Config file persistence with third-party JSON library: Task 1, Task 4, Task 8.
- Failure keeps old ACL: Task 1, Task 2, Task 4, Task 5.
- Admin auth/path constraints: Task 6.
- Tests and E2E smoke: Task 9.

No placeholder terms are intentionally left in this plan. Every task names concrete files, commands, expected results, and implementation snippets.
