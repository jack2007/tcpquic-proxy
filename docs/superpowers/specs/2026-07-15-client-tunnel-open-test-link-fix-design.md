# client tunnel open 测试链接修复设计

## 问题

Linux 构建 `tcpquic_client_tunnel_open_test` 时，链接器报告：

```text
undefined reference to TqRelayStartManagedOnLinuxWorkerForTest(...)
```

该测试目标定义了 `TCPQUIC_TUNNEL_TESTING=1` 与 `TQ_UNIT_TESTING=1`，所以
`tunnel/tcp_tunnel.cpp` 会生成对测试专用 relay 入口的调用。入口实现在
`tunnel/relay.cpp`，但 Linux 的 `TCPQUIC_CLIENT_TUNNEL_OPEN_TEST_SOURCES` 没有包含
该源文件，导致最终链接时缺少定义。

## 修复方案

仅修改 `src/CMakeLists.txt`，在 Linux 条件下为
`TCPQUIC_CLIENT_TUNNEL_OPEN_TEST_SOURCES` 加入 `tunnel/relay.cpp`。

不新增测试 stub，因为 stub 会绕过待验证的生产 relay 启动路径。不拆分测试专用入口，
避免为单一构建依赖遗漏引入额外重构。

## 平台范围

缺失符号由 `TQ_UNIT_TESTING && __linux__` 条件生成，因此本次只修改 Linux 源文件列表。
macOS 不定义该入口；Windows 当前也不会进入 Linux worker override 分支，不修改对应列表。

## 验证

1. 修改前重新构建 `tcpquic_client_tunnel_open_test`，确认以目标 undefined reference 失败。
2. 加入 `tunnel/relay.cpp` 后重新构建该目标，确认链接成功。
3. 运行 `tcpquic_client_tunnel_open_test`，确认测试通过。
4. 构建 `all`，确认不再被该链接错误阻断。
5. 运行独立构建目录中全部 `tcpquic_*_test`，记录通过数量或任何与本次无关的失败。
6. 运行 `git diff --check`，只提交本任务文件。

## 交付

提交修复后，将当前 `master` 推送到 `origin/master`。推送会包含此前尚未推送的 libuv
集成提交、本规格和计划提交，以及本次链接修复提交。不会暂存或提交工作区中已有的
其他修改和未跟踪文件。
