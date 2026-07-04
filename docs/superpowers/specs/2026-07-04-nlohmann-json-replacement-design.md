# nlohmann/json 完整替换设计

## 背景

当前项目的 JSON 处理由多个手写 parser 和 `std::ostringstream` 拼接输出组成：

- `src/config/config.cpp` 的 `JsonParser` 解析运行时配置和 legacy client config。
- `src/runtime/admin_config.cpp` 的 `DiagnosticsPatchParser`、`RuntimeConfigPatchParser` 解析 Admin PATCH body。
- `src/runtime/router_runtime.cpp` 的 `RouterJsonParser` 解析 router/peer 配置和 peer action body。
- `src/runtime/admin_config.cpp`、`src/runtime/server_metrics.cpp`、`src/runtime/relay_metrics.cpp`、`src/runtime/server_admin.cpp`、`src/runtime/memory_stats.cpp` 等文件手写 JSON 输出。

目标是把业务 JSON 解析和输出完整迁移到 `nlohmann/json`，并通过第三方子模块引入依赖。

## 约束

- `nlohmann/json` 作为 `third_party/json` Git 子模块引入。
- CMake 暴露并链接 `nlohmann_json::nlohmann_json`。
- 输出只要求 JSON 语义等价，不要求字段顺序、空格或字节级格式保持不变。
- 保留现有业务规则：字段名、别名兼容、必填字段、范围校验、运行时不支持字段的错误语义。
- 不把第三方库异常泄漏给用户路径；解析异常转换为现有风格的错误字符串。
- 不引入额外抽象层，除非能消除明显重复或简化调用点。

## 方案

### 依赖接入

在顶层 CMake 中加入 `third_party/json`，并让 `tcpquic-proxy` 目标链接 `nlohmann_json::nlohmann_json`。该库是 header-only，预期不会改变运行时链接布局。

### 解析迁移

配置解析由 “游标扫描 JSON 文本” 改为：

1. 使用 `nlohmann::json::parse(body)` 得到 JSON 值。
2. 校验顶层类型必须是 object。
3. 按现有业务字段逐项读取，保留现有错误消息的主要语义。
4. 对支持忽略未知字段的 legacy 路径继续忽略；对 runtime config 和 patch 路径继续拒绝未知字段。

涉及入口：

- `LoadRuntimeConfigFile`
- `TqLoadClientConfig`
- `TqApplyDiagnosticsPatch`
- `TqApplyRuntimeConfigPatch`
- `ParsePeerConfigBody`
- `ParsePeerPatchBody`
- `ParsePeerActionBodyText`

### 输出迁移

JSON 输出由手写字符串拼接改为构造 `nlohmann::json` 后调用 `dump()`。

涉及输出：

- runtime/public config JSON
- diagnostics JSON
- structured error JSON
- client/server metrics 和 health JSON
- relay active/worker/metrics JSON
- server admin connections/tunnels JSON
- memory allocator stats JSON
- benchmark JSON 输出

响应封装仍由现有 HTTP 层负责，例如 `TqSetJson` 和 `TqJsonResponse` 继续设置状态码、Content-Type 和 Content-Length。

## 测试策略

- 先添加或调整测试，使关键响应按 JSON 语义校验，而不是依赖字段顺序。
- 保留业务行为断言：字段存在、值正确、敏感字段 redaction、错误路径、未知字段、范围校验、legacy alias。
- 跑现有单元测试，必要时只修改因字段顺序或转义格式差异导致的字符串断言。
- 完成后跑构建，确认 `nlohmann/json` 子模块和 CMake 集成正常。

## 风险和处理

- 字段顺序变化可能影响外部脚本。已确认只要求 JSON 语义等价；项目内测试会迁移到语义断言。
- 手写 parser 之前只支持有限 JSON 语法，迁移后语法接受面更完整。业务字段仍通过显式类型和范围校验限制。
- `nlohmann::json` 默认 object 可能按 key 排序。因为不要求字节级一致，这可接受。
- 当前工作区已有用户改动，实现时必须避免覆盖无关改动。
