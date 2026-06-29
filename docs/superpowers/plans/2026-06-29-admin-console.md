# Admin Console Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在现有 Admin HTTP server 内置一个浏览器管理页面，严格以 `docs/superpowers/specs/2026-06-29-admin-console-interaction-v4.html` 为 UI 参照，并只展示当前 Admin API 已直接返回的信息。

**Architecture:** Admin HTTP server 继续作为唯一入口，新增 `/`、`/console/`、`/console/app.js`、`/console/style.css` 静态资源路由；`/api/v1/*` 保持 Bearer token 鉴权。前端是无依赖单页应用，所有管理数据通过同源 `/api/v1/*` 获取，页面导航和字段严格服从 v4 快照与设计文档。C++ 端只内嵌静态资源和调整 admin listen 绑定校验，不修改 tcpquic-proxy 核心通信协议或数据面。

**Tech Stack:** C++17、cpp-httplib、现有 Admin API、原生 HTML/CSS/JavaScript、现有 CMake 单元测试可执行文件。

---

## Scope and Design Freeze

本计划实现以下已批准设计：

- 设计文档：`docs/superpowers/specs/2026-06-29-admin-console-design.md`
- 冻结 UI 快照：`docs/superpowers/specs/2026-06-29-admin-console-interaction-v4.html`

实现者必须遵守：

- 页面结构、左侧导航、顶部状态条、字段顺序、按钮能力和视觉密度以 v4 快照为准。
- 顶部状态条只展示 `role`、`health`、`refresh`，不展示 `admin.listen`。
- client 页面只包含 `Overview`、`Peers`、`Connections`、`Tunnels`。
- server 页面只包含 `Overview`、`Peers`、`Connections`、`Tunnels`、`ACL`。
- `Peers - client` 的 Create/Edit 字段顺序固定为 `peer_id`、peer endpoint、`socks_listen`、`http_listen`、`port_forwards`、`enabled`。
- 不展示当前 Admin API 未直接返回的信息，例如 connection 时间、connection 累计字节、client tunnel source、server tunnel remote source、ACL 最近拒绝明细。
- 若实现中发现必须偏离 v4 快照，先更新 spec 和 v4 快照并取得确认，再继续编码。

## File Structure

新增文件：

- `src/runtime/admin_console.h`：声明 Console 静态资源访问函数。
- `src/runtime/admin_console.cpp`：内嵌 `/console/` HTML、CSS、JS 静态资源，并提供 `std::string_view` 访问函数。

修改文件：

- `src/runtime/admin_http.cpp`：增加 Console 静态路由、根路径重定向、非 API 静态资源免鉴权、非 loopback admin listen 校验放开。
- `src/CMakeLists.txt`：把 `runtime/admin_console.cpp` 加入主程序和 `tcpquic_admin_http_test`。
- `src/unittest/admin_http_test.cpp`：覆盖 Console 路由、静态资源免鉴权、API 仍需鉴权、非 loopback listen 校验。

不修改：

- `src/protocol/*`
- `src/tunnel/*`
- `src/runtime/router_runtime.cpp` 的业务 API 字段
- `src/runtime/server_admin.cpp` 的业务 API 字段

---

### Task 1: Add Console Static Asset Module

**Files:**
- Create: `src/runtime/admin_console.h`
- Create: `src/runtime/admin_console.cpp`
- Modify: `src/CMakeLists.txt`
- Test: `src/unittest/admin_http_test.cpp`

- [ ] **Step 1: Write the failing compile reference**

Add these includes and checks near the top of `src/unittest/admin_http_test.cpp`, after existing includes:

```cpp
#include "admin_console.h"
```

Add this block near the beginning of `main()`, after socket startup succeeds:

```cpp
    {
        const std::string_view html = TqAdminConsoleHtml();
        const std::string_view css = TqAdminConsoleCss();
        const std::string_view js = TqAdminConsoleJs();
        if (html.find("raypx2 Admin Console") == std::string_view::npos) return 300;
        if (html.find("role-pill") == std::string_view::npos) return 301;
        if (html.find("<strong>listen</strong>") != std::string_view::npos) return 302;
        if (html.find("Create/Edit Peer") == std::string_view::npos) return 303;
        if (html.find("remote_identity") != std::string_view::npos) return 304;
        if (html.find("transferred_bytes") != std::string_view::npos) return 305;
        if (css.find(".sidebar") == std::string_view::npos) return 306;
        if (js.find("sessionStorage") == std::string_view::npos) return 307;
    }
```

- [ ] **Step 2: Run the test target build and verify it fails**

Run:

```bash
rtk cmake --build build-glibc --target tcpquic_admin_http_test -j
```

Expected: build fails because `admin_console.h` does not exist or `TqAdminConsoleHtml` is undefined.

- [ ] **Step 3: Add `src/runtime/admin_console.h`**

Create:

```cpp
#pragma once

#include <string_view>

std::string_view TqAdminConsoleHtml();
std::string_view TqAdminConsoleCss();
std::string_view TqAdminConsoleJs();
```

- [ ] **Step 4: Add `src/runtime/admin_console.cpp` with frozen UI-derived assets**

Create the file with this structure. The CSS block is copied from the `<style>` section of `docs/superpowers/specs/2026-06-29-admin-console-interaction-v4.html`; the HTML block is copied from the v4 `<body>` content with the `<style>` block replaced by `<link rel="stylesheet" href="/console/style.css">` and the inline `<script>` replaced by `<script src="/console/app.js"></script>`; the JS block starts with the v4 navigation behavior and is extended in later tasks.

