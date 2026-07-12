# Admin Console Action 按钮变量遮蔽修复实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 修复 Admin Console 在英文和中文下无法渲染 Peers Edit/Delete 与 Connections Detail 按钮的问题。

**Architecture:** 保留现有 `tr(key)` 翻译函数和全部翻译键，仅将两个 action 渲染循环中的 DOM 行变量从 `tr` 改为 `rowElement`，消除词法遮蔽。使用现有 `tcpquic_admin_http_test` 对嵌入式 JavaScript 与双语词条建立回归约束，并以 Firefox 对实际 Release Admin Console 做端到端验证。

**Tech Stack:** C++17、嵌入式 HTML/CSS/JavaScript、CMake、cpp-httplib、Firefox/geckodriver。

## Global Constraints

- 文档使用中文。
- 不改变全局翻译函数、翻译键、DOM data 属性、事件绑定或 Admin API。
- 英文按钮文案必须是 `Edit`、`Delete`、`Detail`。
- 中文按钮文案必须是 `编辑`、`删除`、`详情`。
- 不调整表格布局、按钮样式或 CRUD 行为。
- 保留工作区内所有无关修改，不纳入本任务提交。

---

### Task 1: 修复双语 Action 按钮渲染

**Files:**
- Modify: `src/unittest/admin_http_test.cpp:380-450`
- Modify: `src/runtime/admin_console.cpp:1857-1915`

**Interfaces:**
- Consumes: `TqAdminConsoleJs()` 返回的嵌入式 JavaScript；现有 `tr(key)` 根据 `consoleState.lang` 返回英中文案。
- Produces: Peers 行的 `[data-edit-peer]`、`[data-delete-peer]` 按钮，以及 Connections 行的 `[data-connection-id]` Detail 按钮。

- [ ] **Step 1: 写入失败回归测试**

在 `src/unittest/admin_http_test.cpp` 的 Admin Console 静态断言区域加入：

```cpp
        if (js.find("tbody.querySelectorAll('tr').forEach((tr, index) => {") != std::string_view::npos) return 1388;
        if (js.find("tbody.querySelectorAll('tr').forEach((rowElement, index) => {") == std::string_view::npos) return 1389;
        if (js.find("rowElement.appendChild(td);") == std::string_view::npos) return 1390;
        if (js.find("'action.edit': 'Edit'") == std::string_view::npos) return 1391;
        if (js.find("'action.delete': 'Delete'") == std::string_view::npos) return 1392;
        if (js.find("'action.detail': 'Detail'") == std::string_view::npos) return 1393;
        if (js.find("'action.edit': '编辑'") == std::string_view::npos) return 1394;
        if (js.find("'action.delete': '删除'") == std::string_view::npos) return 1395;
        if (js.find("'action.detail': '详情'") == std::string_view::npos) return 1396;
```

- [ ] **Step 2: 构建并运行测试，确认 RED**

Run:

```bash
rtk cmake --build build --config Release --target tcpquic_admin_http_test -j2
rtk build/bin/Release/tcpquic_admin_http_test
```

Expected: 构建成功，测试退出码为 `108`（POSIX 对 `return 1388` 的低 8 位），证明旧的 `forEach((tr, index)` 被新增测试捕获。

- [ ] **Step 3: 写入最小生产修复**

在 Peers 和 Connections 两个循环中只做以下同类替换：

```javascript
      tbody.querySelectorAll('tr').forEach((rowElement, index) => {
        // 保持现有 peerId/row、td.innerHTML 与翻译调用不变。
        rowElement.appendChild(td);
      });
```

Peers 模板继续调用 `tr('action.edit')`、`tr('action.delete')`；Connections 模板继续调用 `tr('action.detail')`。

- [ ] **Step 4: 运行目标测试，确认 GREEN**

Run:

```bash
rtk cmake --build build --config Release --target tcpquic_admin_http_test -j2
rtk build/bin/Release/tcpquic_admin_http_test
```

Expected: 两条命令退出码均为 `0`，无失败输出。

- [ ] **Step 5: 构建 Release 主程序**

Run:

```bash
rtk cmake --build build --config Release --target tcpquic-proxy -j2
```

Expected: 退出码为 `0`，生成更新后的 `build/bin/Release/raypx2`。

- [ ] **Step 6: 使用实际 Admin Console 验证英文和中文**

使用独立端口启动更新后的 Release client，避免修改用户现有 PID 3419645 的运行状态。通过其 token 文件登录 `/console/`，用 Firefox/geckodriver 执行以下断言：

```javascript
// English
consoleState.lang = 'en';
await renderClientPeers();
assert([...document.querySelectorAll('[data-edit-peer]')].every(button => button.textContent === 'Edit'));
assert([...document.querySelectorAll('[data-delete-peer]')].every(button => button.textContent === 'Delete'));

// 中文
consoleState.lang = 'zh';
await renderClientPeers();
assert([...document.querySelectorAll('[data-edit-peer]')].every(button => button.textContent === '编辑'));
assert([...document.querySelectorAll('[data-delete-peer]')].every(button => button.textContent === '删除'));
```

再调用 `renderClientConnections()`：若 API 返回至少一行，分别断言 Detail/详情；无连接行时，确认函数成功完成，并由单元测试覆盖对应 action 模板。捕获并断言整个过程没有 `TypeError: tr is not a function`。

Expected: 两种语言下 Peers 每行各有一个编辑和删除按钮；有 Connections 数据时每行各有一个详情按钮；无 JavaScript 异常。

- [ ] **Step 7: 检查差异与提交**

Run:

```bash
rtk git diff --check -- src/runtime/admin_console.cpp src/unittest/admin_http_test.cpp
rtk git diff -- src/runtime/admin_console.cpp src/unittest/admin_http_test.cpp
rtk git status --short
```

Expected: 仅两个目标文件包含本任务代码改动，`git diff --check` 退出码为 `0`；工作区既有无关改动保持原状。

Commit:

```bash
rtk git add src/runtime/admin_console.cpp src/unittest/admin_http_test.cpp
rtk git commit -m "fix: render localized admin console actions"
```
