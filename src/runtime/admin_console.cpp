#include "admin_console.h"

#include <string>

namespace {

#include "admin_console_pico_css.inc"

constexpr std::string_view kConsoleAdminCss = R"CSS(
    :root {
      color-scheme: light;
      --tq-bg:#f4f6f8;
      --tq-panel:#ffffff;
      --tq-panel-soft:#f9fbfc;
      --tq-sidebar:#111820;
      --tq-sidebar-muted:#a9b6c2;
      --tq-line:#d8dee4;
      --tq-line-strong:#c5ced7;
      --tq-text:#172026;
      --tq-muted:#667380;
      --tq-green:#15803d;
      --tq-red:#b42318;
      --tq-amber:#b45309;
      --tq-blue:#2563eb;
      --tq-ink:#22313a;
      --pico-font-family:Inter,ui-sans-serif,system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;
      --pico-border-radius:0.375rem;
      --pico-form-element-spacing-vertical:0.38rem;
      --pico-form-element-spacing-horizontal:0.55rem;
      --pico-spacing:0.75rem;
      --pico-primary:var(--tq-blue);
      --pico-primary-background:var(--tq-blue);
      --pico-primary-border:var(--tq-blue);
    }
    *{box-sizing:border-box}
    html{background:var(--tq-bg)}
    body{margin:0;background:var(--tq-bg);color:var(--tq-text);letter-spacing:0}
    button,input,select,textarea{font-size:13px}
    button{white-space:nowrap}
    .shell{display:grid;grid-template-columns:232px 1fr;min-height:100vh}
    .sidebar{background:var(--tq-sidebar);color:#eef3f7;padding:18px 14px;display:flex;flex-direction:column;gap:14px}
    .brand{padding:4px 8px 12px;border-bottom:1px solid rgba(255,255,255,.16)}
    .brand h1{font-size:18px;line-height:1.2;margin:0 0 6px;color:#fff}
    .brand p{margin:0;color:var(--tq-sidebar-muted);font-size:12px;line-height:1.45}
    .nav{display:grid;gap:4px}
    .nav button{all:unset;cursor:pointer;border-radius:6px;padding:10px;color:#cad4dd;font-size:13px;display:grid;grid-template-columns:22px 1fr auto;gap:8px;align-items:center}
    .nav button:hover{background:rgba(255,255,255,.08)}
    .nav button:focus-visible{outline:2px solid #7dd3fc;outline-offset:2px}
    .nav button.active{background:#eef3f7;color:#111820;font-weight:650}
    .dot{width:8px;height:8px;border-radius:50%;background:#94a3b8}.dot.ok{background:#22c55e}.dot.warn{background:#f59e0b}.dot.info{background:#38bdf8}
    .main{min-width:0;display:grid;grid-template-rows:auto 1fr}
    .topbar{min-height:58px;background:var(--tq-panel);border-bottom:1px solid var(--tq-line);padding:10px 20px;display:flex;align-items:center;justify-content:space-between;gap:14px}
    .status,.actions,.filters{display:flex;flex-wrap:wrap;align-items:center;gap:8px}
    .pill{display:inline-flex;align-items:center;gap:6px;min-height:26px;padding:4px 8px;border:1px solid var(--tq-line);border-radius:999px;background:#fff;color:var(--tq-muted);font-size:12px;white-space:nowrap}
    .pill strong{color:var(--tq-text)}
    .btn{display:inline-flex;align-items:center;justify-content:center;gap:6px;border:1px solid var(--tq-line);background:#fff;color:var(--tq-text);border-radius:6px;min-height:32px;padding:6px 10px;font-size:13px;cursor:pointer;white-space:nowrap;margin:0}
    .btn:hover{border-color:var(--tq-line-strong);background:#f8fafb}
    .btn.primary{border-color:#1d4ed8;background:var(--tq-blue);color:#fff}.btn.primary:hover{background:#1d4ed8}
    .btn.danger{border-color:#efb4ad;color:var(--tq-red);background:#fff7f6}.btn.danger:hover{background:#feeceb}
    .content{padding:18px 20px 28px;overflow:auto}
    .page{display:none;gap:14px}.page.active{display:grid}
    .title-row{display:flex;justify-content:space-between;align-items:flex-start;gap:16px}
    h2{margin:0 0 4px;font-size:20px;line-height:1.25;color:var(--tq-text)}h3{margin:0 0 8px;font-size:14px;color:var(--tq-text)}
    .subtitle{margin:0;color:var(--tq-muted);font-size:13px;line-height:1.45;max-width:1000px}
    .grid{display:grid;grid-template-columns:repeat(12,minmax(0,1fr));gap:12px}
    .card{background:var(--tq-panel);border:1px solid var(--tq-line);border-radius:8px;padding:14px;min-width:0;box-shadow:0 1px 2px rgba(16,24,40,.04)}
    .span-3{grid-column:span 3}.span-4{grid-column:span 4}.span-5{grid-column:span 5}.span-6{grid-column:span 6}.span-7{grid-column:span 7}.span-8{grid-column:span 8}.span-12{grid-column:span 12}
    .metric{display:grid;gap:6px}.metric .label{color:var(--tq-muted);font-size:12px}.metric .value{font-size:24px;line-height:1.1;font-weight:720;color:var(--tq-text)}.metric .note{color:var(--tq-muted);font-size:12px}
    .toolbar{display:flex;justify-content:space-between;align-items:center;gap:10px;flex-wrap:wrap;margin-bottom:10px}
    input,select,textarea{width:100%;border-color:var(--tq-line);background:#fff;color:var(--tq-text);margin:0}
    textarea{min-height:236px;resize:vertical;font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;line-height:1.5}
    .table-scroll{overflow:auto;background:var(--tq-panel)}
    table{width:100%;border-collapse:collapse;font-size:12px;margin:0}
    th,td{border-bottom:1px solid var(--tq-line);padding:9px 8px;text-align:left;vertical-align:middle;white-space:nowrap}
    th{color:var(--tq-muted);font-weight:650;background:#f8fafb}
    tbody tr:hover{background:#fbfcfd}
    .state{display:inline-flex;align-items:center;gap:6px;padding:3px 7px;border-radius:999px;border:1px solid var(--tq-line);font-size:12px;background:#fff}
    .state.ok{color:var(--tq-green);border-color:#b7e1c1;background:#f1fbf4}.state.warn{color:var(--tq-amber);border-color:#f6d49d;background:#fff8eb}.state.err{color:var(--tq-red);border-color:#f2b8b1;background:#fff6f5}
    .peer-form-panel{display:grid;gap:12px}
    .peer-create-footer{display:flex;justify-content:flex-end}
    .split{display:grid;grid-template-columns:minmax(0,1fr) 420px;gap:12px;align-items:start}
    .drawer{background:var(--tq-panel-soft);border:1px solid var(--tq-line);border-radius:8px;padding:14px;display:grid;gap:12px;min-width:0}
    .form{display:grid;grid-template-columns:1fr 1fr;gap:10px}.field{display:grid;gap:4px}.field.full{grid-column:1/-1}.field label{color:var(--tq-muted);font-size:12px}
    .steps{display:grid;gap:8px;margin:0;padding:0;list-style:none}.steps li{display:grid;grid-template-columns:28px 1fr;gap:8px;align-items:start;font-size:13px;color:var(--tq-ink);line-height:1.4}
    .num{display:inline-flex;width:22px;height:22px;align-items:center;justify-content:center;border-radius:50%;background:#e8eef4;font-size:12px;font-weight:700}
    .callout{border:1px solid #f1cc8f;background:#fff8eb;color:#7c3d00;padding:10px 12px;border-radius:8px;font-size:13px;line-height:1.45}
    .confirm{border:1px solid #f0b4ad;background:#fff7f6;border-radius:8px;padding:12px;display:grid;gap:8px;font-size:13px;line-height:1.45}
    .json-preview{background:#111820;color:#d8e5ed;border-radius:8px;padding:12px;overflow:auto;font-size:12px;line-height:1.5;font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;max-height:280px}
    @media(max-width:1180px){.split{grid-template-columns:1fr}.span-3,.span-4,.span-5,.span-6,.span-7,.span-8{grid-column:span 12}}
    @media(max-width:940px){.shell{grid-template-columns:1fr}.sidebar{position:sticky;top:0;z-index:2}.nav{grid-template-columns:repeat(3,minmax(0,1fr))}.nav button{grid-template-columns:1fr;gap:4px;justify-items:start}.topbar{align-items:flex-start;flex-direction:column}.content{padding:14px}}
  )CSS";

constexpr std::string_view kConsoleHtml = R"HTML(<!doctype html>
<html lang="zh-CN" data-theme="light">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta name="color-scheme" content="light">
  <title>raypx2 Admin Console</title>
  <link rel="stylesheet" href="/console/style.css">
</head>
<body>
  <div class="shell">
    <aside class="sidebar">
      <div class="brand">
        <h1>raypx2 Admin Console</h1>
        <p>Embedded operations console for the local Admin API.</p>
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
            <div class="card span-5"><h3>登录</h3><div class="form"><div class="field full"><label>用户名</label><input id="login-username" value="raypx2"></div><div class="field full"><label>Admin token</label><input id="login-token" type="password" autocomplete="current-password"></div><div class="field full"><button class="btn primary" id="login-submit">Login with /api/v1/admin</button></div><div class="callout" id="login-error"></div></div></div>
            <div class="card span-7"><h3>边界</h3><ul class="steps"><li><span class="num">1</span><span>静态资源无需认证，API 仍强制 Bearer token。</span></li><li><span class="num">2</span><span>token 只保存在 sessionStorage。</span></li><li><span class="num">3</span><span>非 loopback 绑定必须提示管理面暴露风险。</span></li></ul></div>
          </div>
        </section>

        <section id="client-overview" class="page">
          <div class="title-row"><div><h2>Overview - client</h2><p class="subtitle">client 视角有 Overview、Peers、Connections、Tunnels 四个业务页面；Overview 汇总 peer -> connection -> tunnel 层级。</p></div></div>
          <div class="grid">
            <div class="card span-3 metric"><span class="label">Peers</span><span class="value" id="client-overview-peer-count">0</span><span class="note">enabled peers from /api/v1/peers</span></div>
            <div class="card span-3 metric"><span class="label">Connections</span><span class="value" id="client-overview-connection-count">0</span><span class="note">sum of connection_count</span></div>
            <div class="card span-3 metric"><span class="label">Tunnels</span><span class="value" id="client-overview-tunnel-count">0</span><span class="note">from /api/v1/tunnels</span></div>
            <div class="card span-3 metric"><span class="label">Active streams</span><span class="value" id="client-overview-stream-count">0</span><span class="note">sum of peers</span></div>
            <div class="card span-12 table-scroll"><h3>Peer 摘要</h3><table><thead><tr><th>peer_id</th><th>state</th><th>enabled</th><th>connection_count</th><th>connected_connections</th><th>active_streams</th><th>total_streams</th><th>reconnects</th><th>socks_listen</th><th>http_listen</th><th>last_error</th></tr></thead><tbody id="client-overview-peers"></tbody></table></div>
          </div>
        </section>

        <section id="client-peers" class="page">
          <div class="title-row"><div><h2>Peers - client</h2><p class="subtitle">client peer 是配置资源，支持 Create、Edit、Delete；Create/Edit 同一套字段，peer endpoint 支持 quic_peer 地址列表或 paths 两种表达。</p></div></div>
          <div class="card table-scroll"><table><thead><tr><th>peer_id</th><th>state</th><th>enabled</th><th>quic_peer</th><th>socks_listen</th><th>http_listen</th><th>connection_count</th><th>connected_connections</th><th>active_streams</th><th>total_streams</th><th>reconnects</th><th>last_error</th><th>actions</th></tr></thead><tbody id="client-peers-rows"></tbody></table></div>
          <section class="peer-form-panel"><aside class="drawer"><h3>Create/Edit Peer</h3><div class="form"><div class="field full"><label>peer_id</label><input id="peer-id" value=""></div><div class="field full"><label>peers address - quic_peer 模式</label><input id="peer-quic-peer" value=""></div><div class="field full"><label>paths 模式</label><textarea id="peer-paths" style="min-height:96px">[]</textarea></div><div class="field"><label>socks_listen</label><input id="peer-socks-listen" value="127.0.0.1:1080"></div><div class="field"><label>http_listen</label><input id="peer-http-listen" value="127.0.0.1:8080"></div><div class="field full"><label>port_forwards</label><textarea id="peer-port-forwards" style="min-height:72px">[]</textarea></div><div class="field"><label>enabled</label><select id="peer-enabled"><option>true</option><option>false</option></select></div></div><div class="callout">提交时二选一：填写 quic_peer 地址列表，或填写 paths 数组；若 paths 非空，使用 paths 作为连接路径配置。</div><div class="actions"><button class="btn" id="peer-cancel">Cancel</button><button class="btn" id="peer-json-advanced">JSON advanced</button><button class="btn primary" id="peer-save">Create / Save</button></div><div class="confirm"><strong>Delete peer</strong><span>只做 Delete 确认，不在此提供连接控制操作。</span><button class="btn danger" id="peer-delete">Delete</button></div></aside></section>
          <div class="peer-create-footer"><button class="btn primary" id="peer-create">Create Peer</button></div>
        </section>

        <section id="client-connections" class="page">
          <div class="title-row"><div><h2>Connections - client</h2><p class="subtitle">只查询展示，不提供连接控制操作；页面聚合所有 peer 的 /api/v1/peers/{peer_id}/connections。</p></div></div>
          <div class="card table-scroll"><table><thead><tr><th>connection_id</th><th>peer_id</th><th>slot_index</th><th>generation</th><th>connected</th><th>retry_scheduled</th><th>state</th><th>path</th><th>local</th><th>peer</th><th>active_tunnels</th><th>last_error</th><th>actions</th></tr></thead><tbody id="client-connections-rows"></tbody></table></div>
          <div class="card"><h3>Connection detail</h3><pre class="json-preview" id="client-connection-detail">{}</pre></div>
        </section>

        <section id="client-tunnels" class="page">
          <div class="title-row"><div><h2>Tunnels - client</h2><p class="subtitle">只查询展示；只使用当前 /api/v1/tunnels 已返回字段，不展示 source。</p></div></div>
          <div class="card table-scroll"><table><thead><tr><th>tunnel_id</th><th>peer_id</th><th>connection_id</th><th>target</th><th>state</th><th>role</th><th>ingress</th><th>compress</th><th>created_at</th><th>duration_ms</th><th>tcp_read_bytes</th><th>tcp_write_bytes</th><th>pending_bytes</th><th>relay_backend</th><th>worker_index</th><th>last_error</th></tr></thead><tbody id="client-tunnels-rows"></tbody></table></div>
        </section>

        <section id="server-overview" class="page">
          <div class="title-row"><div><h2>Overview - server</h2><p class="subtitle">server 视角同样按 peer -> connection -> tunnel 展示；peer 是运行时聚合概念，不是可配置资源。</p></div></div>
          <div class="grid">
            <div class="card span-3 metric"><span class="label">Peers</span><span class="value" id="server-overview-peer-count">0</span><span class="note">remote peers linked</span></div>
            <div class="card span-3 metric"><span class="label">Connections</span><span class="value" id="server-overview-connection-count">0</span><span class="note">accepted QUIC connections</span></div>
            <div class="card span-3 metric"><span class="label">Tunnels</span><span class="value" id="server-overview-tunnel-count">0</span><span class="note">server-side active</span></div>
            <div class="card span-3 metric"><span class="label">ACL denied</span><span class="value" id="server-overview-acl-denied">0</span><span class="note">aggregate counter</span></div>
            <div class="card span-12 table-scroll"><h3>Peer 摘要</h3><table><thead><tr><th>peer</th><th>remote_address</th><th>connections</th><th>active_streams</th><th>total_streams_opened</th><th>active_tunnels</th><th>last_error</th></tr></thead><tbody id="server-overview-peers"></tbody></table></div>
          </div>
        </section>

        <section id="server-peers" class="page">
          <div class="title-row"><div><h2>Peers - server</h2><p class="subtitle">server peer 是只读运行时视图：展示当前有多少远端 peer 与本 server 有连接，以及每个 peer 下的 connection/tunnel 汇总。</p></div></div>
          <div class="card table-scroll"><table><thead><tr><th>peer</th><th>remote_address</th><th>connections</th><th>active_streams</th><th>total_streams_opened</th><th>active_tunnels</th><th>last_error</th></tr></thead><tbody id="server-peers-rows"></tbody></table></div>
          <div class="callout">server 不支持配置 peer，因此这里没有 Create/Edit/Delete。peer 名称由当前 server connection 的 remote_address 聚合得到。</div>
        </section>

        <section id="server-connections" class="page">
          <div class="title-row"><div><h2>Connections - server</h2><p class="subtitle">server 端连接不展示 Reconnecting；第一版不区分 connecting 和 connected，列表展示当前 server 已登记/可观测连接。</p></div></div>
          <div class="card table-scroll"><table><thead><tr><th>connection_id</th><th>peer</th><th>remote_address</th><th>state</th><th>active_streams</th><th>total_streams_opened</th><th>active_tunnels</th><th>last_error</th></tr></thead><tbody id="server-connections-rows"></tbody></table></div>
          <div class="callout">当前 TqServerConnectionSnapshot 已有 remote_address、state、active/total streams、active_tunnels、last_error；页面不展示未返回的时间或字节字段。</div>
        </section>

        <section id="server-tunnels" class="page">
          <div class="title-row"><div><h2>Tunnels - server</h2><p class="subtitle">只查询展示。当前 server tunnel snapshot 没有 remote source 字段，因此本页不展示 remote source；可通过 connection_id 跳到 Connections 查看 connection remote_address。</p></div></div>
          <div class="card table-scroll"><table><thead><tr><th>tunnel_id</th><th>peer_id</th><th>connection_id</th><th>state</th><th>target</th><th>role</th><th>duration_ms</th><th>active</th></tr></thead><tbody id="server-tunnels-rows"></tbody></table></div>
        </section>

        <section id="server-acl" class="page">
          <div class="title-row"><div><h2>ACL - server</h2><p class="subtitle">server 独有页面。只读展示 server config 中的 allow_targets/deny_targets 和 metrics 中的 acl_denied。</p></div></div>
          <div class="grid">
            <div class="card span-7 table-scroll"><h3>规则摘要</h3><table><thead><tr><th>type</th><th>targets</th></tr></thead><tbody id="server-acl-rules"></tbody></table></div>
            <div class="card span-5"><h3>统计</h3><div class="metric"><span class="label">acl_denied</span><span class="value" id="server-acl-denied">0</span><span class="note">from /api/v1/metrics</span></div></div>
            <div class="card span-12"><div class="callout">当前 API 不提供逐条命中历史或最近拒绝列表，因此首版不展示 matches、last_match、time、reason 等字段。</div></div>
          </div>
        </section>

        <section id="relay" class="page"><div class="title-row"><div><h2>Relay</h2><p class="subtitle">client/server 共享页面，展示 relay backend、worker 聚合、pending bytes 和平台能力边界。</p></div></div><div class="grid"><div class="card span-3 metric"><span class="label">Backend</span><span class="value" id="relay-backend" style="font-size:20px">-</span><span class="note">platform-specific</span></div><div class="card span-3 metric"><span class="label">Active relays</span><span class="value" id="relay-active">-</span><span class="note">workers aggregate</span></div><div class="card span-3 metric"><span class="label">Pending bytes</span><span class="value" id="relay-pending">-</span><span class="note">aggregate</span></div><div class="card span-3 metric"><span class="label">Errors</span><span class="value" id="relay-errors">-</span><span class="note">last refresh</span></div></div></section>
        <section id="config" class="page"><div class="title-row"><div><h2>Config</h2><p class="subtitle">client 可编辑 router config；server 首版只读展示 runtime/server config 和 ACL 配置。</p></div><button class="btn primary" id="config-save">Save config</button></div><div class="split"><div class="card"><textarea id="config-json">{}</textarea></div><aside class="drawer"><h3>提交规则</h3><ul class="steps"><li><span class="num">1</span><span>client 支持 JSON validate/submit。</span></li><li><span class="num">2</span><span>server 配置首版只读，不提供编辑按钮。</span></li></ul></aside></div></section>
        <section id="diagnostics" class="page"><div class="title-row"><div><h2>Diagnostics</h2><p class="subtitle">client/server 共享页面，展示诊断状态和 allocator dump。</p></div></div><div class="grid"><div class="card span-6"><h3>状态</h3><pre class="json-preview" id="diagnostics-json">{}</pre></div><div class="card span-6"><div class="confirm"><strong>Allocator dump</strong><span>显式确认后调用 /api/v1/memory/allocator:dump。</span><button class="btn danger" id="allocator-dump">Run allocator dump</button></div><pre class="json-preview" id="allocator-json">{}</pre></div></div></section>
      </section>
    </main>
  </div>
  <script src="/console/app.js"></script>
</body>
</html>
)HTML";

static constexpr char kConsoleJsStorage[] =
    R"JS_PART1(
    const pageDefs = {
      client: [
        ['login','🔐','Login','info'], ['client-overview','📊','Overview','ok'], ['client-peers','🧭','Peers','ok'], ['client-connections','🔗','Connections','warn'], ['client-tunnels','↔','Tunnels','ok'], ['relay','⚙','Relay','info'], ['config','📝','Config','warn'], ['diagnostics','🧪','Diagnostics','info']
      ],
      server: [
        ['login','🔐','Login','info'], ['server-overview','📊','Overview','ok'], ['server-peers','🧭','Peers','ok'], ['server-connections','🔗','Connections','ok'], ['server-tunnels','↔','Tunnels','ok'], ['server-acl','🛡','ACL','warn'], ['relay','⚙','Relay','info'], ['config','📝','Config','warn'], ['diagnostics','🧪','Diagnostics','info']
      ]
    };

    const consoleState = {
      token: sessionStorage.getItem('tcpquic_admin_token') || '',
      role: 'client',
      page: '',
      peerMode: 'create',
      paused: false,
      refreshTimer: 0
    };

    const nav = document.getElementById('nav');
    const rolePill = document.getElementById('role-pill');
    const healthPill = document.getElementById('health-pill');
    const refreshPill = document.getElementById('refresh-pill');
    const loginError = document.getElementById('login-error');
    const pages = Array.from(document.querySelectorAll('.page'));

    function hasContentType(headers) {
      return Object.keys(headers).some(name => name.toLowerCase() === 'content-type');
    }

    function isPlainJsonBody(body) {
      return Array.isArray(body) || Object.prototype.toString.call(body) === '[object Object]';
    }

    function isSpecialBody(body) {
      return typeof body === 'string' ||
        (typeof FormData !== 'undefined' && body instanceof FormData) ||
        (typeof Blob !== 'undefined' && body instanceof Blob) ||
        (typeof URLSearchParams !== 'undefined' && body instanceof URLSearchParams) ||
        (typeof ArrayBuffer !== 'undefined' && body instanceof ArrayBuffer) ||
        (typeof ArrayBuffer !== 'undefined' && ArrayBuffer.isView && ArrayBuffer.isView(body)) ||
        (typeof ReadableStream !== 'undefined' && body instanceof ReadableStream);
    }

    async function api(path, options = {}) {
      const headers = Object.assign({}, options.headers || {});
      const request = Object.assign({}, options, { headers });
      if (consoleState.token) headers.Authorization = `Bearer ${consoleState.token}`;
      if (request.body !== undefined && request.body !== null) {
        const shouldUseJsonContentType = !isSpecialBody(request.body);
        if (isPlainJsonBody(request.body)) {
          request.body = JSON.stringify(request.body);
        }
        if (shouldUseJsonContentType && !hasContentType(headers)) {
          headers['Content-Type'] = 'application/json';
        }
      }
      const response = await fetch(`/api/v1${path}`, request);
      const raw = await response.text();
      let data = {};
      try {
        data = raw ? JSON.parse(raw) : {};
      } catch (_) {
        data = { raw };
      }
      if (response.status === 401) {
        sessionStorage.removeItem('tcpquic_admin_token');
        consoleState.token = '';
        showPage('login');
      }
      if (!response.ok) {
        const message = data && data.error && data.error.message ? data.error.message :
          (data && typeof data.error === 'string' ? data.error :
          (data && data.message ? data.message : `HTTP ${response.status}`));
        const error = new Error(message);
        error.code = data && data.error && data.error.code ? data.error.code :
          (data && data.code ? data.code : '');
        throw error;
      }
      healthPill.innerHTML = '<span class="dot ok"></span><strong>health</strong> healthy';
      return data;
    }

    function text(value) {
      if (value === undefined || value === null || value === '') return '-';
      if (typeof value === 'object') {
        try {
          return JSON.stringify(value);
        } catch (_) {
          return String(value);
        }
      }
      return String(value);
    }

    function escapeHtml(value) {
      return text(value).replace(/[&<>"']/g, ch => ({
        '&': '&amp;',
        '<': '&lt;',
        '>': '&gt;',
        '"': '&quot;',
        "'": '&#39;'
      }[ch]));
    }

    function renderRows(tbody, rows, columns) {
      if (!tbody) return;
      tbody.innerHTML = rows.map(row => `<tr>${columns.map(column => {
        const value = typeof column === 'function' ? column(row) : row[column];
        return `<td>${escapeHtml(value)}</td>`;
      }).join('')}</tr>`).join('');
    }

    function renderJson(id, value) {
      const element = document.getElementById(id);
      if (element) element.textContent = JSON.stringify(value || {}, null, 2);
    }

    function sum(items, field) {
      return items.reduce((total, item) => total + Number(item[field] || 0), 0);
    }

    function setLoginError(message) {
      loginError.textContent = message || '';
      loginError.style.display = message ? 'block' : 'none';
    }

    function setPeerForm(peer = {}) {
      consoleState.peerMode = peer && peer.peer_id ? 'edit' : 'create';
      document.getElementById('peer-id').value = peer.peer_id || '';
      document.getElementById('peer-quic-peer').value = peer.quic_peer || '';
      document.getElementById('peer-paths').value = JSON.stringify(peer.paths || [], null, 2);
      document.getElementById('peer-socks-listen').value = peer.socks_listen || '127.0.0.1:1080';
      document.getElementById('peer-http-listen').value = peer.http_listen || '127.0.0.1:8080';
      document.getElementById('peer-port-forwards').value = JSON.stringify(peer.port_forwards || [], null, 2);
      document.getElementById('peer-enabled').value = peer.enabled === false ? 'false' : 'true';
    }

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

    function beginCreatePeer() {
      setPeerForm();
      consoleState.peerMode = 'create';
    }

    async function savePeer() {
      const payload = peerFormPayload();
      if (consoleState.peerMode === 'create') {
        await api('/peers', { method: 'POST', body: payload });
      } else {
        await api(`/peers/${encodeURIComponent(payload.peer_id)}`, { method: 'PUT', body: payload });
      }
      consoleState.peerMode = 'edit';
      await renderClientPeers();
    }

    async function deletePeer(peerId) {
      await api(`/peers/${encodeURIComponent(peerId)}`, { method: 'DELETE', body: { mode: 'reject-if-active' } });
      await renderClientPeers();
    }

    async function runClientAction(action) {
      try {
        refreshPill.innerHTML = '<strong>refresh</strong> updating';
        await action();
        refreshPill.innerHTML = '<strong>refresh</strong> 3s auto';
      } catch (error) {
        refreshPill.innerHTML = `<strong>refresh</strong> ${escapeHtml(error.message)}`;
      }
    }

    async function renderClientOverview() {
      const [health, metrics, peers, tunnels] = await Promise.all([
        api('/health'),
        api('/metrics'),
        api('/peers'),
        api('/tunnels')
      ]);
      void metrics;
      document.getElementById('health-pill').innerHTML = `<span class="dot ok"></span><strong>health</strong> ${escapeHtml(health.status)}`;
      const peerList = peers.peers || [];
      document.getElementById('client-overview-peer-count').textContent = peerList.length;
      document.getElementById('client-overview-connection-count').textContent = sum(peerList, 'connection_count');
      document.getElementById('client-overview-tunnel-count').textContent = (tunnels.tunnels || []).length;
      document.getElementById('client-overview-stream-count').textContent = sum(peerList, 'active_streams');
      renderRows(document.getElementById('client-overview-peers'), peerList, ['peer_id','state','enabled','connection_count','connected_connections','active_streams','total_streams','reconnects','socks_listen','http_listen','last_error']);
    }

    async function renderClientPeers() {
      const data = await api('/peers');
      const rows = data.peers || [];
      consoleState.clientPeers = rows;
      const tbody = document.getElementById('client-peers-rows');
      renderRows(tbody, rows, ['peer_id','state','enabled','quic_peer','socks_listen','http_listen','connection_count','connected_connections','active_streams','total_streams','reconnects','last_error']);
      if (!tbody) return;
      tbody.querySelectorAll('tr').forEach((tr, index) => {
        const peerId = rows[index] && rows[index].peer_id ? rows[index].peer_id : '';
        const td = document.createElement('td');
        td.innerHTML = `<button class="btn" data-edit-peer="${escapeHtml(peerId)}">Edit</button> <button class="btn danger" data-delete-peer="${escapeHtml(peerId)}">Delete</button>`;
        tr.appendChild(td);
      });
      tbody.querySelectorAll('[data-edit-peer]').forEach(button => {
        button.onclick = () => {
          const peer = (consoleState.clientPeers || []).find(item => item.peer_id === button.dataset.editPeer);
          if (peer) setPeerForm(peer);
        };
      });
      tbody.querySelectorAll('[data-delete-peer]').forEach(button => {
        button.onclick = () => runClientAction(() => deletePeer(button.dataset.deletePeer));
      });
    }

    async function renderClientConnections() {
      const rows = await loadAllClientConnections();
      const tbody = document.getElementById('client-connections-rows');
      renderRows(tbody, rows, ['connection_id','peer_id','slot_index','generation','connected','retry_scheduled','state','path','local','peer','active_tunnels','last_error']);
      if (!tbody) return;
      tbody.querySelectorAll('tr').forEach((tr, index) => {
        const row = rows[index] || {};
        const td = document.createElement('td');
        td.innerHTML = `<button class="btn" data-peer-id="${escapeHtml(row.peer_id)}" data-connection-id="${escapeHtml(row.connection_id)}">Detail</button>`;
        tr.appendChild(td);
      });
      tbody.querySelectorAll('[data-connection-id]').forEach(button => {
        button.onclick = () => renderClientConnectionDetail(button.dataset.peerId, button.dataset.connectionId);
      });
    }

    async function loadAllClientConnections() {
      const peers = await api('/peers');
      const peerRows = peers.peers || [];
      const nested = await Promise.all(peerRows.map(async peer => {
        const peerId = peer.peer_id || '';
        if (!peerId) return [];
        const data = await api(`/peers/${encodeURIComponent(peerId)}/connections`);
        return (data.connections || []).map(row => Object.assign({}, row, { peer_id: peerId }));
      }));
      return nested.flat();
    }

    async function renderClientConnectionDetail(peerId, connectionId) {
      const data = await api(`/peers/${encodeURIComponent(peerId)}/connections/${encodeURIComponent(connectionId)}`);
      renderJson('client-connection-detail', data);
    }

    async function renderClientTunnels() {
      const data = await api('/tunnels');
)JS_PART1"
    R"JS_PART2(      renderRows(document.getElementById('client-tunnels-rows'), data.tunnels || [], ['tunnel_id','peer_id','connection_id','target','state','role','ingress','compress','created_at','duration_ms','tcp_read_bytes','tcp_write_bytes','pending_bytes','relay_backend','worker_index','last_error']);
    }

    function remoteHostFromAddress(remoteAddress) {
      const value = String(remoteAddress || '').trim();
      if (!value) return 'unknown';
      if (value.startsWith('[')) {
        const end = value.indexOf(']');
        return end > 1 ? value.slice(1, end) : value;
      }
      const firstColon = value.indexOf(':');
      if (firstColon === -1) return value;
      const lastColon = value.lastIndexOf(':');
      if (firstColon === lastColon) return value.slice(0, lastColon) || 'unknown';
      const suffix = value.slice(lastColon + 1);
      if (value[lastColon - 1] !== ':' && /^[0-9]+$/.test(suffix)) {
        return value.slice(0, lastColon) || 'unknown';
      }
      return value;
    }

    function peerNameFromRemote(remoteAddress) {
      return `peer-${peerNameAddressPart(remoteAddress)}`;
    }

    function peerNameAddressPart(remoteAddress) {
      const value = String(remoteAddress || '').trim();
      return value || 'unknown';
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
          total_streams_opened: 0,
          active_tunnels: 0,
          last_error: ''
        };
        item.connections += 1;
        item.active_streams += Number(connection.active_streams || 0);
        item.total_streams_opened += Number(connection.total_streams || 0);
        item.active_tunnels += Number(connection.active_tunnels || 0);
        item.last_error = item.last_error || connection.last_error || '';
        groups.set(key, item);
      }
      return Array.from(groups.values());
    }

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
      renderRows(document.getElementById('server-overview-peers'), peerRows, ['peer','remote_address','connections','active_streams','total_streams_opened','active_tunnels','last_error']);
    }

    async function renderServerPeers() {
      const data = await api('/server/connections');
      renderRows(document.getElementById('server-peers-rows'), groupServerPeers(data.connections || []), ['peer','remote_address','connections','active_streams','total_streams_opened','active_tunnels','last_error']);
    }

    async function renderServerConnections() {
      const data = await api('/server/connections');
      const rows = (data.connections || []).map(row => Object.assign({
        peer: peerNameFromRemote(row.remote_address),
        total_streams_opened: row.total_streams
      }, row));
      renderRows(document.getElementById('server-connections-rows'), rows, ['connection_id','peer','remote_address','state','active_streams','total_streams_opened','active_tunnels','last_error']);
    }

    async function renderServerTunnels() {
      const data = await api('/server/tunnels');
      renderRows(document.getElementById('server-tunnels-rows'), data.tunnels || [], ['tunnel_id','peer_id','connection_id','state','target','role','duration_ms','active']);
    }

    async function renderServerAcl() {
      const [config, metrics] = await Promise.all([api('/server/config'), api('/metrics')]);
      const allowTargets = Array.isArray(config.allow_targets) ? config.allow_targets : [];
      const denyTargets = Array.isArray(config.deny_targets) ? config.deny_targets : [];
      renderRows(document.getElementById('server-acl-rules'), [
        { type: 'allow_targets', targets: allowTargets.join(', ') },
        { type: 'deny_targets', targets: denyTargets.join(', ') }
      ], ['type','targets']);
      document.getElementById('server-acl-denied').textContent = text(metrics.acl_denied);
    }

    function renderPanelError(elementId, err) {
      const element = document.getElementById(elementId);
      if (!element) return;
      const message = err && (err.code === 'not_supported' || err.message === 'not_supported')
        ? '当前平台不支持该明细'
        : text(err && err.message ? err.message : err);
      element.textContent = message;
    }

    function formatJson(data) {
      return JSON.stringify(data, null, 2);
    }

    function setElementText(id, value) {
      const element = document.getElementById(id);
      if (element) element.textContent = text(value);
    }

    function setElementJson(id, value) {
      renderJson(id, value);
    }

    function firstDefined(...values) {
      return values.find(value => value !== undefined && value !== null);
    }

    function platformRelayBackend(data) {
      return firstDefined(
        data.linux_relay_backend,
        data.darwin_relay_backend,
        data.windows_relay_backend,
        data.backend,
        data.relay_backend
      );
    }

    async function renderRelay() {
      try {
        const data = await api('/relay/metrics');
        setElementText('relay-backend', platformRelayBackend(data));
        setElementText('relay-active', firstDefined(data.active_relays, data.active, data.active_count));
        setElementText('relay-pending', firstDefined(data.pending_bytes, data.pending, data.pending_relay_bytes));
        setElementText('relay-errors', firstDefined(data.errors, data.error_count, data.last_error));
      } catch (error) {
        renderPanelError('relay-errors', error);
        throw error;
      }
    }

    async function renderConfig() {
      const saveButton = document.getElementById('config-save');
      const editor = document.getElementById('config-json');
      if (saveButton) {
        saveButton.style.display = consoleState.role === 'server' ? 'none' : '';
      }
      try {
        const data = await api(consoleState.role === 'server' ? '/server/config' : '/config');
        if (editor) editor.value = formatJson(data);
      } catch (error) {
        renderPanelError('config-json', error);
        throw error;
      }
    }

    async function saveConfig() {
      const editor = document.getElementById('config-json');
      const payload = JSON.parse(editor ? editor.value : '{}');
      await api('/config', { method: 'PUT', body: payload });
      await renderConfig();
    }

    async function renderDiagnostics() {
      try {
        const data = await api('/diagnostics');
        setElementJson('diagnostics-json', data);
      } catch (error) {
        renderPanelError('diagnostics-json', error);
        throw error;
      }
    }

    async function runAllocatorDump() {
      try {
        const data = await api('/memory/allocator:dump', { method: 'POST' });
        setElementJson('allocator-json', data);
      } catch (error) {
        renderPanelError('allocator-json', error);
        throw error;
      }
    }

    function overviewPage(role) {
      return role === 'server' ? 'server-overview' : 'client-overview';
    }

    function setRole(role) {
      consoleState.role = role === 'server' ? 'server' : 'client';
      rolePill.innerHTML = `<strong>role</strong> ${consoleState.role}`;
      renderNav();
    }

    function showPage(pageId) {
      const knownPage = pages.some(page => page.id === pageId) ? pageId : overviewPage(consoleState.role);
      consoleState.page = knownPage;
      pages.forEach(page => page.classList.toggle('active', page.id === knownPage));
      Array.from(nav.querySelectorAll('button')).forEach(button => {
        button.classList.toggle('active', button.dataset.page === knownPage);
      });
      sessionStorage.setItem('tcpquic.admin.page', knownPage);
      if (window.recordEvent) window.recordEvent({ type:'click', choice: consoleState.role + ':' + knownPage, text: knownPage });
    }

    function renderNav() {
      nav.innerHTML = '';
      pageDefs[consoleState.role].forEach(([id, icon, label, tone]) => {
        const button = document.createElement('button');
        button.dataset.page = id;
        button.innerHTML = `<span>${icon}</span><span>${label}</span><span class="dot ${tone}"></span>`;
        button.onclick = () => {
          showPage(id);
          refreshCurrentPage();
        };
        nav.appendChild(button);
      });
    }

    async function refreshPage(pageId) {
      if (!consoleState.token || pageId === 'login') return;
      if (pageId === 'client-overview') return renderClientOverview();
      if (pageId === 'client-peers') return renderClientPeers();
      if (pageId === 'client-connections') return renderClientConnections();
      if (pageId === 'client-tunnels') return renderClientTunnels();
      if (pageId === 'server-overview') return renderServerOverview();
      if (pageId === 'server-peers') return renderServerPeers();
      if (pageId === 'server-connections') return renderServerConnections();
      if (pageId === 'server-tunnels') return renderServerTunnels();
      if (pageId === 'server-acl') return renderServerAcl();
      if (pageId === 'relay') return renderRelay();
      if (pageId === 'config') return renderConfig();
      if (pageId === 'diagnostics') return renderDiagnostics();
    }

    async function refreshCurrentPage() {
      if (consoleState.paused || !consoleState.token) return;
      await refreshCurrentPageNow();
    }

    async function refreshCurrentPageNow() {
      if (!consoleState.token) return;
      try {
        refreshPill.innerHTML = '<strong>refresh</strong> updating';
        await refreshPage(consoleState.page);
        refreshPill.innerHTML = '<strong>refresh</strong> 3s auto';
      } catch (error) {
        healthPill.innerHTML = '<span class="dot warn"></span><strong>health</strong> degraded';
        refreshPill.innerHTML = `<strong>refresh</strong> ${escapeHtml(error.message)}`;
      }
    }

    async function login() {
      const username = document.getElementById('login-username').value.trim();
      const token = document.getElementById('login-token').value;
      setLoginError('');
      if (username === 'raypx2') {
        consoleState.token = token;
      } else {
        consoleState.token = '';
        setLoginError('用户名必须是 raypx2。');
        return;
      }
      try {
        const admin = await api('/admin');
        sessionStorage.setItem('tcpquic_admin_token', token);
        setRole(admin.role);
        showPage(overviewPage(consoleState.role));
        await refreshCurrentPage();
      } catch (error) {
        sessionStorage.removeItem('tcpquic_admin_token');
        consoleState.token = '';
        showPage('login');
        setLoginError(error.message);
      }
    }

    function logout() {
      sessionStorage.removeItem('tcpquic_admin_token');
      consoleState.token = '';
      showPage('login');
    }

    async function bootConsole() {
      setLoginError('');
      document.getElementById('login-submit').onclick = login;
      document.getElementById('login-token').onkeydown = event => {
        if (event.key === 'Enter') login();
      };
      document.getElementById('logout').onclick = logout;
)JS_PART2"
    R"JS_PART3(      document.getElementById('refresh-now').onclick = refreshCurrentPageNow;
      document.getElementById('peer-create').onclick = beginCreatePeer;
      document.getElementById('peer-save').onclick = () => runClientAction(savePeer);
      const configSave = document.getElementById('config-save');
      if (configSave) configSave.onclick = () => runClientAction(saveConfig);
      const allocatorDump = document.getElementById('allocator-dump');
      if (allocatorDump) allocatorDump.onclick = () => runClientAction(runAllocatorDump);
      document.getElementById('peer-delete').onclick = () => {
        const peerId = document.getElementById('peer-id').value.trim();
        if (peerId) runClientAction(() => deletePeer(peerId));
      };
      document.getElementById('peer-cancel').onclick = () => setPeerForm();
      document.getElementById('peer-json-advanced').onclick = () => setPeerForm(peerFormPayload());
      document.getElementById('pause-refresh').onclick = () => {
        consoleState.paused = !consoleState.paused;
        document.getElementById('pause-refresh').textContent = consoleState.paused ? 'Resume refresh' : 'Pause refresh';
        refreshPill.innerHTML = consoleState.paused ? '<strong>refresh</strong> paused' : '<strong>refresh</strong> 3s auto';
      };

      const savedPage = sessionStorage.getItem('tcpquic.admin.page');
      if (consoleState.token) {
        try {
          const admin = await api('/admin');
          setRole(admin.role);
          const validPages = pageDefs[consoleState.role].map(([id]) => id);
          showPage(validPages.includes(savedPage) && savedPage !== 'login' ? savedPage : overviewPage(consoleState.role));
          await refreshCurrentPage();
        } catch (_) {
          showPage('login');
        }
      } else {
        showPage('login');
      }
      consoleState.refreshTimer = setInterval(refreshCurrentPage, 3000);
    }

    bootConsole();)JS_PART3";

constexpr std::string_view kConsoleJs(kConsoleJsStorage);

} // namespace

std::string_view TqAdminConsoleHtml() {
    return kConsoleHtml;
}

std::string_view TqAdminConsoleCss() {
    static const std::string css =
        std::string(reinterpret_cast<const char*>(kTqAdminConsolePicoCss), kTqAdminConsolePicoCssLen) +
        "\n" +
        std::string(kConsoleAdminCss);
    return css;
}

std::string_view TqAdminConsoleJs() {
    return kConsoleJs;
}