```cpp
#include "admin_console.h"

namespace {

constexpr std::string_view kConsoleCss = R"CSS(
:root {
  --bg:#f5f7f8; --panel:#fff; --line:#d8dee4; --text:#172026; --muted:#64717d;
  --green:#15803d; --red:#b42318; --amber:#b45309; --blue:#2563eb; --ink:#22313a;
}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font-family:Inter,ui-sans-serif,system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;letter-spacing:0}
.shell{display:grid;grid-template-columns:232px 1fr;min-height:100vh}
.sidebar{background:#111820;color:#eef3f7;padding:18px 14px;display:flex;flex-direction:column;gap:14px}
.brand{padding:4px 8px 12px;border-bottom:1px solid rgba(255,255,255,.16)}
.brand h1{font-size:18px;line-height:1.2;margin:0 0 6px}
.brand p{margin:0;color:#a9b6c2;font-size:12px;line-height:1.45}
.role-switch{display:grid;grid-template-columns:1fr 1fr;gap:6px}
.role-switch button,.nav button{all:unset;cursor:pointer;border-radius:6px}
.role-switch button{padding:8px 10px;text-align:center;font-size:13px;color:#cbd5df;background:rgba(255,255,255,.07)}
.role-switch button.active{background:#eef3f7;color:#111820;font-weight:700}
.nav{display:grid;gap:4px}
.nav button{padding:10px;color:#cad4dd;font-size:13px;display:grid;grid-template-columns:22px 1fr auto;gap:8px;align-items:center}
.nav button:hover{background:rgba(255,255,255,.08)}
.nav button.active{background:#eef3f7;color:#111820;font-weight:650}
.dot{width:8px;height:8px;border-radius:50%;background:#94a3b8}.dot.ok{background:#22c55e}.dot.warn{background:#f59e0b}.dot.info{background:#38bdf8}
.main{min-width:0;display:grid;grid-template-rows:auto 1fr}
.topbar{min-height:58px;background:var(--panel);border-bottom:1px solid var(--line);padding:10px 20px;display:flex;align-items:center;justify-content:space-between;gap:14px}
.status,.actions,.filters{display:flex;flex-wrap:wrap;align-items:center;gap:8px}
.pill{display:inline-flex;align-items:center;gap:6px;min-height:26px;padding:4px 8px;border:1px solid var(--line);border-radius:999px;background:#fff;color:var(--muted);font-size:12px;white-space:nowrap}
.pill strong{color:var(--text)}
.btn{border:1px solid var(--line);background:#fff;color:var(--text);border-radius:6px;min-height:32px;padding:6px 10px;font-size:13px;cursor:pointer;white-space:nowrap}
.btn.primary{border-color:#1d4ed8;background:var(--blue);color:#fff}.btn.danger{border-color:#efb4ad;color:var(--red);background:#fff7f6}
.content{padding:18px 20px 28px;overflow:auto}
.page{display:none;gap:14px}.page.active{display:grid}
.title-row{display:flex;justify-content:space-between;align-items:flex-start;gap:16px}
h2{margin:0 0 4px;font-size:20px;line-height:1.25}h3{margin:0 0 8px;font-size:14px}
.subtitle{margin:0;color:var(--muted);font-size:13px;line-height:1.45;max-width:1000px}
.grid{display:grid;grid-template-columns:repeat(12,minmax(0,1fr));gap:12px}
.card{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:14px;min-width:0}
.span-3{grid-column:span 3}.span-4{grid-column:span 4}.span-5{grid-column:span 5}.span-6{grid-column:span 6}.span-7{grid-column:span 7}.span-8{grid-column:span 8}.span-12{grid-column:span 12}
.metric{display:grid;gap:6px}.metric .label{color:var(--muted);font-size:12px}.metric .value{font-size:24px;line-height:1.1;font-weight:720}.metric .note{color:var(--muted);font-size:12px}
.toolbar{display:flex;justify-content:space-between;align-items:center;gap:10px;flex-wrap:wrap;margin-bottom:10px}
input,select,textarea{border:1px solid var(--line);border-radius:6px;background:#fff;color:var(--text);font:inherit;font-size:13px;min-height:32px;padding:6px 9px}
textarea{width:100%;min-height:236px;resize:vertical;font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;line-height:1.5}
.table-scroll{overflow:auto}
table{width:100%;border-collapse:collapse;font-size:12px}
th,td{border-bottom:1px solid var(--line);padding:9px 8px;text-align:left;vertical-align:middle;white-space:nowrap}
th{color:var(--muted);font-weight:650;background:#f8fafb}
.state{display:inline-flex;align-items:center;gap:6px;padding:3px 7px;border-radius:999px;border:1px solid var(--line);font-size:12px;background:#fff}
.state.ok{color:var(--green);border-color:#b7e1c1;background:#f1fbf4}.state.warn{color:var(--amber);border-color:#f6d49d;background:#fff8eb}.state.err{color:var(--red);border-color:#f2b8b1;background:#fff6f5}
.peer-layout{display:grid;grid-template-columns:minmax(0,.96fr) minmax(560px,1.04fr);gap:12px;align-items:start}
.split{display:grid;grid-template-columns:minmax(0,1fr) 420px;gap:12px;align-items:start}
.drawer{background:#fbfcfd;border:1px solid var(--line);border-radius:8px;padding:14px;display:grid;gap:12px;min-width:0}
.form{display:grid;grid-template-columns:1fr 1fr;gap:10px}.field{display:grid;gap:4px}.field.full{grid-column:1/-1}.field label{color:var(--muted);font-size:12px}
.steps{display:grid;gap:8px;margin:0;padding:0;list-style:none}.steps li{display:grid;grid-template-columns:28px 1fr;gap:8px;align-items:start;font-size:13px;color:var(--ink);line-height:1.4}
.num{display:inline-flex;width:22px;height:22px;align-items:center;justify-content:center;border-radius:50%;background:#e8eef4;font-size:12px;font-weight:700}
.callout{border:1px solid #f1cc8f;background:#fff8eb;color:#7c3d00;padding:10px 12px;border-radius:8px;font-size:13px;line-height:1.45}
.confirm{border:1px solid #f0b4ad;background:#fff7f6;border-radius:8px;padding:12px;display:grid;gap:8px;font-size:13px;line-height:1.45}
.json-preview{background:#111820;color:#d8e5ed;border-radius:8px;padding:12px;overflow:auto;font-size:12px;line-height:1.5;font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;max-height:280px}
@media(max-width:1180px){.peer-layout,.split{grid-template-columns:1fr}.span-3,.span-4,.span-5,.span-6,.span-7,.span-8{grid-column:span 12}}
@media(max-width:940px){.shell{grid-template-columns:1fr}.sidebar{position:sticky;top:0;z-index:2}.nav{grid-template-columns:repeat(3,minmax(0,1fr))}.nav button{grid-template-columns:1fr;gap:4px;justify-items:start}}
)CSS";

constexpr std::string_view kConsoleJs = R"JS(
const consoleState = {
  token: sessionStorage.getItem('tcpquic_admin_token') || '',
  role: 'client',
  refreshTimer: 0,
  paused: false,
  lastSuccess: null
};

async function api(path, options = {}) {
  const headers = Object.assign({}, options.headers || {});
  if (consoleState.token) headers.Authorization = `Bearer ${consoleState.token}`;
  if (options.body && !headers['Content-Type']) headers['Content-Type'] = 'application/json';
  const response = await fetch(`/api/v1${path}`, Object.assign({}, options, { headers }));
  const text = await response.text();
  let data = {};
  try { data = text ? JSON.parse(text) : {}; } catch (_) { data = { raw: text }; }
  if (response.status === 401) {
    sessionStorage.removeItem('tcpquic_admin_token');
    consoleState.token = '';
    showPage('login');
  }
  if (!response.ok) {
    const message = data && data.error && data.error.message ? data.error.message : `HTTP ${response.status}`;
    throw new Error(message);
  }
  consoleState.lastSuccess = new Date();
  return data;
}

function text(value) {
  if (value === undefined || value === null || value === '') return '-';
  return String(value);
}

function renderRows(tbody, rows, columns) {
  tbody.innerHTML = rows.map(row => `<tr>${columns.map(col => `<td>${text(row[col])}</td>`).join('')}</tr>`).join('');
}

function setRole(role) {
  consoleState.role = role === 'server' ? 'server' : 'client';
  renderNav();
  document.getElementById('role-pill').innerHTML = `<strong>role</strong> ${consoleState.role}`;
  refreshCurrentPage();
}

function showPage(id) {
  document.querySelectorAll('.page').forEach(page => page.classList.toggle('active', page.id === id));
  document.querySelectorAll('.nav button').forEach(button => button.classList.toggle('active', button.dataset.page === id));
}

function renderNav() {
  const nav = document.getElementById('nav');
  const defs = consoleState.role === 'server'
    ? [['server-overview','Overview'],['server-peers','Peers'],['server-connections','Connections'],['server-tunnels','Tunnels'],['server-acl','ACL'],['relay','Relay'],['config','Config'],['diagnostics','Diagnostics']]
    : [['client-overview','Overview'],['client-peers','Peers'],['client-connections','Connections'],['client-tunnels','Tunnels'],['relay','Relay'],['config','Config'],['diagnostics','Diagnostics']];
  nav.innerHTML = defs.map(([id, label]) => `<button data-page="${id}"><span></span><span>${label}</span><span class="dot info"></span></button>`).join('');
  nav.querySelectorAll('button').forEach(button => {
    button.onclick = () => { showPage(button.dataset.page); refreshCurrentPage(); };
  });
  showPage(defs[0][0]);
}

function refreshCurrentPage() {
  if (!consoleState.token || consoleState.paused) return;
  const active = document.querySelector('.page.active');
  if (!active) return;
  document.getElementById('refresh-pill').innerHTML = '<strong>refresh</strong> loading';
  refreshPage(active.id)
    .then(() => { document.getElementById('refresh-pill').innerHTML = '<strong>refresh</strong> 3s auto'; })
    .catch(err => { document.getElementById('refresh-pill').innerHTML = `<strong>refresh</strong> ${text(err.message)}`; });
}
)JS";

constexpr std::string_view kConsoleHtml = R"HTML(<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>raypx2 Admin Console</title>
  <link rel="stylesheet" href="/console/style.css">
</head>
<body>
  <div class="shell">
    <aside class="sidebar">
      <div class="brand">
        <h1>raypx2 Admin Console</h1>
        <p>v4: 只展示当前 Admin API 可直接获得的信息。</p>
      </div>
      <div class="role-switch">
        <button id="role-client" class="active" data-role="client">client</button>
        <button id="role-server" data-role="server">server</button>
      </div>
      <nav class="nav" id="nav"></nav>
    </aside>
    <main class="main">
      <header class="topbar">
        <div class="status">
          <span class="pill" id="role-pill"><strong>role</strong> client</span>
          <span class="pill" id="health-pill"><span class="dot ok"></span><strong>health</strong> healthy</span>
          <span class="pill" id="refresh-pill"><strong>refresh</strong> 3s auto</span>
        </div>
        <div class="actions"><button class="btn" id="pause-refresh">Pause refresh</button><button class="btn" id="refresh-now">Refresh now</button><button class="btn danger" id="logout">Logout</button></div>
      </header>
      <section class="content">
        <section id="login" class="page">
          <div class="title-row"><div><h2>Login</h2><p class="subtitle">用户名固定为 raypx2，密码为 admin token 文件 JSON 中的 token。</p></div></div>
          <div class="grid">
            <div class="card span-5"><h3>登录</h3><div class="form"><div class="field full"><label>用户名</label><input id="login-username" value="raypx2"></div><div class="field full"><label>Admin token</label><input id="login-token" type="password" autocomplete="current-password"></div><div class="field full"><button class="btn primary" id="login-submit">Login with /api/v1/admin</button></div><div class="field full"><div class="callout" id="login-error"></div></div></div></div>
            <div class="card span-7"><h3>边界</h3><ul class="steps"><li><span class="num">1</span><span>静态资源无需认证，API 仍强制 Bearer token。</span></li><li><span class="num">2</span><span>token 只保存在 sessionStorage。</span></li><li><span class="num">3</span><span>非 loopback 绑定必须提示管理面暴露风险。</span></li></ul></div>
          </div>
        </section>
        <section id="client-overview" class="page"><div class="title-row"><div><h2>Overview - client</h2><p class="subtitle">client 视角有 Overview、Peers、Connections、Tunnels 四个业务页面；Overview 汇总 peer -> connection -> tunnel 层级。</p></div></div><div class="grid"><div class="card span-3 metric"><span class="label">Peers</span><span class="value" id="client-overview-peer-count">0</span><span class="note">enabled peers from /api/v1/peers</span></div><div class="card span-3 metric"><span class="label">Connections</span><span class="value" id="client-overview-connection-count">0</span><span class="note">sum of connection_count</span></div><div class="card span-3 metric"><span class="label">Tunnels</span><span class="value" id="client-overview-tunnel-count">0</span><span class="note">from /api/v1/tunnels</span></div><div class="card span-3 metric"><span class="label">Active streams</span><span class="value" id="client-overview-stream-count">0</span><span class="note">sum of peers</span></div><div class="card span-12 table-scroll"><h3>Peer 摘要</h3><table><thead><tr><th>peer_id</th><th>state</th><th>enabled</th><th>connection_count</th><th>connected_connections</th><th>active_streams</th><th>total_streams</th><th>reconnects</th><th>socks_listen</th><th>http_listen</th><th>last_error</th></tr></thead><tbody id="client-overview-peers"></tbody></table></div></div></section>
        <section id="client-peers" class="page"><div class="title-row"><div><h2>Peers - client</h2><p class="subtitle">client peer 是配置资源，支持 Create、Edit、Delete；Create/Edit 同一套字段，peer endpoint 支持 quic_peer 地址列表或 paths 两种表达。</p></div><button class="btn primary" id="peer-create">Create peer</button></div><div class="peer-layout"><div class="card table-scroll"><table><thead><tr><th>peer_id</th><th>state</th><th>enabled</th><th>quic_peer</th><th>socks_listen</th><th>http_listen</th><th>connection_count</th><th>connected_connections</th><th>active_streams</th><th>total_streams</th><th>reconnects</th><th>last_error</th><th>actions</th></tr></thead><tbody id="client-peers-rows"></tbody></table></div><aside class="drawer"><h3>Create/Edit Peer</h3><div class="form"><div class="field full"><label>peer_id</label><input id="peer-id" value=""></div><div class="field full"><label>peers address - quic_peer 模式</label><input id="peer-quic-peer" value=""></div><div class="field full"><label>paths 模式</label><textarea id="peer-paths" style="min-height:96px">[]</textarea></div><div class="field"><label>socks_listen</label><input id="peer-socks-listen" value="127.0.0.1:1080"></div><div class="field"><label>http_listen</label><input id="peer-http-listen" value="127.0.0.1:8080"></div><div class="field full"><label>port_forwards</label><textarea id="peer-port-forwards" style="min-height:72px">[]</textarea></div><div class="field"><label>enabled</label><select id="peer-enabled"><option>true</option><option>false</option></select></div></div><div class="callout">提交时二选一：填写 quic_peer 地址列表，或填写 paths 数组；若 paths 非空，使用 paths 作为连接路径配置。</div><div class="actions"><button class="btn">Cancel</button><button class="btn" id="peer-json-advanced">JSON advanced</button><button class="btn primary" id="peer-save">Create / Save</button></div><div class="confirm"><strong>Delete peer</strong><span>只做 Delete 确认，不在此提供 Drain/Abort。</span><button class="btn danger" id="peer-delete">Delete</button></div></aside></div></section>
        <section id="client-connections" class="page"><div class="title-row"><div><h2>Connections - client</h2><p class="subtitle">只查询展示，无 Add/Delete/Reconnect/Abort；字段来自 /api/v1/peers/{peer_id}/connections。</p></div></div><div class="toolbar"><input id="client-connections-peer" placeholder="peer_id"></div><div class="card table-scroll"><table><thead><tr><th>connection_id</th><th>peer_id</th><th>slot_index</th><th>generation</th><th>connected</th><th>retry_scheduled</th><th>state</th><th>path</th><th>local</th><th>peer</th><th>active_tunnels</th><th>last_error</th></tr></thead><tbody id="client-connections-rows"></tbody></table></div></section>
        <section id="client-tunnels" class="page"><div class="title-row"><div><h2>Tunnels - client</h2><p class="subtitle">只查询展示；只使用当前 /api/v1/tunnels 已返回字段，不展示 source。</p></div></div><div class="card table-scroll"><table><thead><tr><th>tunnel_id</th><th>peer_id</th><th>connection_id</th><th>target</th><th>state</th><th>role</th><th>ingress</th><th>compress</th><th>created_at</th><th>duration_ms</th><th>tcp_read_bytes</th><th>tcp_write_bytes</th><th>pending_bytes</th><th>relay_backend</th><th>worker_index</th><th>last_error</th></tr></thead><tbody id="client-tunnels-rows"></tbody></table></div></section>
        <section id="server-overview" class="page"><div class="title-row"><div><h2>Overview - server</h2><p class="subtitle">server 视角同样按 peer -> connection -> tunnel 展示；peer 是运行时聚合概念，不是可配置资源。</p></div></div><div class="grid"><div class="card span-3 metric"><span class="label">Peers</span><span class="value" id="server-overview-peer-count">0</span><span class="note">remote peers linked</span></div><div class="card span-3 metric"><span class="label">Connections</span><span class="value" id="server-overview-connection-count">0</span><span class="note">accepted QUIC connections</span></div><div class="card span-3 metric"><span class="label">Tunnels</span><span class="value" id="server-overview-tunnel-count">0</span><span class="note">server-side active</span></div><div class="card span-3 metric"><span class="label">ACL denied</span><span class="value" id="server-overview-acl-denied">0</span><span class="note">aggregate counter</span></div><div class="card span-12 table-scroll"><h3>Peer 摘要</h3><table><thead><tr><th>peer</th><th>remote_address</th><th>connections</th><th>active_streams</th><th>total_streams</th><th>active_tunnels</th><th>last_error</th></tr></thead><tbody id="server-overview-peers"></tbody></table></div></div></section>
        <section id="server-peers" class="page"><div class="title-row"><div><h2>Peers - server</h2><p class="subtitle">server peer 是只读运行时视图：展示当前有多少远端 peer 与本 server 有连接，以及每个 peer 下的 connection/tunnel 汇总。</p></div></div><div class="card table-scroll"><table><thead><tr><th>peer</th><th>remote_address</th><th>connections</th><th>active_streams</th><th>total_streams</th><th>active_tunnels</th><th>last_error</th></tr></thead><tbody id="server-peers-rows"></tbody></table></div><div class="callout">server 不支持配置 peer，因此这里没有 Create/Edit/Delete。peer 名称由当前 server connection 的 remote_address 聚合得到。</div></section>
        <section id="server-connections" class="page"><div class="title-row"><div><h2>Connections - server</h2><p class="subtitle">server 端连接不展示 Reconnecting；第一版不区分 connecting 和 connected，列表展示当前 server 已登记/可观测连接。</p></div></div><div class="card table-scroll"><table><thead><tr><th>connection_id</th><th>peer</th><th>remote_address</th><th>state</th><th>active_streams</th><th>total_streams</th><th>active_tunnels</th><th>last_error</th></tr></thead><tbody id="server-connections-rows"></tbody></table></div><div class="callout">当前 TqServerConnectionSnapshot 已有 remote_address、state、active/total streams、active_tunnels、last_error；页面不展示未返回的时间或字节字段。</div></section>
        <section id="server-tunnels" class="page"><div class="title-row"><div><h2>Tunnels - server</h2><p class="subtitle">只查询展示。当前 server tunnel snapshot 没有 remote source 字段，因此本页不展示 remote source；可通过 connection_id 跳到 Connections 查看 connection remote_address。</p></div></div><div class="card table-scroll"><table><thead><tr><th>tunnel_id</th><th>peer_id</th><th>connection_id</th><th>state</th><th>target</th><th>role</th><th>duration_ms</th><th>active</th></tr></thead><tbody id="server-tunnels-rows"></tbody></table></div></section>
        <section id="server-acl" class="page"><div class="title-row"><div><h2>ACL - server</h2><p class="subtitle">server 独有页面。只读展示 server config 中的 allow_targets/deny_targets 和 metrics 中的 acl_denied。</p></div></div><div class="grid"><div class="card span-7 table-scroll"><h3>规则摘要</h3><table><thead><tr><th>type</th><th>targets</th></tr></thead><tbody id="server-acl-rules"></tbody></table></div><div class="card span-5"><h3>统计</h3><div class="metric"><span class="label">acl_denied</span><span class="value" id="server-acl-denied">0</span><span class="note">from /api/v1/metrics</span></div></div><div class="card span-12"><div class="callout">当前 API 不提供逐条命中历史或最近拒绝列表，因此首版不展示 matches、last_match、time、reason 等字段。</div></div></div></section>
        <section id="relay" class="page"><div class="title-row"><div><h2>Relay</h2><p class="subtitle">client/server 共享页面，展示 relay backend、worker 聚合、pending bytes 和平台能力边界。</p></div></div><div class="grid"><div class="card span-3 metric"><span class="label">Backend</span><span class="value" id="relay-backend">-</span><span class="note">platform-specific</span></div><div class="card span-3 metric"><span class="label">Active relays</span><span class="value" id="relay-active">0</span><span class="note">aggregate</span></div><div class="card span-3 metric"><span class="label">Pending bytes</span><span class="value" id="relay-pending">0</span><span class="note">aggregate</span></div><div class="card span-3 metric"><span class="label">Errors</span><span class="value" id="relay-errors">0</span><span class="note">last refresh</span></div></div></section>
        <section id="config" class="page"><div class="title-row"><div><h2>Config</h2><p class="subtitle">client 可编辑 router config；server 首版只读展示 runtime/server config 和 ACL 配置。</p></div></div><div class="split"><div class="card"><textarea id="config-json"></textarea></div><aside class="drawer"><h3>提交规则</h3><ul class="steps"><li><span class="num">1</span><span>client 支持 JSON validate/submit。</span></li><li><span class="num">2</span><span>server 配置首版只读，不提供编辑按钮。</span></li></ul><button class="btn primary" id="config-save">Save</button></aside></div></section>
        <section id="diagnostics" class="page"><div class="title-row"><div><h2>Diagnostics</h2><p class="subtitle">client/server 共享页面，展示诊断状态和 allocator dump。</p></div></div><div class="grid"><div class="card span-6"><h3>状态</h3><pre class="json-preview" id="diagnostics-json"></pre></div><div class="card span-6"><div class="confirm"><strong>Allocator dump</strong><span>显式确认后调用 /api/v1/memory/allocator:dump。</span><button class="btn danger" id="allocator-dump">Run allocator dump</button><pre class="json-preview" id="allocator-json"></pre></div></div></div></section>
      </section>
    </main>
  </div>
  <script src="/console/app.js"></script>
</body>
</html>)HTML";

} // namespace

std::string_view TqAdminConsoleHtml() {
    return kConsoleHtml;
}

std::string_view TqAdminConsoleCss() {
    return kConsoleCss;
}

std::string_view TqAdminConsoleJs() {
    return kConsoleJs;
}
```

