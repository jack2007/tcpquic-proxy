# client tunnel open 测试链接修复设计

## 问题

Linux 构建 `tcpquic_client_tunnel_open_test` 时，链接器报告：

```text
undefined reference to TqRelayStartManagedOnLinuxWorkerForTest(...)
```

该测试目标定义了 `TCPQUIC_TUNNEL_TESTING=1` 与 `TQ_UNIT_TESTING=1`，所以
`tunnel/tcp_tunnel.cpp` 会生成对测试专用 relay 入口的调用。生产入口实现在
`tunnel/relay.cpp`，但 `client_tunnel_open_test.cpp` 为 relay API 提供了自己的测试
stub，以支持失败注入、调用计数和流生命周期捕获。新增 Linux worker override 入口时，
这组 stub 没有同步补齐该符号，导致最终链接时缺少定义。

## 修复方案

仅修改 `src/unittest/client_tunnel_open_test.cpp`，在
`TQ_UNIT_TESTING && __linux__` 条件下补充
`TqRelayStartManagedOnLinuxWorkerForTest(...)` stub。该 stub 忽略测试当前不使用的
worker override，并把其余参数原样委托给同文件现有的 `TqRelayStartManaged(...)`
stub，保持已有失败注入、调用计数、fd 消费和流生命周期语义。

不把 `tunnel/relay.cpp` 加入该测试目标，因为它会与文件内已有的七个 relay stub 产生
重复定义。不拆分公共 stub 文件，避免为单一符号遗漏引入额外重构。

## 平台范围

缺失符号由 `TQ_UNIT_TESTING && __linux__` 条件生成，因此新增 stub 使用相同条件保护。
macOS 和 Windows 不编译该 stub，也不修改对应目标结构。

## 验证

1. 修改前重新构建 `tcpquic_client_tunnel_open_test`，确认以目标 undefined reference 失败。
2. 补齐测试 stub 后重新构建该目标，确认链接成功。
3. 运行 `tcpquic_client_tunnel_open_test`，确认测试通过。
4. 构建 `all`，确认不再被该链接错误阻断。
5. 运行独立构建目录中全部 `tcpquic_*_test`，记录通过数量或任何与本次无关的失败。
6. 运行 `git diff --check`，只提交本任务文件。

## 交付

提交修复后，将当前 `master` 推送到 `origin/master`。推送会包含此前尚未推送的 libuv
集成提交、本规格和计划提交，以及本次链接修复提交。不会暂存或提交工作区中已有的
其他修改和未跟踪文件。
