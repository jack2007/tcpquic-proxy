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
    .language-select{width:auto;min-width:112px;min-height:32px;margin:0;padding:5px 8px;font-size:13px}
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
    input[type="checkbox"]{width:auto;margin:0}
    textarea{min-height:236px;resize:vertical;font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;line-height:1.5}
    .table-scroll{overflow:auto;background:var(--tq-panel)}
    table{width:100%;border-collapse:collapse;font-size:12px;margin:0}
    th,td{border-bottom:1px solid var(--tq-line);padding:9px 8px;text-align:left;vertical-align:middle;white-space:nowrap}
    th{color:var(--tq-muted);font-weight:650;background:#f8fafb}
    tbody tr:hover{background:#fbfcfd}
    .empty-cell{color:var(--tq-muted);text-align:center;padding:18px 8px;background:#fbfcfd}
    .state{display:inline-flex;align-items:center;gap:6px;padding:3px 7px;border-radius:999px;border:1px solid var(--tq-line);font-size:12px;background:#fff}
    .state.ok{color:var(--tq-green);border-color:#b7e1c1;background:#f1fbf4}.state.warn{color:var(--tq-amber);border-color:#f6d49d;background:#fff8eb}.state.err{color:var(--tq-red);border-color:#f2b8b1;background:#fff6f5}
    .peer-form-panel{display:grid;gap:12px}
    .hidden{display:none!important}
    .split{display:grid;grid-template-columns:minmax(0,1fr) 420px;gap:12px;align-items:start}
    .config-layout{display:grid;grid-template-columns:1fr;gap:12px}
    .drawer{background:var(--tq-panel-soft);border:1px solid var(--tq-line);border-radius:8px;padding:14px;display:grid;gap:12px;min-width:0}
    .form{display:grid;grid-template-columns:1fr 1fr;gap:10px}.field{display:grid;gap:4px}.field.full{grid-column:1/-1}.field label{color:var(--tq-muted);font-size:12px}
    .login-layout{display:grid;justify-content:center;align-content:center;min-height:calc(100vh - 150px);padding:16px 0}
    .login-card{width:min(100%,420px);max-width:420px}
    .login-card h3{font-size:20px;margin-bottom:4px}
    .login-card .subtitle{margin-bottom:18px}
    .overview-grid{grid-template-columns:repeat(2,minmax(0,1fr))}
    .overview-grid .span-6{grid-column:span 1}
    .overview-metric{min-height:150px;align-content:center;padding:22px}
    .overview-metric .value{font-size:34px}
    .peer-form-grid{grid-template-columns:repeat(3,minmax(180px,1fr));align-items:start}
    .diagnostics-layout{display:grid;grid-template-columns:minmax(0,1fr) minmax(0,1fr);gap:12px;align-items:stretch}
    .diagnostics-control-card{display:grid;grid-template-rows:auto auto 1fr auto}
    .diagnostics-control-form{display:grid;gap:11px;align-content:start}
    .diagnostics-switch-row{display:flex;align-items:center;justify-content:space-between;gap:12px;min-height:32px}
    .diagnostics-switch-text{display:grid;gap:2px}.diagnostics-switch-text strong{font-size:13px}.diagnostics-switch-text span{color:var(--tq-muted);font-size:12px}
    .diagnostics-toggle{position:relative;display:inline-flex;align-items:center;width:44px;height:24px;flex:0 0 auto}
    .diagnostics-toggle input{position:absolute;inset:0;opacity:0;cursor:pointer}
    .diagnostics-toggle span{width:44px;height:24px;border:1px solid var(--tq-line-strong);border-radius:999px;background:#e8eef4;transition:background .15s,border-color .15s}
    .diagnostics-toggle span::before{content:"";position:absolute;left:3px;top:3px;width:18px;height:18px;border-radius:50%;background:#fff;box-shadow:0 1px 2px rgba(16,24,40,.18);transition:transform .15s}
    .diagnostics-toggle input:checked + span{background:var(--tq-blue);border-color:var(--tq-blue)}
    .diagnostics-toggle input:checked + span::before{transform:translateX(20px)}
    .diagnostics-save-row{display:flex;gap:8px;justify-content:stretch;border-top:1px solid var(--tq-line);padding-top:12px;margin-top:2px}
    .diagnostics-save-row .btn.primary{width:100%;min-width:132px}
    .diagnostics-allocator-column{display:grid;grid-template-rows:auto 1fr;gap:12px;align-items:stretch}
    .diagnostics-allocator-column .confirm{background:var(--tq-panel-soft);border-color:var(--tq-line);color:var(--tq-text)}
    .allocator-result-card{display:grid}
    .allocator-result-panel{background:var(--tq-panel);border:1px solid var(--tq-line);border-radius:8px;padding:12px;display:grid;gap:6px;font-size:12px;color:var(--tq-muted);align-content:start}
    .allocator-result-panel strong{color:var(--tq-text);font-size:13px}
    .allocator-metric-row{display:grid;grid-template-columns:1fr auto;gap:8px;border-top:1px solid var(--tq-line);padding-top:8px}
    .acl-editor{min-height:180px}
    .acl-status{margin-top:12px}
    .steps{display:grid;gap:8px;margin:0;padding:0;list-style:none}.steps li{display:grid;grid-template-columns:28px 1fr;gap:8px;align-items:start;font-size:13px;color:var(--tq-ink);line-height:1.4}
    .num{display:inline-flex;width:22px;height:22px;align-items:center;justify-content:center;border-radius:50%;background:#e8eef4;font-size:12px;font-weight:700}
    .callout{border:1px solid #f1cc8f;background:#fff8eb;color:#7c3d00;padding:10px 12px;border-radius:8px;font-size:13px;line-height:1.45}
    .confirm{border:1px solid #f0b4ad;background:#fff7f6;border-radius:8px;padding:12px;display:grid;gap:8px;font-size:13px;line-height:1.45}
    .json-preview{background:#f8fafb;color:var(--tq-text);border:1px solid var(--tq-line);border-radius:8px;padding:12px;overflow:auto;font-size:12px;line-height:1.5;font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;max-height:280px}
    #config-json{min-height:420px}
    @media(max-width:1180px){.split{grid-template-columns:1fr}.span-3,.span-4,.span-5,.span-6,.span-7,.span-8{grid-column:span 12}.overview-grid .span-6{grid-column:span 1}}
    @media(max-width:1100px){.peer-form-grid{grid-template-columns:repeat(2,minmax(0,1fr))}}
    @media(max-width:640px){.peer-form-grid{grid-template-columns:1fr}}
    @media(max-width:720px){.diagnostics-layout{grid-template-columns:1fr}.diagnostics-allocator-column{grid-template-rows:auto auto}}
    @media(max-width:940px){.shell{grid-template-columns:1fr}.sidebar{position:sticky;top:0;z-index:2}.nav{grid-template-columns:repeat(3,minmax(0,1fr))}.nav button{grid-template-columns:1fr;gap:4px;justify-items:start}.topbar{align-items:flex-start;flex-direction:column}.content{padding:14px}}
  )CSS";

// Embedded admin HTML must stay split across multiple raw string literals.
// MSVC rejects a single literal longer than ~16380 bytes (error C2026). Adjacent
// literals concatenate at compile time into kConsoleHtmlStorage; do NOT merge
// HTML_PART1/HTML_PART2 back into one R"HTML(...)HTML" block.
static constexpr char kConsoleHtmlStorage[] =
    R"HTML_PART1(<!doctype html>
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
        <div class="actions"><select class="language-select" id="language-select" aria-label="Language"><option value="zh">中文</option><option value="en">English</option></select><button class="btn" id="pause-refresh" data-i18n="action.pauseRefresh">Pause refresh</button><button class="btn" id="refresh-now" data-i18n="action.refreshNow">Refresh now</button><button class="btn danger" id="logout" data-i18n="action.logout">Logout</button></div>
      </header>
      <section class="content">
        <section id="login" class="page">
          <div class="login-layout">
            <div class="card login-card"><h3 data-i18n="login.title">Sign in</h3><p class="subtitle" data-i18n="login.subtitle">Use the admin token from the token JSON file.</p><div class="form"><div class="field full"><label data-i18n="login.username">Username</label><input id="login-username" value="raypx2" autocomplete="username"></div><div class="field full"><label data-i18n="login.token">Admin token</label><input id="login-token" type="password" autocomplete="current-password"></div><div class="field full"><button class="btn primary" id="login-submit" data-i18n="login.submit">Sign in</button></div><div class="callout" id="login-error"></div></div></div>
          </div>
        </section>

        <section id="client-overview" class="page">
          <div class="title-row"><div><h2>Overview - client</h2><p class="subtitle">Overview summarizes peer, connection, tunnel, and stream activity for the client runtime.</p></div></div>
          <div class="grid overview-grid">
            <div class="card span-6 metric overview-metric"><span class="label">Peers</span><span class="value" id="client-overview-peer-count">0</span><span class="note">configured peers and current peer state</span></div>
            <div class="card span-6 metric overview-metric"><span class="label">Connections</span><span class="value" id="client-overview-connection-count">0</span><span class="note">total connection slots across peers</span></div>
            <div class="card span-6 metric overview-metric"><span class="label">Tunnels</span><span class="value" id="client-overview-tunnel-count">0</span><span class="note">active tunnel sessions</span></div>
            <div class="card span-6 metric overview-metric"><span class="label">Active streams</span><span class="value" id="client-overview-stream-count">0</span><span class="note">currently open streams across peers</span></div>
          </div>
        </section>

        <section id="client-peers" class="page">
          <div class="title-row"><div><h2>Peers - client</h2><p class="subtitle">Client peers are configurable entries that can be created, edited, or deleted. Use either quic_peer addresses or paths to describe peer endpoints.</p></div></div>
          <div class="card table-scroll"><table><thead><tr><th>peer_id</th><th>state</th><th>enabled</th><th>quic_peer</th><th>socks_listen</th><th>http_listen</th><th>connection_count</th><th>connected_connections</th><th>active_streams</th><th>total_streams</th><th>reconnects</th><th>last_error</th><th>actions</th></tr></thead><tbody id="client-peers-rows"></tbody><tfoot><tr><td colspan="12"></td><td><button class="btn primary" id="peer-create">Create Peer</button></td></tr></tfoot></table></div>
          <section class="peer-form-panel hidden" id="peer-form-panel"><aside class="drawer"><h3>Create/Edit Peer</h3><div class="form peer-form-grid"><div class="field"><label>peer_id</label><input id="peer-id" value=""></div><div class="field"><label>quic_peer addresses</label><input id="peer-quic-peer" value=""></div><div class="field"><label>connections</label><input id="peer-connections" type="number" min="1" max="128" value="1"></div><div class="field"><label>socks_listen</label><input id="peer-socks-listen" value="127.0.0.1:1080"></div><div class="field"><label>http_listen</label><input id="peer-http-listen" value="127.0.0.1:8080"></div><div class="field"><label>enabled</label><select id="peer-enabled"><option>true</option><option>false</option></select></div><div class="field"><label>Path settings</label><textarea id="peer-paths" style="min-height:88px">[]</textarea></div><div class="field"><label>port_forwards</label><textarea id="peer-port-forwards" style="min-height:88px">[]</textarea></div></div><div class="callout">Provide either quic_peer addresses or a paths array. When paths is not empty, it becomes the connection path configuration.</div><div class="actions"><button class="btn" id="peer-cancel">Cancel</button><button class="btn primary" id="peer-save">Create / Save</button></div></aside></section>
        </section>

        <section id="client-connections" class="page">
          <div class="title-row"><div><h2>Connections - client</h2><p class="subtitle">Read-only connection inventory across all configured client peers. Use the detail action to inspect one connection.</p></div></div>
          <div class="card table-scroll"><table><thead><tr><th>connection_id</th><th>peer_id</th><th>slot_index</th><th>generation</th><th>connected</th><th>retry_scheduled</th><th>state</th><th>encryption</th><th>path</th><th>local</th><th>peer</th><th>active_tunnels</th><th>last_error</th><th>actions</th></tr></thead><tbody id="client-connections-rows"></tbody></table></div>
          <div class="card"><h3>Connection detail</h3><pre class="json-preview" id="client-connection-detail">{}</pre></div>
        </section>

        <section id="client-tunnels" class="page">
          <div class="title-row"><div><h2>Tunnels - client</h2><p class="subtitle">Read-only tunnel inventory for the client runtime. The table shows currently reported tunnel fields and omits unavailable source details.</p></div></div>
          <div class="card table-scroll"><table><thead><tr><th>tunnel_id</th><th>peer_id</th><th>connection_id</th><th>target</th><th>state</th><th>role</th><th>ingress</th><th>compress</th><th>created_at</th><th>duration_ms</th><th>tcp_read_bytes</th><th>tcp_write_bytes</th><th>pending_bytes</th><th>relay_backend</th><th>worker_index</th><th>last_error</th></tr></thead><tbody id="client-tunnels-rows"></tbody></table></div>
        </section>
)HTML_PART1"
    R"HTML_PART2(
        <section id="server-overview" class="page">
          <div class="title-row"><div><h2>Overview - server</h2><p class="subtitle">Overview summarizes peer, connection, tunnel, and ACL activity for the server runtime.</p></div></div>
          <div class="grid overview-grid">
            <div class="card span-6 metric overview-metric"><span class="label">Peers</span><span class="value" id="server-overview-peer-count">0</span><span class="note">remote peers with active server-side state</span></div>
            <div class="card span-6 metric overview-metric"><span class="label">Connections</span><span class="value" id="server-overview-connection-count">0</span><span class="note">accepted QUIC connections</span></div>
            <div class="card span-6 metric overview-metric"><span class="label">Tunnels</span><span class="value" id="server-overview-tunnel-count">0</span><span class="note">server-side tunnel sessions</span></div>
            <div class="card span-6 metric overview-metric"><span class="label">ACL denied</span><span class="value" id="server-overview-acl-denied">0</span><span class="note">aggregate deny counter</span></div>
          </div>
        </section>

        <section id="server-peers" class="page">
          <div class="title-row"><div><h2>Peers - server</h2><p class="subtitle">Server peers are read-only runtime groups built from current server connections. Each row summarizes connection and tunnel activity for one remote peer.</p></div></div>
          <div class="card table-scroll"><table><thead><tr><th>peer</th><th>remote_address</th><th>connections</th><th>active_streams</th><th>total_streams_opened</th><th>active_tunnels</th><th>last_error</th></tr></thead><tbody id="server-peers-rows"></tbody></table></div>
        </section>

        <section id="server-connections" class="page">
          <div class="title-row"><div><h2>Connections - server</h2><p class="subtitle">Read-only list of server-side connections currently registered or observable by the runtime.</p></div></div>
          <div class="card table-scroll"><table><thead><tr><th>connection_id</th><th>peer</th><th>remote_address</th><th>state</th><th>encryption</th><th>active_streams</th><th>total_streams_opened</th><th>active_tunnels</th><th>last_error</th></tr></thead><tbody id="server-connections-rows"></tbody></table></div>
        </section>

        <section id="server-tunnels" class="page">
          <div class="title-row"><div><h2>Tunnels - server</h2><p class="subtitle">Read-only server tunnel inventory. Remote source details are not reported here; use connection_id to correlate a tunnel with connection data.</p></div></div>
          <div class="card table-scroll"><table><thead><tr><th>tunnel_id</th><th>peer_id</th><th>connection_id</th><th>state</th><th>target</th><th>role</th><th>duration_ms</th><th>active</th></tr></thead><tbody id="server-tunnels-rows"></tbody></table></div>
        </section>

        <section id="server-acl" class="page">
          <div class="title-row"><div><h2>ACL - server</h2><p class="subtitle">Edit allow_targets and deny_targets, then save to apply new ACL rules to subsequent tunnels.</p></div><div class="actions"><button class="btn primary" id="server-acl-save">Save ACL</button></div></div>
          <div class="grid">
            <div class="card span-6"><h3>allow_targets</h3><textarea id="server-acl-allow" class="acl-editor" spellcheck="false"></textarea></div>
            <div class="card span-6"><h3>deny_targets</h3><textarea id="server-acl-deny" class="acl-editor" spellcheck="false"></textarea></div>
            <div class="card span-7 table-scroll"><h3>Rule summary</h3><table><thead><tr><th>type</th><th>targets</th></tr></thead><tbody id="server-acl-rules"></tbody></table></div>
            <div class="card span-5"><h3>Status</h3><div class="metric"><span class="label">acl_denied</span><span class="value" id="server-acl-denied">0</span><span class="note">aggregate deny counter</span></div><div class="callout acl-status" id="server-acl-status">Waiting for refresh</div></div>
            <div class="card span-12"><div class="callout">One CIDR per line, or paste a comma-separated list. An empty allow_targets list rejects all regular targets.</div></div>
          </div>
        </section>

        <section id="relay" class="page"><div class="title-row"><div><h2>Relay</h2><p class="subtitle">Relay shows the active relay backend, worker totals, pending bytes, and platform capabilities.</p></div></div><div class="grid"><div class="card span-3 metric"><span class="label">Backend</span><span class="value" id="relay-backend" style="font-size:20px">-</span><span class="note">platform-specific</span></div><div class="card span-3 metric"><span class="label">Active relays</span><span class="value" id="relay-active">-</span><span class="note">worker total</span></div><div class="card span-3 metric"><span class="label">Pending bytes</span><span class="value" id="relay-pending">-</span><span class="note">worker total</span></div><div class="card span-3 metric"><span class="label">Errors</span><span class="value" id="relay-errors">-</span><span class="note">latest refresh</span></div><div class="card span-4"><h3>Capabilities</h3><pre class="json-preview" id="relay-capabilities-json">{}</pre></div><div class="card span-8 table-scroll"><h3>Workers</h3><table><thead><tr><th>worker_id</th><th>backend</th><th>worker_index</th><th>active_relays</th><th>pending_bytes</th><th>tcp_read_bytes</th><th>tcp_write_bytes</th><th>errors</th></tr></thead><tbody id="relay-workers-rows"></tbody></table></div></div></section>
        <section id="config" class="page"><div class="title-row"><div><h2>Config</h2><p class="subtitle">Edit the client router configuration here. Runtime patch updates only safe live fields, while startup-level settings still require a restart.</p></div><div class="actions"><button class="btn" id="runtime-config-save">Patch runtime</button><button class="btn primary" id="config-save">Save config</button></div></div><div class="config-layout"><div class="card"><textarea id="config-json">{}</textarea></div><aside class="drawer"><h3 data-i18n="config.rulesTitle">Submission rules</h3><ul class="steps"><li><span class="num">1</span><span>Router JSON can be validated and submitted from this page.</span></li><li><span class="num">2</span><span>Runtime patch updates only compression and tuning fields that are safe to change live.</span></li></ul></aside></div></section>
        <section id="diagnostics" class="page"><div class="title-row"><div><h2>Diagnostics</h2><p class="subtitle">Diagnostics controls trace and diag_stats updates, while allocator dump remains a separate operations action.</p></div></div><div class="diagnostics-layout"><div class="card diagnostics-control-card"><h3>Diagnostics controls</h3><p class="subtitle">The form reflects the current diagnostics configuration and refreshes after each save.</p><div class="diagnostics-control-form"><div class="diagnostics-switch-row"><div class="diagnostics-switch-text"><strong>trace</strong><span>write trace logs</span></div><label class="diagnostics-toggle" aria-label="trace"><input id="diagnostics-trace" type="checkbox"><span></span></label></div><div class="field"><label>trace_interval_sec</label><input id="diagnostics-trace-interval" type="number" min="1" max="86400"></div><div class="field"><label>trace_level</label><select id="diagnostics-trace-level"><option value="info">info</option><option value="debug">debug</option></select></div><div class="diagnostics-switch-row"><div class="diagnostics-switch-text"><strong>diag_stats</strong><span>periodic stderr statistics</span></div><label class="diagnostics-toggle" aria-label="diag_stats"><input id="diagnostics-diag-stats" type="checkbox"><span></span></label></div><div class="field"><label>diag_stats_interval_sec</label><input id="diagnostics-diag-interval" type="number" min="1" max="86400"></div></div><div class="diagnostics-save-row"><button class="btn primary" id="diagnostics-save">Save diagnostics</button></div></div><div class="diagnostics-allocator-column"><div class="card"><h3>Allocator dump</h3><div class="confirm"><strong>Run explicitly</strong><span>Write allocator statistics to the log and refresh the summary below.</span><button class="btn danger" id="allocator-dump">Run allocator dump</button></div></div><div class="card allocator-result-card"><h3>Allocator result</h3><div class="allocator-result-panel" id="allocator-json"><strong>Waiting for allocator dump</strong><span>Run allocator dump to display the latest summary here.</span></div></div></div></div></section>
      </section>
    </main>
  </div>
  <script src="/console/app.js"></script>
</body>
</html>
)HTML_PART2";

constexpr std::string_view kConsoleHtml(kConsoleHtmlStorage);

// Same MSVC C2026 limit as kConsoleHtmlStorage above. Keep JS_PART1/2/3/4 separate;
// do NOT collapse into one raw string literal.
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

    const translations = {
      en: {
        'action.pauseRefresh': 'Pause refresh',
        'action.resumeRefresh': 'Resume refresh',
        'action.refreshNow': 'Refresh now',
        'action.logout': 'Logout',
        'action.saveAcl': 'Save ACL',
        'action.patchRuntime': 'Patch runtime',
        'action.saveConfig': 'Save config',
        'action.saveDiagnostics': 'Save diagnostics',
        'action.runAllocatorDump': 'Run allocator dump',
        'action.createPeer': 'Create Peer',
        'action.cancel': 'Cancel',
        'action.createSave': 'Create / Save',
        'action.edit': 'Edit',
        'action.delete': 'Delete',
        'action.detail': 'Detail',
        'nav.login': 'Login',
        'nav.client-overview': 'Overview',
        'nav.client-peers': 'Peers',
        'nav.client-connections': 'Connections',
        'nav.client-tunnels': 'Tunnels',
        'nav.server-overview': 'Overview',
        'nav.server-peers': 'Peers',
        'nav.server-connections': 'Connections',
        'nav.server-tunnels': 'Tunnels',
        'nav.server-acl': 'ACL',
        'nav.relay': 'Relay',
        'nav.config': 'Config',
        'nav.diagnostics': 'Diagnostics',
        'brand.subtitle': 'Embedded operations console for the local Admin API.',
        'pill.role': 'role',
        'pill.health': 'health',
        'pill.refresh': 'refresh',
        'status.healthy': 'healthy',
        'status.degraded': 'degraded',
        'status.updating': 'updating',
        'status.paused': 'paused',
        'status.auto3s': '3s auto',
        'login.title': 'Sign in',
        'login.subtitle': 'Use the admin token from the token JSON file.',
        'login.username': 'Username',
        'login.token': 'Admin token',
        'login.submit': 'Sign in',
        'login.usernameError': 'Username must be raypx2.',
        'overview.client.title': 'Overview - client',
        'overview.client.subtitle': 'Overview summarizes peer, connection, tunnel, and stream activity for the client runtime.',
        'overview.server.title': 'Overview - server',
        'overview.server.subtitle': 'Overview summarizes peer, connection, tunnel, and ACL activity for the server runtime.',
        'metric.peers': 'Peers',
        'metric.connections': 'Connections',
        'metric.tunnels': 'Tunnels',
        'metric.activeStreams': 'Active streams',
        'metric.aclDenied': 'ACL denied',
        'metric.clientPeersNote': 'configured peers and current peer state',
        'metric.clientConnectionsNote': 'total connection slots across peers',
        'metric.clientTunnelsNote': 'active tunnel sessions',
        'metric.clientStreamsNote': 'currently open streams across peers',
        'metric.serverPeersNote': 'remote peers with active server-side state',
        'metric.serverConnectionsNote': 'accepted QUIC connections',
        'metric.serverTunnelsNote': 'server-side tunnel sessions',
        'metric.aclDeniedNote': 'aggregate deny counter',
        'clientPeers.title': 'Peers - client',
        'clientPeers.subtitle': 'Client peers are configurable entries that can be created, edited, or deleted. Use either quic_peer addresses or paths to describe peer endpoints.',
        'peerForm.title': 'Create/Edit Peer',
        'peerForm.quicPeer': 'quic_peer addresses',
        'peerForm.paths': 'Path settings',
        'peerForm.callout': 'Provide either quic_peer addresses or a paths array. When paths is not empty, it becomes the connection path configuration.',
        'clientConnections.title': 'Connections - client',
        'clientConnections.subtitle': 'Read-only connection inventory across all configured client peers. Use the detail action to inspect one connection.',
        'connectionDetail.title': 'Connection detail',
        'clientTunnels.title': 'Tunnels - client',
        'clientTunnels.subtitle': 'Read-only tunnel inventory for the client runtime. The table shows currently reported tunnel fields and omits unavailable source details.',
        'serverPeers.title': 'Peers - server',
        'serverPeers.subtitle': 'Server peers are read-only runtime groups built from current server connections. Each row summarizes connection and tunnel activity for one remote peer.',
        'serverConnections.title': 'Connections - server',
        'serverConnections.subtitle': 'Read-only list of server-side connections currently registered or observable by the runtime.',
        'serverTunnels.title': 'Tunnels - server',
        'serverTunnels.subtitle': 'Read-only server tunnel inventory. Remote source details are not reported here; use connection_id to correlate a tunnel with connection data.',
        'acl.title': 'ACL - server',
        'acl.subtitle': 'Edit allow_targets and deny_targets, then save to apply new ACL rules to subsequent tunnels.',
        'acl.ruleSummary': 'Rule summary',
        'acl.status': 'Status',
        'acl.waiting': 'Waiting for refresh',
        'acl.callout': 'One CIDR per line, or paste a comma-separated list. An empty allow_targets list rejects all regular targets.',
        'relay.title': 'Relay',
        'relay.subtitle': 'Relay shows the active relay backend, worker totals, pending bytes, and platform capabilities.',
        'relay.backend': 'Backend',
        'relay.activeRelays': 'Active relays',
        'relay.pendingBytes': 'Pending bytes',
        'relay.errors': 'Errors',
        'relay.platformSpecific': 'platform-specific',
        'relay.workerTotal': 'worker total',
        'relay.latestRefresh': 'latest refresh',
        'relay.capabilities': 'Capabilities',
        'relay.workers': 'Workers',
        'config.title': 'Config',
        'config.subtitle': 'Edit the client router configuration here. Runtime patch updates only safe live fields, while startup-level settings still require a restart.',
        'config.rulesTitle': 'Submission rules',
        'config.ruleJson': 'Router JSON can be validated and submitted from this page.',
        'config.ruleRuntime': 'Runtime patch updates only compression and tuning fields that are safe to change live.',
        'diagnostics.title': 'Diagnostics',
        'diagnostics.subtitle': 'Diagnostics controls trace and diag_stats updates, while allocator dump remains a separate operations action.',
        'diagnostics.controls': 'Diagnostics controls',
        'diagnostics.formSubtitle': 'The form reflects the current diagnostics configuration and refreshes after each save.',
        'diagnostics.traceNote': 'write trace logs',
        'diagnostics.statsNote': 'periodic stderr statistics',
        'allocator.dump': 'Allocator dump',
        'allocator.runExplicitly': 'Run explicitly',
        'allocator.dumpNote': 'Write allocator statistics to the log and refresh the summary below.',
        'allocator.result': 'Allocator result',
        'allocator.waiting': 'Waiting for allocator dump',
        'allocator.waitingNote': 'Run allocator dump to display the latest summary here.',
        'allocator.dumpRequested': 'dump requested',
        'allocator.completed': 'Allocator dump request completed.',
        'error.notSupported': 'This detail is not supported on the current platform.',
        'empty.rows': 'No rows returned',
        'acl.loadedFrom': 'loaded from',
        'acl.runtimeState': 'runtime state',
        'acl.saving': 'saving',
        'acl.saved': 'saved'
      },
      zh: {
        'action.pauseRefresh': '暂停刷新',
        'action.resumeRefresh': '继续刷新',
        'action.refreshNow': '立即刷新',
        'action.logout': '退出登录',
        'action.saveAcl': '保存 ACL',
        'action.patchRuntime': '更新运行时',
        'action.saveConfig': '保存配置',
        'action.saveDiagnostics': '保存诊断配置',
        'action.runAllocatorDump': '执行 allocator dump',
        'action.createPeer': '创建 Peer',
        'action.cancel': '取消',
        'action.createSave': '创建 / 保存',
        'action.edit': '编辑',
        'action.delete': '删除',
        'action.detail': '详情',
        'nav.login': '登录',
        'nav.client-overview': '概览',
        'nav.client-peers': 'Peers',
        'nav.client-connections': '连接',
        'nav.client-tunnels': '隧道',
        'nav.server-overview': '概览',
        'nav.server-peers': 'Peers',
        'nav.server-connections': '连接',
        'nav.server-tunnels': '隧道',
        'nav.server-acl': 'ACL',
        'nav.relay': 'Relay',
        'nav.config': '配置',
        'nav.diagnostics': '诊断',
        'brand.subtitle': '面向本地管理 API 的嵌入式运维控制台。',
        'pill.role': '角色',
        'pill.health': '健康状态',
        'pill.refresh': '刷新',
        'status.healthy': '健康',
        'status.degraded': '降级',
        'status.updating': '刷新中',
        'status.paused': '已暂停',
        'status.auto3s': '3 秒自动',
        'login.title': '登录',
        'login.subtitle': '使用 admin token 文件 JSON 中的 token。',
        'login.username': '用户名',
        'login.token': 'Admin token',
        'login.submit': '登录',
        'login.usernameError': '用户名必须是 raypx2。',
        'overview.client.title': '客户端概览',
        'overview.client.subtitle': '汇总客户端运行时的 peer、连接、隧道和 stream 活动。',
        'overview.server.title': '服务端概览',
        'overview.server.subtitle': '汇总服务端运行时的 peer、连接、隧道和 ACL 活动。',
        'metric.peers': 'Peers',
        'metric.connections': '连接',
        'metric.tunnels': '隧道',
        'metric.activeStreams': '活跃 streams',
        'metric.aclDenied': 'ACL 拒绝',
        'metric.clientPeersNote': '已配置 peer 及当前状态',
        'metric.clientConnectionsNote': '所有 peer 的连接槽位总数',
        'metric.clientTunnelsNote': '活跃隧道会话',
        'metric.clientStreamsNote': '当前打开的 streams',
        'metric.serverPeersNote': '存在服务端运行态的远端 peer',
        'metric.serverConnectionsNote': '已接受的 QUIC 连接',
        'metric.serverTunnelsNote': '服务端隧道会话',
        'metric.aclDeniedNote': '拒绝计数汇总',
        'clientPeers.title': '客户端 Peers',
        'clientPeers.subtitle': '客户端 peer 是可配置资源，可创建、编辑或删除。使用 quic_peer 地址或 paths 描述 peer endpoint。',
        'peerForm.title': '创建/编辑 Peer',
        'peerForm.quicPeer': 'quic_peer 地址',
        'peerForm.paths': '路径配置',
        'peerForm.callout': '填写 quic_peer 地址或 paths 数组二选一。paths 非空时会作为连接路径配置。',
        'clientConnections.title': '客户端连接',
        'clientConnections.subtitle': '展示所有已配置客户端 peer 的只读连接清单，可通过详情操作查看单个连接。',
        'connectionDetail.title': '连接详情',
        'clientTunnels.title': '客户端隧道',
        'clientTunnels.subtitle': '展示客户端运行时的只读隧道清单。表格只显示当前上报字段，并省略不可用的 source 信息。',
        'serverPeers.title': '服务端 Peers',
        'serverPeers.subtitle': '服务端 peer 是由当前服务端连接聚合出的只读运行时分组。每行汇总一个远端 peer 的连接和隧道活动。',
        'serverConnections.title': '服务端连接',
        'serverConnections.subtitle': '展示服务端运行时当前已登记或可观测连接的只读列表。',
        'serverTunnels.title': '服务端隧道',
        'serverTunnels.subtitle': '展示服务端只读隧道清单。本页不报告 remote source，可通过 connection_id 关联连接数据。',
        'acl.title': '服务端 ACL',
        'acl.subtitle': '编辑 allow_targets 和 deny_targets 并保存，后续新建隧道将使用新的 ACL 规则。',
        'acl.ruleSummary': '规则摘要',
        'acl.status': '状态',
        'acl.waiting': '等待刷新',
        'acl.callout': '每行一个 CIDR，也可以粘贴逗号分隔列表。allow_targets 为空时会拒绝所有普通目标。',
        'relay.title': 'Relay',
        'relay.subtitle': '展示当前 relay backend、worker 汇总、pending bytes 和平台能力。',
        'relay.backend': 'Backend',
        'relay.activeRelays': '活跃 relays',
        'relay.pendingBytes': 'Pending bytes',
        'relay.errors': '错误',
        'relay.platformSpecific': '平台相关',
        'relay.workerTotal': 'worker 汇总',
        'relay.latestRefresh': '最近刷新',
        'relay.capabilities': '能力',
        'relay.workers': 'Workers',
        'config.title': '配置',
        'config.subtitle': '在这里编辑客户端 router 配置。运行时更新只修改可安全热更新的字段，启动级配置仍需要重启。',
        'config.rulesTitle': '提交规则',
        'config.ruleJson': '本页可校验并提交 router JSON。',
        'config.ruleRuntime': '运行时更新只提交可安全热更新的 compression 和 tuning 字段。',
        'diagnostics.title': '诊断',
        'diagnostics.subtitle': '诊断页用于更新 trace 和 diag_stats；allocator dump 保持为独立运维动作。',
        'diagnostics.controls': '诊断控制',
        'diagnostics.formSubtitle': '表单反映当前 diagnostics 配置，保存后会刷新控件状态。',
        'diagnostics.traceNote': '写入 trace 日志',
        'diagnostics.statsNote': '周期性 stderr 统计',
        'allocator.dump': 'Allocator dump',
        'allocator.runExplicitly': '显式触发',
        'allocator.dumpNote': '将 allocator 统计写入日志，并刷新下方摘要。',
        'allocator.result': 'Allocator 结果',
        'allocator.waiting': '等待 allocator dump',
        'allocator.waitingNote': '执行 allocator dump 后在这里显示最新摘要。',
        'allocator.dumpRequested': 'dump 已请求',
        'allocator.completed': 'Allocator dump 请求已完成。',
        'error.notSupported': '当前平台不支持该明细。',
        'empty.rows': '没有返回记录',
        'acl.loadedFrom': '加载自',
        'acl.runtimeState': '运行时状态',
        'acl.saving': '保存中',
        'acl.saved': '已保存',
        'column.peer_id': 'peer_id',
        'column.state': '状态',
        'column.enabled': '启用',
        'column.quic_peer': 'quic_peer',
        'column.socks_listen': 'socks_listen',
        'column.http_listen': 'http_listen',
        'column.connection_count': '连接数',
        'column.connected_connections': '已连接',
        'column.active_streams': '活跃 streams',
        'column.total_streams': '总 streams',
        'column.reconnects': '重连次数',
        'column.last_error': '最近错误',
        'column.actions': '操作',
        'column.connection_id': 'connection_id',
        'column.slot_index': 'slot_index',
        'column.generation': 'generation',
        'column.connected': '已连接',
        'column.retry_scheduled': '已计划重试',
        'column.encryption': '加密',
        'column.path': '路径',
        'column.local': '本地',
        'column.peer': 'peer',
        'column.active_tunnels': '活跃隧道',
        'column.tunnel_id': 'tunnel_id',
        'column.target': '目标',
        'column.role': '角色',
        'column.ingress': '入口',
        'column.compress': '压缩',
        'column.created_at': '创建时间',
        'column.duration_ms': '持续时间 ms',
        'column.tcp_read_bytes': 'TCP 读字节',
        'column.tcp_write_bytes': 'TCP 写字节',
        'column.pending_bytes': 'Pending bytes',
        'column.relay_backend': 'relay backend',
        'column.worker_index': 'worker_index',
        'column.remote_address': '远端地址',
        'column.connections': '连接',
        'column.total_streams_opened': '已打开 streams',
        'column.type': '类型',
        'column.targets': '目标',
        'column.worker_id': 'worker_id',
        'column.backend': 'backend',
        'column.active_relays': '活跃 relays',
        'column.errors': '错误'
      }
    };

    const consoleState = {
      token: sessionStorage.getItem('tcpquic_admin_token') || '',
      role: 'client',
      page: '',
      peerMode: 'create',
      paused: false,
      refreshTimer: 0,
      lang: localStorage.getItem('tcpquic.admin.lang') || 'en'
    };

    const nav = document.getElementById('nav');
    const rolePill = document.getElementById('role-pill');
    const healthPill = document.getElementById('health-pill');
    const refreshPill = document.getElementById('refresh-pill');
    const loginError = document.getElementById('login-error');
    const languageSelect = document.getElementById('language-select');
    const pages = Array.from(document.querySelectorAll('.page'));

    function tr(key) {
      const langPack = translations[consoleState.lang] || translations.en;
      return langPack[key] || translations.en[key] || key;
    }

    const i18nSelectors = [
      ['.brand p', 'brand.subtitle'],
      ['#role-pill strong', 'pill.role'],
      ['#health-pill strong', 'pill.health'],
      ['#refresh-pill strong', 'pill.refresh'],
      ['#client-overview h2', 'overview.client.title'],
      ['#client-overview .title-row .subtitle', 'overview.client.subtitle'],
      ['#client-overview .metric:nth-child(1) .label', 'metric.peers'],
      ['#client-overview .metric:nth-child(1) .note', 'metric.clientPeersNote'],
      ['#client-overview .metric:nth-child(2) .label', 'metric.connections'],
      ['#client-overview .metric:nth-child(2) .note', 'metric.clientConnectionsNote'],
      ['#client-overview .metric:nth-child(3) .label', 'metric.tunnels'],
      ['#client-overview .metric:nth-child(3) .note', 'metric.clientTunnelsNote'],
      ['#client-overview .metric:nth-child(4) .label', 'metric.activeStreams'],
      ['#client-overview .metric:nth-child(4) .note', 'metric.clientStreamsNote'],
      ['#server-overview h2', 'overview.server.title'],
      ['#server-overview .title-row .subtitle', 'overview.server.subtitle'],
      ['#server-overview .metric:nth-child(1) .label', 'metric.peers'],
      ['#server-overview .metric:nth-child(1) .note', 'metric.serverPeersNote'],
      ['#server-overview .metric:nth-child(2) .label', 'metric.connections'],
      ['#server-overview .metric:nth-child(2) .note', 'metric.serverConnectionsNote'],
      ['#server-overview .metric:nth-child(3) .label', 'metric.tunnels'],
      ['#server-overview .metric:nth-child(3) .note', 'metric.serverTunnelsNote'],
      ['#server-overview .metric:nth-child(4) .label', 'metric.aclDenied'],
      ['#server-overview .metric:nth-child(4) .note', 'metric.aclDeniedNote'],
      ['#client-peers h2', 'clientPeers.title'],
      ['#client-peers .title-row .subtitle', 'clientPeers.subtitle'],
      ['#peer-create', 'action.createPeer'],
      ['#peer-form-panel h3', 'peerForm.title'],
      ['#peer-form-panel .field:nth-child(2) label', 'peerForm.quicPeer'],
      ['#peer-form-panel .field:nth-child(7) label', 'peerForm.paths'],
      ['#peer-form-panel .callout', 'peerForm.callout'],
      ['#peer-cancel', 'action.cancel'],
      ['#peer-save', 'action.createSave'],
      ['#client-connections h2', 'clientConnections.title'],
      ['#client-connections .title-row .subtitle', 'clientConnections.subtitle'],
      ['#client-connections .card h3', 'connectionDetail.title'],
      ['#client-tunnels h2', 'clientTunnels.title'],
      ['#client-tunnels .title-row .subtitle', 'clientTunnels.subtitle'],
      ['#server-peers h2', 'serverPeers.title'],
      ['#server-peers .title-row .subtitle', 'serverPeers.subtitle'],
      ['#server-connections h2', 'serverConnections.title'],
      ['#server-connections .title-row .subtitle', 'serverConnections.subtitle'],
      ['#server-tunnels h2', 'serverTunnels.title'],
      ['#server-tunnels .title-row .subtitle', 'serverTunnels.subtitle'],
      ['#server-acl h2', 'acl.title'],
      ['#server-acl .title-row .subtitle', 'acl.subtitle'],
      ['#server-acl-save', 'action.saveAcl'],
      ['#server-acl .table-scroll h3', 'acl.ruleSummary'],
      ['#server-acl .span-5 h3', 'acl.status'],
      ['#server-acl .span-5 .note', 'metric.aclDeniedNote'],
      ['#server-acl .span-12 .callout', 'acl.callout'],
      ['#relay h2', 'relay.title'],
      ['#relay .title-row .subtitle', 'relay.subtitle'],
      ['#relay .metric:nth-child(1) .label', 'relay.backend'],
      ['#relay .metric:nth-child(1) .note', 'relay.platformSpecific'],
      ['#relay .metric:nth-child(2) .label', 'relay.activeRelays'],
      ['#relay .metric:nth-child(2) .note', 'relay.workerTotal'],
      ['#relay .metric:nth-child(3) .label', 'relay.pendingBytes'],
      ['#relay .metric:nth-child(3) .note', 'relay.workerTotal'],
      ['#relay .metric:nth-child(4) .label', 'relay.errors'],
      ['#relay .metric:nth-child(4) .note', 'relay.latestRefresh'],
      ['#relay .span-4 h3', 'relay.capabilities'],
      ['#relay .span-8 h3', 'relay.workers'],
      ['#config h2', 'config.title'],
      ['#config .title-row .subtitle', 'config.subtitle'],
      ['#runtime-config-save', 'action.patchRuntime'],
      ['#config-save', 'action.saveConfig'],
      ['#config .steps li:nth-child(1) span:last-child', 'config.ruleJson'],
      ['#config .steps li:nth-child(2) span:last-child', 'config.ruleRuntime'],
      ['#diagnostics h2', 'diagnostics.title'],
      ['#diagnostics .title-row .subtitle', 'diagnostics.subtitle'],
      ['#diagnostics .diagnostics-control-card h3', 'diagnostics.controls'],
      ['#diagnostics .diagnostics-control-card > .subtitle', 'diagnostics.formSubtitle'],
      ['#diagnostics .diagnostics-switch-row:nth-child(1) span', 'diagnostics.traceNote'],
      ['#diagnostics .diagnostics-switch-row:nth-child(4) span', 'diagnostics.statsNote'],
      ['#diagnostics-save', 'action.saveDiagnostics'],
      ['#diagnostics .diagnostics-allocator-column .card:nth-child(1) h3', 'allocator.dump'],
      ['#diagnostics .confirm strong', 'allocator.runExplicitly'],
      ['#diagnostics .confirm span', 'allocator.dumpNote'],
      ['#allocator-dump', 'action.runAllocatorDump'],
      ['#diagnostics .allocator-result-card h3', 'allocator.result'],
      ['#allocator-json strong', 'allocator.waiting'],
      ['#allocator-json span', 'allocator.waitingNote']
    ];

    function applyI18n() {
      document.documentElement.lang = consoleState.lang;
      if (languageSelect) languageSelect.value = consoleState.lang;
      document.querySelectorAll('[data-i18n]').forEach(element => {
        element.textContent = tr(element.dataset.i18n);
      });
      i18nSelectors.forEach(([selector, key]) => {
        const element = document.querySelector(selector);
        if (element) element.textContent = tr(key);
      });
      document.querySelectorAll('th').forEach(header => {
        if (!header.dataset.i18nColumn) header.dataset.i18nColumn = header.textContent;
        const key = 'column.' + header.dataset.i18nColumn;
        const langPack = translations[consoleState.lang] || translations.en;
        header.textContent = langPack[key] || header.dataset.i18nColumn;
      });
      renderNav();
      const pauseButton = document.getElementById('pause-refresh');
      if (pauseButton) pauseButton.textContent = tr(consoleState.paused ? 'action.resumeRefresh' : 'action.pauseRefresh');
    }

    function setLanguage(lang) {
      consoleState.lang = translations[lang] ? lang : 'en';
      localStorage.setItem('tcpquic.admin.lang', lang);
      applyI18n();
    }

    function pill(labelKey, value) {
      return `<strong>${tr(labelKey)}</strong> ${escapeHtml(value)}`;
    }

    function setHealth(value, tone = 'ok') {
      healthPill.innerHTML = `<span class="dot ${tone}"></span>${pill('pill.health', value)}`;
    }

    function setRefresh(value) {
      refreshPill.innerHTML = pill('pill.refresh', value);
    }

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
      setHealth(tr('status.healthy'));
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

    function stateClass(value) {
      const normalized = String(value || '').toLowerCase();
      if (['connected','healthy','ok','enabled','active','running'].some(word => normalized.includes(word))) return 'ok';
      if (['error','failed','denied','disabled'].some(word => normalized.includes(word))) return 'err';
      return 'warn';
    }

    function statusClass(value) {
      return stateClass(value);
    }

    function emptyRow(colspan, message) {
      return `<tr><td colspan="${colspan}" class="empty-cell">${escapeHtml(message || 'No rows returned')}</td></tr>`;
    }

    function formatCell(column, value) {
      if (column === 'state' || column === 'status') {
        return `<span class="state ${statusClass(value)}">${escapeHtml(value)}</span>`;
      }
      if (column === 'encryption') {
        const normalized = String(value || '').toLowerCase();
        if (normalized === 'disabled') {
          return '<span class="state err">disabled</span>';
        }
        if (normalized === 'enabled') {
          return '<span class="state ok">enabled</span>';
        }
        return escapeHtml(value);
      }
      return escapeHtml(value);
    }

    function renderRows(tbody, rows, columns, emptyColspan = columns.length) {
      if (!tbody) return;
      if (!rows.length) {
        tbody.innerHTML = emptyRow(emptyColspan, tr('empty.rows'));
        return;
      }
      tbody.innerHTML = rows.map(row => `<tr>${columns.map(column => {
        const value = typeof column === 'function' ? column(row) : row[column];
        return `<td>${formatCell(column, value)}</td>`;
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
      document.getElementById('peer-connections').value = String(peer.proto_connections || peer.quic_connections || 1);
      document.getElementById('peer-port-forwards').value = JSON.stringify(peer.port_forwards || [], null, 2);
      document.getElementById('peer-enabled').value = peer.enabled === false ? 'false' : 'true';
    }

    function showPeerForm() {
      document.getElementById('peer-form-panel').classList.remove('hidden');
    }

    function hidePeerForm() {
      setPeerForm();
      document.getElementById('peer-form-panel').classList.add('hidden');
    }

    function peerFormPayload() {
      const pathsText = document.getElementById('peer-paths').value.trim();
      const forwardsText = document.getElementById('peer-port-forwards').value.trim();
      const payload = {
        peer_id: document.getElementById('peer-id').value.trim(),
        quic_peer: document.getElementById('peer-quic-peer').value.trim(),
        socks_listen: document.getElementById('peer-socks-listen').value.trim(),
        http_listen: document.getElementById('peer-http-listen').value.trim(),
        proto_connections: Number(document.getElementById('peer-connections').value || 1),
        enabled: document.getElementById('peer-enabled').value === 'true'
      };
      if (pathsText) payload.paths = JSON.parse(pathsText);
      if (forwardsText) payload.port_forwards = JSON.parse(forwardsText);
      return payload;
    }

    function beginCreatePeer() {
      setPeerForm();
      showPeerForm();
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
        setRefresh(tr('status.updating'));
        await action();
        setRefresh(tr('status.auto3s'));
      } catch (error) {
        setRefresh(error.message);
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
      setHealth(tr('status.' + health.status) || health.status);
      const peerList = peers.peers || [];
      document.getElementById('client-overview-peer-count').textContent = peerList.length;
      document.getElementById('client-overview-connection-count').textContent = sum(peerList, 'connection_count');
      document.getElementById('client-overview-tunnel-count').textContent = (tunnels.tunnels || []).length;
      document.getElementById('client-overview-stream-count').textContent = sum(peerList, 'active_streams');
    }

    async function renderClientPeers() {
      const data = await api('/peers');
      const rows = data.peers || [];
      consoleState.clientPeers = rows;
      const tbody = document.getElementById('client-peers-rows');
      renderRows(tbody, rows, ['peer_id','state','enabled','quic_peer','socks_listen','http_listen','connection_count','connected_connections','active_streams','total_streams','reconnects','last_error'], 13);
      if (!tbody || rows.length === 0) return;
      tbody.querySelectorAll('tr').forEach((tr, index) => {
        const peerId = rows[index] && rows[index].peer_id ? rows[index].peer_id : '';
        const td = document.createElement('td');
        td.innerHTML = `<button class="btn" data-edit-peer="${escapeHtml(peerId)}">${escapeHtml(tr('action.edit'))}</button> <button class="btn danger" data-delete-peer="${escapeHtml(peerId)}">${escapeHtml(tr('action.delete'))}</button>`;
        tr.appendChild(td);
      });
      tbody.querySelectorAll('[data-edit-peer]').forEach(button => {
        button.onclick = () => {
          const peer = (consoleState.clientPeers || []).find(item => item.peer_id === button.dataset.editPeer);
          if (peer) {
            setPeerForm(peer);
            showPeerForm();
          }
        };
      });
      tbody.querySelectorAll('[data-delete-peer]').forEach(button => {
        button.onclick = () => runClientAction(() => deletePeer(button.dataset.deletePeer));
      });
    }

    async function renderClientConnections() {
      const rows = await loadAllClientConnections();
      const tbody = document.getElementById('client-connections-rows');
      renderRows(tbody, rows, ['connection_id','peer_id','slot_index','generation','connected','retry_scheduled','state','encryption','path','local','peer','active_tunnels','last_error'], 14);
      if (!tbody || rows.length === 0) return;
      tbody.querySelectorAll('tr').forEach((tr, index) => {
        const row = rows[index] || {};
        const td = document.createElement('td');
        td.innerHTML = `<button class="btn" data-peer-id="${escapeHtml(row.peer_id)}" data-connection-id="${escapeHtml(row.connection_id)}">${escapeHtml(tr('action.detail'))}</button>`;
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

    function peerNameFromConnection(connection) {
      const clientName = String((connection && connection.client_name) || '').trim();
      if (clientName) return clientName;
      return peerNameFromRemote(connection ? connection.remote_address : '');
    }

    function peerNameAddressPart(remoteAddress) {
      const value = String(remoteAddress || '').trim();
      return value || 'unknown';
    }

    function groupServerPeers(connections) {
      const groups = new Map();
      for (const connection of connections) {
        const key = peerNameFromConnection(connection);
        const item = groups.get(key) || {
          peer: key,
          remote_address: '',
          remote_address_set: [],
          connections: 0,
          active_streams: 0,
          total_streams_opened: 0,
          active_tunnels: 0,
          last_error: ''
        };
        if (connection.remote_address && !item.remote_address_set.includes(connection.remote_address)) {
          item.remote_address_set.push(connection.remote_address);
          item.remote_address = formatRemoteAddressSet(item.remote_address_set);
        }
        item.connections += 1;
        item.active_streams += Number(connection.active_streams || 0);
        item.total_streams_opened += Number(connection.total_streams || 0);
        item.active_tunnels += Number(connection.active_tunnels || 0);
        item.last_error = item.last_error || connection.last_error || '';
        groups.set(key, item);
      }
      return Array.from(groups.values());
    }

    function formatRemoteAddressSet(addresses) {
      if (!addresses || addresses.length === 0) return '';
      if (addresses.length === 1) return addresses[0];
      return `${addresses[0]} (+${addresses.length - 1})`;
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
    }

    async function renderServerPeers() {
      const data = await api('/server/connections');
      renderRows(document.getElementById('server-peers-rows'), groupServerPeers(data.connections || []), ['peer','remote_address','connections','active_streams','total_streams_opened','active_tunnels','last_error']);
    }

    async function renderServerConnections() {
      const data = await api('/server/connections');
      const rows = (data.connections || []).map(row => Object.assign({
        peer: peerNameFromConnection(row),
        total_streams_opened: row.total_streams
      }, row));
      renderRows(document.getElementById('server-connections-rows'), rows, ['connection_id','peer','remote_address','state','encryption','active_streams','total_streams_opened','active_tunnels','last_error']);
    }

    async function renderServerTunnels() {
      const data = await api('/server/tunnels');
      renderRows(document.getElementById('server-tunnels-rows'), data.tunnels || [], ['tunnel_id','peer_id','connection_id','state','target','role','duration_ms','active']);
    }

    function parseAclTargets(value) {
      return String(value || '')
        .split(/[\n,]/)
        .map(item => item.trim())
        .filter(Boolean);
    }

    function formatAclTargets(values) {
      return (Array.isArray(values) ? values : []).join('\n');
    }

    function setServerAclForm(config) {
      const allowTargets = Array.isArray(config.allow_targets) ? config.allow_targets : [];
      const denyTargets = Array.isArray(config.deny_targets) ? config.deny_targets : [];
      const allowEditor = document.getElementById('server-acl-allow');
      const denyEditor = document.getElementById('server-acl-deny');
      if (allowEditor) allowEditor.value = formatAclTargets(allowTargets);
      if (denyEditor) denyEditor.value = formatAclTargets(denyTargets);
      renderRows(document.getElementById('server-acl-rules'), [
        { type: 'allow_targets', targets: allowTargets.join(', ') },
        { type: 'deny_targets', targets: denyTargets.join(', ') }
      ], ['type','targets']);
    }

    async function renderServerAcl() {
      const [config, metrics] = await Promise.all([api('/server/config'), api('/metrics')]);
      setServerAclForm(config);
      document.getElementById('server-acl-denied').textContent = text(metrics.acl_denied);
      const status = document.getElementById('server-acl-status');
      if (status) status.textContent = `${tr('acl.loadedFrom')} ${text(config.config_path || tr('acl.runtimeState'))}`;
    }

    async function saveServerAcl() {
      const payload = {
        allow_targets: parseAclTargets(document.getElementById('server-acl-allow').value),
        deny_targets: parseAclTargets(document.getElementById('server-acl-deny').value)
      };
      const status = document.getElementById('server-acl-status');
      if (status) status.textContent = tr('acl.saving');
      const config = await api('/server/config', { method: 'PATCH', body: payload });
      setServerAclForm(config);
      if (status) status.textContent = tr('acl.saved');
    }

    function renderPanelError(elementId, err) {
      const element = document.getElementById(elementId);
      if (!element) return;
      const message = err && (err.code === 'not_supported' || err.message === 'not_supported')
        ? tr('error.notSupported')
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
        data.relay_backend,
        data.linux_relay_backend,
        data.darwin_relay_backend,
        data.windows_relay_backend,
        data.backend,
        'unsupported');
    }

    function renderRelayCapabilities(capabilities) {
      setElementJson('relay-capabilities-json', capabilities || {});
    }

    async function renderRelay() {
      try {
        const [data, workers] = await Promise.all([
          api('/relay/metrics'),
          api('/relay/workers')
        ]);
        setElementText('relay-backend', platformRelayBackend(data));
        setElementText('relay-active', firstDefined(data.active_relays, data.active, data.active_count));
        setElementText('relay-pending', firstDefined(data.pending_bytes, data.pending, data.pending_relay_bytes));
        setElementText('relay-errors', firstDefined(data.errors, data.error_count, data.last_error));
        renderRelayCapabilities(workers.capabilities || {});
        renderRows(document.getElementById('relay-workers-rows'), workers.workers || [], ['worker_id','backend','worker_index','active_relays','pending_bytes','tcp_read_bytes','tcp_write_bytes','errors'], 8);
      } catch (error) {
        renderPanelError('relay-errors', error);
        throw error;
      }
    }
)JS_PART2"
    R"JS_PART3(
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

    async function saveRuntimeConfig() {
      const editor = document.getElementById('config-json');
      const payload = JSON.parse(editor ? editor.value : '{}');
      await api('/runtime/config', { method: 'PATCH', body: payload });
      await renderConfig();
    }

    async function saveConfig() {
      const editor = document.getElementById('config-json');
      const payload = JSON.parse(editor ? editor.value : '{}');
      await api('/config', { method: 'PUT', body: payload });
      await renderConfig();
    }

    async function renderDiagnostics() {
      const data = await api('/diagnostics');
      const trace = document.getElementById('diagnostics-trace');
      const traceInterval = document.getElementById('diagnostics-trace-interval');
      const traceLevel = document.getElementById('diagnostics-trace-level');
      const diagStats = document.getElementById('diagnostics-diag-stats');
      const diagInterval = document.getElementById('diagnostics-diag-interval');
      if (trace) trace.checked = !!data.trace;
      if (traceInterval) traceInterval.value = text(data.trace_interval_sec || 30);
      if (traceLevel) traceLevel.value = text(data.trace_level || 'info');
      if (diagStats) diagStats.checked = !!data.diag_stats;
      if (diagInterval) diagInterval.value = text(data.diag_stats_interval_sec || 5);
    }

    async function saveDiagnostics() {
      const payload = {
        trace: !!document.getElementById('diagnostics-trace').checked,
        trace_interval_sec: Number(document.getElementById('diagnostics-trace-interval').value || 30),
        trace_level: document.getElementById('diagnostics-trace-level').value || 'info',
        diag_stats: !!document.getElementById('diagnostics-diag-stats').checked,
        diag_stats_interval_sec: Number(document.getElementById('diagnostics-diag-interval').value || 5)
      };
      await api('/diagnostics', { method: 'PATCH', body: payload });
      await renderDiagnostics();
    }

    function renderAllocatorResult(data) {
      const element = document.getElementById('allocator-json');
      if (!element) return;
      const source = data && typeof data === 'object' ? data : {};
      const hasValue = value => value !== undefined && value !== null && value !== '';
      const rows = [];
      const addRow = (label, value) => {
        if (!hasValue(value)) return;
        rows.push(`<div class="allocator-metric-row"><span>${escapeHtml(label)}</span><strong>${escapeHtml(value)}</strong></div>`);
      };
      const metricFields = [
        ['bytes', 'bytes'],
        ['total_bytes', 'total_bytes'],
        ['active_bytes', 'active_bytes'],
        ['allocated_bytes', 'allocated_bytes']
      ];
      addRow('status', source.status);
      addRow('allocator', source.allocator);
      addRow('message', source.message);
      metricFields.forEach(([label, key]) => addRow(label, source[key]));
      const summaryTitle = hasValue(source.status) ? source.status : tr('allocator.dumpRequested');
      const summaryText = hasValue(source.message)
        ? source.message
        : (hasValue(source.allocator) ? `Allocator: ${text(source.allocator)}` : tr('allocator.completed'));
      const summary = `<strong>${escapeHtml(summaryTitle)}</strong><span>${escapeHtml(summaryText)}</span>`;
      element.innerHTML = rows.length ? `${summary}${rows.join('')}` : `<strong>${escapeHtml(tr('allocator.dumpRequested'))}</strong><span>${escapeHtml(tr('allocator.completed'))}</span>`;
    }

    async function runAllocatorDump() {
      try {
        const data = await api('/memory/allocator:dump', { method: 'POST' });
        renderAllocatorResult(data);
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
      rolePill.innerHTML = pill('pill.role', consoleState.role);
      renderNav();
    }

    function authenticatedPage(pageId) {
      if (!consoleState.token && pageId !== 'login') return 'login';
      return pages.some(page => page.id === pageId) ? pageId : overviewPage(consoleState.role);
    }

    function showPage(pageId) {
      const knownPage = authenticatedPage(pageId);
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
      const navPages = pageDefs[consoleState.role].filter(([id]) => consoleState.token ? id !== 'login' : id === 'login');
      navPages.forEach(([id, icon, label, tone]) => {
        const button = document.createElement('button');
        button.dataset.page = id;
        button.innerHTML = `<span>${icon}</span><span>${tr('nav.' + id) || label}</span><span class="dot ${tone}"></span>`;
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
        setRefresh(tr('status.updating'));
        await refreshPage(consoleState.page);
        setRefresh(tr('status.auto3s'));
      } catch (error) {
        setHealth(tr('status.degraded'), 'warn');
        setRefresh(error.message);
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
        setLoginError(tr('login.usernameError'));
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
      sessionStorage.removeItem('tcpquic.admin.page');
      consoleState.token = '';
      renderNav();
      showPage('login');
    }

    async function bootConsole() {
      setLoginError('');
      document.getElementById('login-submit').onclick = login;
      document.getElementById('login-token').onkeydown = event => {
        if (event.key === 'Enter') login();
      };
      document.getElementById('logout').onclick = logout;
)JS_PART3"
    R"JS_PART4(      document.getElementById('refresh-now').onclick = refreshCurrentPageNow;
      if (languageSelect) languageSelect.onchange = event => setLanguage(event.target.value);
      document.getElementById('peer-create').onclick = beginCreatePeer;
      document.getElementById('peer-save').onclick = () => runClientAction(savePeer);
      const configSave = document.getElementById('config-save');
      if (configSave) configSave.onclick = () => runClientAction(saveConfig);
      const runtimeConfigSave = document.getElementById('runtime-config-save');
      if (runtimeConfigSave) runtimeConfigSave.onclick = () => runClientAction(saveRuntimeConfig);
      const diagnosticsSave = document.getElementById('diagnostics-save');
      if (diagnosticsSave) diagnosticsSave.onclick = () => runClientAction(saveDiagnostics);
      const serverAclSave = document.getElementById('server-acl-save');
      if (serverAclSave) serverAclSave.onclick = () => runClientAction(saveServerAcl);
      const allocatorDump = document.getElementById('allocator-dump');
      if (allocatorDump) allocatorDump.onclick = () => runClientAction(runAllocatorDump);
      document.getElementById('peer-cancel').onclick = () => hidePeerForm();
      document.getElementById('pause-refresh').onclick = () => {
        consoleState.paused = !consoleState.paused;
        document.getElementById('pause-refresh').textContent = tr(consoleState.paused ? 'action.resumeRefresh' : 'action.pauseRefresh');
        setRefresh(tr(consoleState.paused ? 'status.paused' : 'status.auto3s'));
      };

      applyI18n();
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

    bootConsole();)JS_PART4";

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