The embedded HTML above keeps the v4 page ids and field order. When copying from the frozen v4 snapshot, keep the same navigation order, table columns, button labels, and topbar content shown here.

- [ ] **Step 5: Add `runtime/admin_console.cpp` to CMake targets**

Modify `src/CMakeLists.txt` by inserting `runtime/admin_console.cpp` immediately before `runtime/admin_http.cpp` in `TCPQUIC_PROXY_SOURCES`:

```cmake
set(TCPQUIC_PROXY_SOURCES
    main.cpp
    config/config.cpp
    config/tuning.cpp
    protocol/control_protocol.cpp
    acl/acl.cpp
    protocol/compress.cpp
    protocol/quic_address.cpp
    protocol/quic_session.cpp
    tunnel/tcp_dialer.cpp
    tunnel/tcp_tunnel.cpp
    tunnel/tunnel_registry.cpp
    ingress/http_connect_server.cpp
    ingress/proxy_auth.cpp
    ingress/socks5_server.cpp
    tunnel/relay.cpp
    runtime/thread_pool.cpp
    runtime/listen_socket.cpp
    tunnel/tunnel_reaper.cpp
    runtime/admin_auth.cpp
    runtime/admin_console.cpp
    runtime/admin_http.cpp
    runtime/admin_memory.cpp
    runtime/admin_config.cpp
    runtime/client_peer_runtime.cpp
    runtime/memory_stats.cpp
    runtime/router_runtime.cpp
    runtime/relay_metrics.cpp
    runtime/server_admin.cpp
    runtime/server_metrics.cpp
    runtime/speed_test.cpp
    runtime/shutdown.cpp
    runtime/trace.cpp
    platform/exe_path.cpp
    ${TCPQUIC_PLATFORM_SOURCES}
)
```

Add to `tcpquic_admin_http_test`:

```cmake
add_executable(tcpquic_admin_http_test
    unittest/admin_http_test.cpp
    runtime/admin_auth.cpp
    runtime/admin_console.cpp
    runtime/admin_http.cpp
    ${TCPQUIC_PLATFORM_SOURCES}
)
```

- [ ] **Step 6: Run test target and verify it passes**

Run:

```bash
rtk cmake --build build-glibc --target tcpquic_admin_http_test -j
rtk ./build-glibc/src/tcpquic_admin_http_test
```

Expected: build succeeds and test exits `0`.

- [ ] **Step 7: Commit**

```bash
rtk git add src/runtime/admin_console.h src/runtime/admin_console.cpp src/CMakeLists.txt src/unittest/admin_http_test.cpp
rtk git commit -m "Add embedded admin console assets"
```

---

### Task 2: Serve Console Routes Without API Authentication

**Files:**
- Modify: `src/runtime/admin_http.cpp`
- Test: `src/unittest/admin_http_test.cpp`

- [ ] **Step 1: Write failing route tests**

Append this block to `src/unittest/admin_http_test.cpp` after the existing `/api/v1/admin` authenticated status test:

```cpp
    {
        TqAdminHttpServerOptions options;
        options.AdminThreads = 1;
        options.EnableTokenAuth = true;
        options.Role = "client";
        std::string err;
        TqAdminHttpServer server("127.0.0.1:0", [&](const TqHttpRequest&) {
            return TqJsonResponse(500, "{\"handler_called\":true}");
        }, options);
        if (!server.Start(err)) return 308;
        const uint16_t port = TqPortFromListenAddress(server.ListenAddress());
        if (port == 0) return 309;

        auto request = [&](const std::string& path, std::string& response) -> int {
            TqSocketHandle fd = TqConnectLocal(port);
            if (!TqSocketValid(fd)) return 310;
            const std::string req = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
            if (!TqSendAll(fd, req)) {
                TqCloseSocket(fd);
                return 311;
            }
            if (!TqRecvUntilClosed(fd, response)) {
                TqCloseSocket(fd);
                return 312;
            }
            TqCloseSocket(fd);
            return 0;
        };

        std::string root;
        if (int rc = request("/", root)) return rc;
        if (!TqHttpStatusIs(root, 302)) return 313;
        if (root.find("Location: /console/") == std::string::npos) return 314;

        std::string html;
        if (int rc = request("/console/", html)) return rc;
        if (!TqHttpStatusIs(html, 200)) return 315;
        if (html.find("Content-Type: text/html") == std::string::npos) return 316;
        if (html.find("raypx2 Admin Console") == std::string::npos) return 317;
        if (html.find("<strong>listen</strong>") != std::string::npos) return 318;

        std::string css;
        if (int rc = request("/console/style.css", css)) return rc;
        if (!TqHttpStatusIs(css, 200)) return 319;
        if (css.find("Content-Type: text/css") == std::string::npos) return 320;
        if (css.find(".sidebar") == std::string::npos) return 321;

        std::string js;
        if (int rc = request("/console/app.js", js)) return rc;
        if (!TqHttpStatusIs(js, 200)) return 322;
        if (js.find("Content-Type: application/javascript") == std::string::npos) return 323;
        if (js.find("sessionStorage") == std::string::npos) return 324;

        std::string api;
        if (int rc = request("/api/v1/health", api)) return rc;
        server.Stop();
        if (!TqHttpStatusIs(api, 401)) return 325;
    }
```

- [ ] **Step 2: Build and run to verify failure**

Run:

```bash
rtk cmake --build build-glibc --target tcpquic_admin_http_test -j
rtk ./build-glibc/src/tcpquic_admin_http_test
```

Expected: test fails because `/`, `/console/`, `/console/style.css`, and `/console/app.js` currently return `404` or require API auth.

- [ ] **Step 3: Add response helpers and static dispatch**

In `src/runtime/admin_http.cpp`, include the asset header:

```cpp
#include "admin_console.h"
```

Add helper functions near `TqSetJson`:

```cpp
void TqSetText(
    httplib::Response& res,
    int status,
    std::string_view body,
    const char* contentType) {
    res.status = status;
    res.set_header("Connection", "close");
    res.set_content(std::string(body), contentType);
}

void TqSetRedirect(httplib::Response& res, const char* location) {
    res.status = 302;
    res.set_header("Location", location);
    res.set_header("Connection", "close");
    res.set_content("", "text/plain");
}

bool TqHandleConsoleStatic(const httplib::Request& req, httplib::Response& res) {
    if (req.method != "GET") {
        return false;
    }
    if (req.path == "/") {
        TqSetRedirect(res, "/console/");
        return true;
    }
    if (req.path == "/console" || req.path == "/console/") {
        TqSetText(res, 200, TqAdminConsoleHtml(), "text/html; charset=utf-8");
        return true;
    }
    if (req.path == "/console/style.css") {
        TqSetText(res, 200, TqAdminConsoleCss(), "text/css; charset=utf-8");
        return true;
    }
    if (req.path == "/console/app.js") {
        TqSetText(res, 200, TqAdminConsoleJs(), "application/javascript; charset=utf-8");
        return true;
    }
    return false;
}
```

At the start of the `dispatch` lambda inside `TqAdminHttpServer::ConfigureRoutes()`, before `TqIsV1Prefix`, add:

```cpp
        if (TqHandleConsoleStatic(req, res)) {
            return;
        }
```

- [ ] **Step 4: Run tests**

Run:

```bash
rtk cmake --build build-glibc --target tcpquic_admin_http_test -j
rtk ./build-glibc/src/tcpquic_admin_http_test
```

Expected: test exits `0`.

- [ ] **Step 5: Commit**

```bash
rtk git add src/runtime/admin_http.cpp src/unittest/admin_http_test.cpp
rtk git commit -m "Serve admin console static routes"
```

---

### Task 3: Allow Explicit Non-Loopback Admin Bind Addresses

**Files:**
- Modify: `src/runtime/admin_http.cpp`
- Modify: `src/unittest/admin_http_test.cpp`

- [ ] **Step 1: Update failing validation tests**

In `src/unittest/admin_http_test.cpp`, replace the existing loopback-only assertions:

```cpp
        if (TqValidateAdminListen("0.0.0.0:19091", err)) return 14;
        if (err.find("loopback") == std::string::npos) return 15;
```

with:

```cpp
        if (!TqValidateAdminListen("0.0.0.0:19091", err)) return 14;
        if (!TqValidateAdminListen("172.16.10.80:19091", err)) return 15;
        if (!TqValidateAdminListen("192.168.1.10:19091", err)) return 326;
```

Add this server start check after the existing `TqAdminHttpServer server("127.0.0.1:0", [&](const TqHttpRequest&) { return TqJsonResponse(200, "{}"); });` startup block:

```cpp
    {
        std::string err;
        TqAdminHttpServer server("0.0.0.0:0", [&](const TqHttpRequest&) {
            return TqJsonResponse(200, "{\"ok\":true}");
        });
        if (!server.Start(err)) return 327;
        if (server.ListenAddress().find("0.0.0.0:") != 0) return 328;
        server.Stop();
    }
```

- [ ] **Step 2: Run and verify failure**

Run:

```bash
rtk cmake --build build-glibc --target tcpquic_admin_http_test -j
rtk ./build-glibc/src/tcpquic_admin_http_test
```

Expected: test fails because `TqValidateAdminListen` and `TqValidateAdminBindListen` still reject non-loopback hosts.

- [ ] **Step 3: Relax listen validation while keeping host:port parsing**

In `src/runtime/admin_http.cpp`, replace `TqValidateAdminBindListen` with:

```cpp
bool TqValidateAdminBindListen(const std::string& listen, std::string& err) {
    TqHostPort hostPort{};
    if (!TqParseHostPort(listen, hostPort, true)) {
        err = "admin listen must be host:port";
        return false;
    }
    if (hostPort.Host.empty()) {
        err = "admin listen host must not be empty";
        return false;
    }
    return true;
}
```

Replace `TqValidateAdminListen` with:

```cpp
bool TqValidateAdminListen(const std::string& listen, std::string& err) {
    auto pos = listen.rfind(':');
    if (pos == std::string::npos || pos == 0 || pos + 1 == listen.size()) {
        err = "admin listen must be host:port";
        return false;
    }
    uint16_t port = 0;
    if (!TqParsePort(listen.substr(pos + 1), port)) {
        err = "admin listen port must be in range 1..65535";
        return false;
    }
    if (listen.substr(0, pos).empty()) {
        err = "admin listen host must not be empty";
        return false;
    }
    return true;
}
```

- [ ] **Step 4: Run tests**

Run:

```bash
rtk cmake --build build-glibc --target tcpquic_admin_http_test -j
rtk ./build-glibc/src/tcpquic_admin_http_test
```

Expected: test exits `0`.

- [ ] **Step 5: Commit**

```bash
rtk git add src/runtime/admin_http.cpp src/unittest/admin_http_test.cpp
rtk git commit -m "Allow explicit admin bind addresses"
```

---

### Task 4: Implement Login, Token Handling, Role Detection, and Refresh Shell

**Files:**
- Modify: `src/runtime/admin_console.cpp`
- Test: `src/unittest/admin_http_test.cpp`

- [ ] **Step 1: Add asset-level tests for login behavior**

Extend the `TqAdminConsoleJs()` asset test from Task 1 with these checks:

```cpp
        if (js.find("username === 'raypx2'") == std::string_view::npos) return 329;
        if (js.find("sessionStorage.setItem('tcpquic_admin_token'") == std::string_view::npos) return 330;
        if (js.find("GET /api/v1/admin") != std::string_view::npos) return 331;
        if (js.find("api('/admin'") == std::string_view::npos) return 332;
        if (js.find("setInterval(refreshCurrentPage, 3000)") == std::string_view::npos) return 333;
```

- [ ] **Step 2: Run and verify failure**

Run:

```bash
rtk cmake --build build-glibc --target tcpquic_admin_http_test -j
rtk ./build-glibc/src/tcpquic_admin_http_test
```

Expected: test fails because login behavior is not fully implemented in `kConsoleJs`.

- [ ] **Step 3: Implement login functions in `kConsoleJs`**

Add these functions to the JS asset:

```javascript
async function login() {
  const username = document.getElementById('login-username').value.trim();
  const token = document.getElementById('login-token').value;
  const error = document.getElementById('login-error');
  error.textContent = '';
  if (username !== 'raypx2') {
    error.textContent = 'invalid username';
    return;
  }
  consoleState.token = token;
  try {
    const admin = await api('/admin');
    sessionStorage.setItem('tcpquic_admin_token', token);
    consoleState.role = admin.role === 'server' ? 'server' : 'client';
    document.getElementById('role-client').classList.toggle('active', consoleState.role === 'client');
    document.getElementById('role-server').classList.toggle('active', consoleState.role === 'server');
    setRole(consoleState.role);
  } catch (err) {
    consoleState.token = '';
    sessionStorage.removeItem('tcpquic_admin_token');
    error.textContent = err.message;
  }
}

function logout() {
  sessionStorage.removeItem('tcpquic_admin_token');
  consoleState.token = '';
  showPage('login');
}

function bootConsole() {
  document.getElementById('login-submit').onclick = login;
  document.getElementById('logout').onclick = logout;
  document.getElementById('refresh-now').onclick = refreshCurrentPage;
  document.getElementById('pause-refresh').onclick = () => {
    consoleState.paused = !consoleState.paused;
    document.getElementById('pause-refresh').textContent = consoleState.paused ? 'Resume refresh' : 'Pause refresh';
  };
  document.getElementById('role-client').onclick = () => setRole('client');
  document.getElementById('role-server').onclick = () => setRole('server');
  renderNav();
  showPage(consoleState.token ? 'client-overview' : 'login');
  if (consoleState.token) refreshCurrentPage();
  setInterval(refreshCurrentPage, 3000);
}

document.addEventListener('DOMContentLoaded', bootConsole);
```

Update the login HTML markup inside `kConsoleHtml` so the login inputs have these ids:

```html
<input id="login-username" value="raypx2">
<input id="login-token" type="password" autocomplete="current-password">
<button class="btn primary" id="login-submit">Login with /api/v1/admin</button>
<div class="callout" id="login-error"></div>
```

- [ ] **Step 4: Run tests**

Run:

```bash
rtk cmake --build build-glibc --target tcpquic_admin_http_test -j
rtk ./build-glibc/src/tcpquic_admin_http_test
```

Expected: test exits `0`.

- [ ] **Step 5: Commit**

```bash
rtk git add src/runtime/admin_console.cpp src/unittest/admin_http_test.cpp
rtk git commit -m "Implement admin console login shell"
```

---

### Task 5: Implement Client Pages Using Existing API Fields

**Files:**
- Modify: `src/runtime/admin_console.cpp`
- Test: `src/unittest/admin_http_test.cpp`

- [ ] **Step 1: Add asset checks for client page field constraints**

Extend the asset test with:

```cpp
        if (html.find("client-overview") == std::string_view::npos) return 334;
        if (html.find("client-peers") == std::string_view::npos) return 335;
        if (html.find("client-connections") == std::string_view::npos) return 336;
        if (html.find("client-tunnels") == std::string_view::npos) return 337;
        if (html.find("peers address - quic_peer") == std::string_view::npos) return 338;
        if (html.find("paths 模式") == std::string_view::npos) return 339;
        if (html.find("127.0.0.1:1080") == std::string_view::npos) return 340;
        if (html.find("127.0.0.1:8080") == std::string_view::npos) return 341;
        if (html.find("connected_at") != std::string_view::npos) return 342;
        if (html.find("disconnected_at") != std::string_view::npos) return 343;
        if (html.find("<th>source</th>") != std::string_view::npos) return 344;
        if (js.find("renderClientOverview") == std::string_view::npos) return 345;
        if (js.find("renderClientPeers") == std::string_view::npos) return 346;
        if (js.find("renderClientConnections") == std::string_view::npos) return 347;
        if (js.find("renderClientTunnels") == std::string_view::npos) return 348;
```

- [ ] **Step 2: Run and verify failure**

Run:

```bash
rtk cmake --build build-glibc --target tcpquic_admin_http_test -j
rtk ./build-glibc/src/tcpquic_admin_http_test
```

Expected: test fails until client render functions and field ids exist.

- [ ] **Step 3: Implement client render functions in JS**

Add these functions to `kConsoleJs`:

```javascript
function sum(items, field) {
  return items.reduce((total, item) => total + Number(item[field] || 0), 0);
}

async function renderClientOverview() {
  const [health, metrics, peers, tunnels] = await Promise.all([
    api('/health'),
    api('/metrics'),
    api('/peers'),
    api('/tunnels')
  ]);
  document.getElementById('health-pill').innerHTML = `<span class="dot ok"></span><strong>health</strong> ${text(health.status)}`;
  const peerList = peers.peers || [];
  document.getElementById('client-overview-peer-count').textContent = peerList.length;
  document.getElementById('client-overview-connection-count').textContent = sum(peerList, 'connection_count');
  document.getElementById('client-overview-tunnel-count').textContent = (tunnels.tunnels || []).length;
  document.getElementById('client-overview-stream-count').textContent = sum(peerList, 'active_streams');
  renderRows(document.getElementById('client-overview-peers'), peerList, ['peer_id','state','enabled','connection_count','connected_connections','active_streams','total_streams','reconnects','socks_listen','http_listen','last_error']);
}

async function renderClientPeers() {
  const data = await api('/peers');
  renderRows(document.getElementById('client-peers-rows'), data.peers || [], ['peer_id','state','enabled','quic_peer','socks_listen','http_listen','connection_count','connected_connections','active_streams','total_streams','reconnects','last_error']);
}

async function renderClientConnections() {
  const peerId = document.getElementById('client-connections-peer').value.trim();
  if (!peerId) return;
  const data = await api(`/peers/${encodeURIComponent(peerId)}/connections`);
  const rows = (data.connections || []).map(row => Object.assign({ peer_id: peerId }, row));
  renderRows(document.getElementById('client-connections-rows'), rows, ['connection_id','peer_id','slot_index','generation','connected','retry_scheduled','state','path','local','peer','active_tunnels','last_error']);
}

async function renderClientTunnels() {
  const data = await api('/tunnels');
  renderRows(document.getElementById('client-tunnels-rows'), data.tunnels || [], ['tunnel_id','peer_id','connection_id','target','state','role','ingress','compress','created_at','duration_ms','tcp_read_bytes','tcp_write_bytes','pending_bytes','relay_backend','worker_index','last_error']);
}
```

Update `refreshPage(id)`:

```javascript
async function refreshPage(id) {
  if (id === 'client-overview') return renderClientOverview();
  if (id === 'client-peers') return renderClientPeers();
  if (id === 'client-connections') return renderClientConnections();
  if (id === 'client-tunnels') return renderClientTunnels();
  if (id === 'server-overview') return renderServerOverview();
  if (id === 'server-peers') return renderServerPeers();
  if (id === 'server-connections') return renderServerConnections();
  if (id === 'server-tunnels') return renderServerTunnels();
  if (id === 'server-acl') return renderServerAcl();
  if (id === 'relay') return renderRelay();
  if (id === 'config') return renderConfig();
  if (id === 'diagnostics') return renderDiagnostics();
}
```

Add matching `id` attributes to client table bodies and metric values in `kConsoleHtml`:

```html
<span class="value" id="client-overview-peer-count">0</span>
<span class="value" id="client-overview-connection-count">0</span>
<span class="value" id="client-overview-tunnel-count">0</span>
<span class="value" id="client-overview-stream-count">0</span>
<tbody id="client-overview-peers"></tbody>
<tbody id="client-peers-rows"></tbody>
<input id="client-connections-peer" value="">
<tbody id="client-connections-rows"></tbody>
<tbody id="client-tunnels-rows"></tbody>
```

- [ ] **Step 4: Implement client peer create/edit/delete handlers**

Add JS functions:

```javascript
function peerFormPayload() {
  const pathsText = document.getElementById('peer-paths').value.trim();
  const forwardsText = document.getElementById('peer-port-forwards').value.trim();
  const payload = {
    peer_id: document.getElementById('peer-id').value.trim(),
    quic_peer: document.getElementById('peer-quic-peer').value.trim(),
    socks_listen: document.getElementById('peer-socks-listen').value.trim(),
    http_listen: document.getElementById('peer-http-listen').value.trim(),
    enabled: document.getElementById('peer-enabled').value === 'true'
  };
  if (pathsText) payload.paths = JSON.parse(pathsText);
  if (forwardsText) payload.port_forwards = JSON.parse(forwardsText);
  return payload;
}

async function createPeer() {
  await api('/peers', { method: 'POST', body: JSON.stringify(peerFormPayload()) });
  await renderClientPeers();
}

async function savePeer() {
  const payload = peerFormPayload();
  await api(`/peers/${encodeURIComponent(payload.peer_id)}`, { method: 'PUT', body: JSON.stringify(payload) });
  await renderClientPeers();
}

async function deletePeer(peerId) {
  await api(`/peers/${encodeURIComponent(peerId)}`, { method: 'DELETE', body: JSON.stringify({ mode: 'reject-if-active' }) });
  await renderClientPeers();
}
```

Wire the Create/Save/Delete buttons without adding Drain or Abort buttons.

- [ ] **Step 5: Run tests**

Run:

```bash
rtk cmake --build build-glibc --target tcpquic_admin_http_test -j
rtk ./build-glibc/src/tcpquic_admin_http_test
```

Expected: test exits `0`.

- [ ] **Step 6: Commit**

```bash
rtk git add src/runtime/admin_console.cpp src/unittest/admin_http_test.cpp
rtk git commit -m "Implement admin console client pages"
```

---

### Task 6: Implement Server Pages and ACL View Using Existing API Fields

**Files:**
- Modify: `src/runtime/admin_console.cpp`
- Test: `src/unittest/admin_http_test.cpp`

- [ ] **Step 1: Add asset checks for server field constraints**

Extend the asset test:

```cpp
        if (html.find("server-overview") == std::string_view::npos) return 349;
        if (html.find("server-peers") == std::string_view::npos) return 350;
        if (html.find("server-connections") == std::string_view::npos) return 351;
        if (html.find("server-tunnels") == std::string_view::npos) return 352;
        if (html.find("server-acl") == std::string_view::npos) return 353;
        if (html.find("remote source") == std::string_view::npos) return 354;
        if (html.find("remote_identity") != std::string_view::npos) return 355;
        if (html.find("first_seen") != std::string_view::npos) return 356;
        if (html.find("last_seen") != std::string_view::npos) return 357;
        if (html.find("transferred_bytes") != std::string_view::npos) return 358;
        if (js.find("renderServerOverview") == std::string_view::npos) return 359;
        if (js.find("renderServerPeers") == std::string_view::npos) return 360;
        if (js.find("renderServerConnections") == std::string_view::npos) return 361;
        if (js.find("renderServerTunnels") == std::string_view::npos) return 362;
        if (js.find("renderServerAcl") == std::string_view::npos) return 363;
```

- [ ] **Step 2: Run and verify failure**

Run:

```bash
rtk cmake --build build-glibc --target tcpquic_admin_http_test -j
rtk ./build-glibc/src/tcpquic_admin_http_test
```

Expected: test fails until server render functions exist.

- [ ] **Step 3: Implement server aggregation helpers**

Add to JS:

```javascript
function peerNameFromRemote(remoteAddress) {
  const host = String(remoteAddress || '').split(':')[0] || 'unknown';
  return `peer-${host}`;
}

function groupServerPeers(connections) {
  const groups = new Map();
  for (const connection of connections) {
    const key = peerNameFromRemote(connection.remote_address);
    const item = groups.get(key) || {
      peer: key,
      remote_address: connection.remote_address,
      connections: 0,
      active_streams: 0,
      total_streams: 0,
      active_tunnels: 0,
      last_error: ''
    };
    item.connections += 1;
    item.active_streams += Number(connection.active_streams || 0);
    item.total_streams += Number(connection.total_streams || 0);
    item.active_tunnels += Number(connection.active_tunnels || 0);
    item.last_error = item.last_error || connection.last_error || '';
    groups.set(key, item);
  }
  return Array.from(groups.values());
}
```

- [ ] **Step 4: Implement server render functions**

Add:

```javascript
async function renderServerOverview() {
  const [metrics, connections, tunnels] = await Promise.all([
    api('/metrics'),
    api('/server/connections'),
    api('/server/tunnels')
  ]);
  const connRows = connections.connections || [];
  const peerRows = groupServerPeers(connRows);
  document.getElementById('server-overview-peer-count').textContent = peerRows.length;
  document.getElementById('server-overview-connection-count').textContent = connRows.length;
  document.getElementById('server-overview-tunnel-count').textContent = (tunnels.tunnels || []).length;
  document.getElementById('server-overview-acl-denied').textContent = text(metrics.acl_denied);
  renderRows(document.getElementById('server-overview-peers'), peerRows, ['peer','remote_address','connections','active_streams','total_streams','active_tunnels','last_error']);
}

async function renderServerPeers() {
  const data = await api('/server/connections');
  renderRows(document.getElementById('server-peers-rows'), groupServerPeers(data.connections || []), ['peer','remote_address','connections','active_streams','total_streams','active_tunnels','last_error']);
}

async function renderServerConnections() {
  const data = await api('/server/connections');
  const rows = (data.connections || []).map(row => Object.assign({ peer: peerNameFromRemote(row.remote_address) }, row));
  renderRows(document.getElementById('server-connections-rows'), rows, ['connection_id','peer','remote_address','state','active_streams','total_streams','active_tunnels','last_error']);
}

async function renderServerTunnels() {
  const data = await api('/server/tunnels');
  renderRows(document.getElementById('server-tunnels-rows'), data.tunnels || [], ['tunnel_id','peer_id','connection_id','state','target','role','duration_ms','active']);
}

async function renderServerAcl() {
  const [config, metrics] = await Promise.all([api('/server/config'), api('/metrics')]);
  renderRows(document.getElementById('server-acl-rules'), [
    { type: 'allow_targets', targets: (config.allow_targets || []).join(', ') },
    { type: 'deny_targets', targets: (config.deny_targets || []).join(', ') }
  ], ['type','targets']);
  document.getElementById('server-acl-denied').textContent = text(metrics.acl_denied);
}
```

Add matching element ids to server HTML:

```html
<span class="value" id="server-overview-peer-count">0</span>
<span class="value" id="server-overview-connection-count">0</span>
<span class="value" id="server-overview-tunnel-count">0</span>
<span class="value" id="server-overview-acl-denied">0</span>
<tbody id="server-overview-peers"></tbody>
<tbody id="server-peers-rows"></tbody>
<tbody id="server-connections-rows"></tbody>
<tbody id="server-tunnels-rows"></tbody>
<tbody id="server-acl-rules"></tbody>
<span class="value" id="server-acl-denied">0</span>
```

- [ ] **Step 5: Run tests**

Run:

```bash
rtk cmake --build build-glibc --target tcpquic_admin_http_test -j
rtk ./build-glibc/src/tcpquic_admin_http_test
```

Expected: test exits `0`.

- [ ] **Step 6: Commit**

```bash
rtk git add src/runtime/admin_console.cpp src/unittest/admin_http_test.cpp
rtk git commit -m "Implement admin console server pages"
```

---

### Task 7: Implement Shared Relay, Config, Diagnostics, and Error Handling

**Files:**
- Modify: `src/runtime/admin_console.cpp`
- Test: `src/unittest/admin_http_test.cpp`

- [ ] **Step 1: Add asset checks**

Extend asset tests:

```cpp
        if (js.find("renderRelay") == std::string_view::npos) return 364;
        if (js.find("renderConfig") == std::string_view::npos) return 365;
        if (js.find("renderDiagnostics") == std::string_view::npos) return 366;
        if (js.find("allocator:dump") == std::string_view::npos) return 367;
        if (js.find("not_supported") == std::string_view::npos) return 368;
        if (js.find("JSON.stringify") == std::string_view::npos) return 369;
```

- [ ] **Step 2: Run and verify failure**

Run:

```bash
rtk cmake --build build-glibc --target tcpquic_admin_http_test -j
rtk ./build-glibc/src/tcpquic_admin_http_test
```

Expected: test fails until shared render functions exist.

- [ ] **Step 3: Implement shared render functions**

Add:

```javascript
async function renderRelay() {
  const metrics = await api('/relay/metrics');
  document.getElementById('relay-backend').textContent = text(metrics.backend);
  document.getElementById('relay-active').textContent = text(metrics.active_relays);
  document.getElementById('relay-pending').textContent = text(metrics.pending_bytes);
  document.getElementById('relay-errors').textContent = text(metrics.errors);
}

async function renderConfig() {
  const path = consoleState.role === 'server' ? '/server/config' : '/config';
  const config = await api(path);
  document.getElementById('config-json').value = JSON.stringify(config, null, 2);
  document.getElementById('config-save').style.display = consoleState.role === 'server' ? 'none' : '';
}

async function saveConfig() {
  const value = document.getElementById('config-json').value;
  JSON.parse(value);
  await api('/config', { method: 'PUT', body: value });
  await renderConfig();
}

async function renderDiagnostics() {
  const diagnostics = await api('/diagnostics');
  document.getElementById('diagnostics-json').textContent = JSON.stringify(diagnostics, null, 2);
}

async function runAllocatorDump() {
  const result = await api('/memory/allocator:dump', { method: 'POST' });
  document.getElementById('allocator-json').textContent = JSON.stringify(result, null, 2);
}

function renderPanelError(elementId, err) {
  const textValue = err && err.message ? err.message : String(err);
  document.getElementById(elementId).textContent = textValue.indexOf('not_supported') >= 0 ? '当前平台不支持该明细' : textValue;
}
```

Add matching HTML ids:

```html
<span class="value" id="relay-backend">-</span>
<span class="value" id="relay-active">0</span>
<span class="value" id="relay-pending">0</span>
<span class="value" id="relay-errors">0</span>
<textarea id="config-json"></textarea>
<button class="btn primary" id="config-save">Save</button>
<pre class="json-preview" id="diagnostics-json"></pre>
<button class="btn danger" id="allocator-dump">Run allocator dump</button>
<pre class="json-preview" id="allocator-json"></pre>
```

Wire:

```javascript
document.getElementById('config-save').onclick = saveConfig;
document.getElementById('allocator-dump').onclick = runAllocatorDump;
```

- [ ] **Step 4: Run tests**

Run:

```bash
rtk cmake --build build-glibc --target tcpquic_admin_http_test -j
rtk ./build-glibc/src/tcpquic_admin_http_test
```

Expected: test exits `0`.

- [ ] **Step 5: Commit**

```bash
rtk git add src/runtime/admin_console.cpp src/unittest/admin_http_test.cpp
rtk git commit -m "Implement shared admin console pages"
```

---

### Task 8: Visual Regression Guard Against v4 Reference

**Files:**
- Modify: `src/unittest/admin_http_test.cpp`
- Create: `scripts/check-admin-console-v4.sh`

- [ ] **Step 1: Add exact static guard checks**

Add checks to `src/unittest/admin_http_test.cpp` ensuring implementation cannot drift from frozen v4 requirements:

```cpp
        if (html.find("server overview") != std::string_view::npos) return 370;
        if (html.find("Server Overview") != std::string_view::npos) return 371;
        if (html.find("Drain</button>") != std::string_view::npos) return 372;
        if (html.find("Abort</button>") != std::string_view::npos) return 373;
        if (html.find("Add highest slot") != std::string_view::npos) return 374;
        if (html.find("<th>connected_at</th>") != std::string_view::npos) return 375;
        if (html.find("<th>disconnected_at</th>") != std::string_view::npos) return 376;
        if (html.find("<th>transferred_bytes</th>") != std::string_view::npos) return 377;
        if (html.find("<strong>listen</strong>") != std::string_view::npos) return 378;
```

- [ ] **Step 2: Run tests**

Run:

```bash
rtk cmake --build build-glibc --target tcpquic_admin_http_test -j
rtk ./build-glibc/src/tcpquic_admin_http_test
```

Expected: test exits `0`.

- [ ] **Step 3: Add shell check for manual review**

Create `scripts/check-admin-console-v4.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

impl="${1:-src/runtime/admin_console.cpp}"
ref="docs/superpowers/specs/2026-06-29-admin-console-interaction-v4.html"

grep -q "raypx2 Admin Console" "$impl"
grep -q "Create/Edit Peer" "$impl"
grep -q "server-acl" "$impl"
grep -q "client-tunnels" "$impl"
grep -q "server-tunnels" "$impl"

if grep -q "<strong>listen</strong>" "$impl"; then
  echo "admin console must not show admin listen in topbar" >&2
  exit 1
fi

if grep -q "remote_identity\\|first_seen\\|last_seen\\|transferred_bytes\\|connected_at\\|disconnected_at" "$impl"; then
  echo "admin console contains fields outside current API contract" >&2
  exit 1
fi

grep -q "Interaction Design v4" "$ref"
echo "admin console v4 guard passed"
```

Make it executable:

```bash
rtk chmod +x scripts/check-admin-console-v4.sh
```

- [ ] **Step 4: Run shell check**

Run:

```bash
rtk scripts/check-admin-console-v4.sh
```

Expected:

```text
admin console v4 guard passed
```

- [ ] **Step 5: Commit**

```bash
rtk git add src/unittest/admin_http_test.cpp scripts/check-admin-console-v4.sh
rtk git commit -m "Guard admin console v4 UI contract"
```

---

### Task 9: End-to-End Admin Console Smoke Test

**Files:**
- Modify: `src/unittest/admin_http_test.cpp`
- Test command only; no production code changes expected unless a prior task missed behavior.

- [ ] **Step 1: Add HTTP smoke test**

Add this block to `src/unittest/admin_http_test.cpp` after the Console static route test:

```cpp
    {
        TqAdminHttpServerOptions options;
        options.AdminThreads = 2;
        options.TokenFile = TqSecureTokenFile("console-smoke").string();
        options.Role = "server";
        std::string err;
        TqAdminHttpServer server("127.0.0.1:0", [&](const TqHttpRequest& req) {
            if (req.Method == "GET" && req.Path == "/health") return TqJsonResponse(200, "{\"role\":\"server\",\"status\":\"healthy\",\"uptime_seconds\":1}");
            if (req.Method == "GET" && req.Path == "/metrics") return TqJsonResponse(200, "{\"role\":\"server\",\"status\":\"healthy\",\"uptime_seconds\":1,\"acl_denied\":0}");
            if (req.Method == "GET" && req.Path == "/server/connections") return TqJsonResponse(200, "{\"connections\":[]}");
            if (req.Method == "GET" && req.Path == "/server/tunnels") return TqJsonResponse(200, "{\"tunnels\":[]}");
            if (req.Method == "GET" && req.Path == "/server/config") return TqJsonResponse(200, "{\"role\":\"server\",\"allow_targets\":[\"0.0.0.0/0\"],\"deny_targets\":[]}");
            return TqJsonResponse(404, "{}");
        }, options);
        if (!server.Start(err)) return 379;
        const uint16_t port = TqPortFromListenAddress(server.ListenAddress());
        if (port == 0) return 380;

        TqSocketHandle htmlFd = TqConnectLocal(port);
        if (!TqSocketValid(htmlFd)) return 381;
        if (!TqSendAll(htmlFd, "GET /console/ HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n")) return 382;
        std::string html;
        if (!TqRecvUntilClosed(htmlFd, html)) return 383;
        TqCloseSocket(htmlFd);
        if (!TqHttpStatusIs(html, 200)) return 384;
        if (html.find("server-acl") == std::string::npos) return 385;

        TqSocketHandle apiFd = TqConnectLocal(port);
        if (!TqSocketValid(apiFd)) return 386;
        const std::string request = "GET /api/v1/server/config HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " +
            server.AuthTokenForTesting() + "\r\n\r\n";
        if (!TqSendAll(apiFd, request)) return 387;
        std::string apiResponse;
        if (!TqRecvUntilClosed(apiFd, apiResponse)) return 388;
        TqCloseSocket(apiFd);
        server.Stop();
        if (!TqHttpStatusIs(apiResponse, 200)) return 389;
        if (apiResponse.find("\"allow_targets\"") == std::string::npos) return 390;
    }
```

- [ ] **Step 2: Run test**

Run:

```bash
rtk cmake --build build-glibc --target tcpquic_admin_http_test -j
rtk ./build-glibc/src/tcpquic_admin_http_test
```

Expected: test exits `0`.

- [ ] **Step 3: Run focused runtime tests**

Run:

```bash
rtk cmake --build build-glibc --target tcpquic_router_runtime_test tcpquic_server_admin_test -j
rtk ./build-glibc/src/tcpquic_router_runtime_test
rtk ./build-glibc/src/tcpquic_server_admin_test
```

Expected: all three commands exit `0`.

- [ ] **Step 4: Commit**

```bash
rtk git add src/unittest/admin_http_test.cpp
rtk git commit -m "Add admin console smoke coverage"
```

---

### Task 10: Final Verification and Documentation Check

**Files:**
- Modify only if verification finds a mismatch:
  - `docs/superpowers/specs/2026-06-29-admin-console-design.md`
  - `docs/superpowers/specs/2026-06-29-admin-console-interaction-v4.html`
  - implementation files from prior tasks

- [ ] **Step 1: Build all affected targets**

Run:

```bash
rtk cmake --build build-glibc --target tcpquic-proxy tcpquic_admin_http_test tcpquic_router_runtime_test tcpquic_server_admin_test -j
```

Expected: build exits `0`.

- [ ] **Step 2: Run affected tests**

Run:

```bash
rtk ./build-glibc/src/tcpquic_admin_http_test
rtk ./build-glibc/src/tcpquic_router_runtime_test
rtk ./build-glibc/src/tcpquic_server_admin_test
rtk scripts/check-admin-console-v4.sh
```

Expected: all commands exit `0`; `scripts/check-admin-console-v4.sh` prints `admin console v4 guard passed`.

- [ ] **Step 3: Manual browser smoke**

Run a server with admin enabled in a local development setup:

```bash
rtk ./build-glibc/src/tcpquic-proxy client --admin-listen 127.0.0.1:18080 --admin-token-file /tmp/tcpquic-admin-token.json --peer 127.0.0.1:4433
```

Open:

```text
http://127.0.0.1:18080/console/
```

Expected:

- Login page loads without API token.
- Username is `raypx2`.
- Token from `/tmp/tcpquic-admin-token.json` logs in.
- Topbar shows `role`、`health`、`refresh` only.
- client navigation shows `Overview`、`Peers`、`Connections`、`Tunnels` plus shared pages.
- Peers page Create/Edit drawer field order matches v4.

- [ ] **Step 4: Check git diff**

Run:

```bash
rtk git status --short
rtk git diff --check
```

Expected:

- `git diff --check` exits `0`.
- Remaining changed files are only files from this plan or pre-existing unrelated user changes.

- [ ] **Step 5: Final commit if any verification fixes were made**

If Step 1-4 required fixes:

```bash
rtk git add src/runtime/admin_console.h src/runtime/admin_console.cpp src/runtime/admin_http.cpp src/CMakeLists.txt src/unittest/admin_http_test.cpp scripts/check-admin-console-v4.sh docs/superpowers/specs/2026-06-29-admin-console-design.md docs/superpowers/specs/2026-06-29-admin-console-interaction-v4.html
rtk git commit -m "Stabilize admin console implementation"
```

If no files changed after Step 1-4, do not create an empty commit.

---

## Self-Review

Spec coverage:

- UI 参照冻结：Task 1 uses the v4 HTML as the source asset; Task 8 adds guards against drift.
- Console routes and auth: Task 2 serves static resources unauthenticated and keeps `/api/v1/*` protected.
- non-loopback admin bind: Task 3 relaxes validation while preserving host:port parsing.
- login/token/sessionStorage: Task 4 implements `raypx2` login and session token handling.
- client pages: Task 5 covers Overview、Peers、Connections、Tunnels with current API fields only.
- server pages and ACL: Task 6 covers Overview、Peers、Connections、Tunnels、ACL with current API fields only.
- shared pages: Task 7 covers Relay、Config、Diagnostics and allocator dump.
- tests and verification: Tasks 8-10 cover static contract, smoke, focused runtime tests, and manual browser validation.

Placeholder scan:

- No `TBD`.
- No open-ended validation instructions.
- No unspecified test commands.
- No task depends on unlisted files.

Type and path consistency:

- New functions are consistently named `TqAdminConsoleHtml`, `TqAdminConsoleCss`, `TqAdminConsoleJs`.
- New files live under `src/runtime/`, matching existing Admin HTTP ownership.
- Test target remains `tcpquic_admin_http_test`, with focused runtime tests `tcpquic_router_runtime_test` and `tcpquic_server_admin_test`.
