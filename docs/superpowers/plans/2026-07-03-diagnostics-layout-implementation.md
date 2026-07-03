# Diagnostics Layout Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rebuild the shared client/server Diagnostics console page into the confirmed 50/50 layout: left diagnostics controls, right allocator dump/result stack, without duplicated diagnostics JSON state.

**Architecture:** Keep the Admin Console embedded in `src/runtime/admin_console.cpp`. Replace the temporary Diagnostics-specific CSS/HTML with final 50/50 classes, and adjust JS to populate controls only while rendering allocator dump output into a dedicated result panel. Static regression coverage stays in `src/unittest/admin_http_test.cpp`.

**Tech Stack:** C++ embedded string literals for HTML/CSS/JS, static assertions in `tcpquic_admin_http_test`, existing Admin Console helper functions.

---

## File Structure

- Modify: `src/runtime/admin_console.cpp`
  - CSS: replace temporary `.diagnostics-*` rules with final 50/50 layout rules.
  - HTML: replace the current Diagnostics section with the confirmed two-column layout.
  - JS: remove `diagnostics-json` updates and route diagnostics errors to the refresh/status path; allocator output goes to `allocator-json`.
- Modify: `src/unittest/admin_http_test.cpp`
  - Update static assertions to match final class names and absence of `diagnostics-json`.
- Reference only: `docs/superpowers/specs/2026-07-03-diagnostics-layout-design.md`

## Task 1: Static Test For Final Diagnostics Layout

**Files:**
- Modify: `src/unittest/admin_http_test.cpp`

- [ ] **Step 1: Replace old Diagnostics layout assertions with failing assertions for the final layout**

In the first static Console block, replace these current assertions:

```cpp
        if (css.find("input[type=\"checkbox\"]{width:auto;margin:0}") == std::string_view::npos) return 540;
        if (css.find(".diagnostics-form{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px;align-items:end}") == std::string_view::npos) return 541;
        if (css.find(".diagnostics-actions .btn{width:auto;min-width:132px}") == std::string_view::npos) return 542;
```

with:

```cpp
        if (css.find(".diagnostics-layout{display:grid;grid-template-columns:minmax(0,1fr) minmax(0,1fr);gap:12px;align-items:stretch}") == std::string_view::npos) return 540;
        if (css.find(".diagnostics-allocator-column{display:grid;grid-template-rows:auto 1fr;gap:12px;align-items:stretch}") == std::string_view::npos) return 541;
        if (css.find(".diagnostics-control-card{display:grid;grid-template-rows:auto auto 1fr}") == std::string_view::npos) return 542;
        if (css.find(".diagnostics-save-row .btn.primary{width:100%;min-width:132px}") == std::string_view::npos) return 545;
```

Replace these current HTML assertions:

```cpp
        if (html.find("class=\"card span-6 diagnostics-card\"") == std::string_view::npos) return 543;
        if (html.find("class=\"diagnostics-actions\"><button class=\"btn primary\" id=\"diagnostics-save\"") == std::string_view::npos) return 544;
```

with:

```cpp
        if (html.find("class=\"diagnostics-layout\"") == std::string_view::npos) return 543;
        if (html.find("class=\"card diagnostics-control-card\"") == std::string_view::npos) return 544;
        if (html.find("class=\"diagnostics-allocator-column\"") == std::string_view::npos) return 546;
        if (html.find("id=\"diagnostics-json\"") != std::string_view::npos) return 547;
        if (html.find("id=\"allocator-json\"") == std::string_view::npos) return 548;
```

- [ ] **Step 2: Run test and verify it fails before implementation**

Run:

```bash
rtk cmake --build build --target tcpquic_admin_http_test -j2
rtk ./build/bin/Release/tcpquic_admin_http_test
```

Expected: build succeeds, test fails with one of the new return codes `540`, `541`, `542`, `543`, `544`, `545`, `546`, or `547`.

## Task 2: Implement Final Diagnostics CSS

**Files:**
- Modify: `src/runtime/admin_console.cpp`

- [ ] **Step 1: Replace temporary Diagnostics CSS rules**

In `kConsoleCss`, replace this block:

```css
    .diagnostics-card{display:grid;gap:12px}
    .diagnostics-form{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px;align-items:end}
    .check-field{display:flex;align-items:center;gap:8px;min-height:32px;color:var(--tq-text);font-size:13px}
    .check-field input{flex:0 0 auto}
    .diagnostics-actions{grid-column:1/-1;display:flex;justify-content:flex-end;align-items:center;border-top:1px solid var(--tq-line);padding-top:10px}
    .diagnostics-actions .btn{width:auto;min-width:132px}
```

with:

```css
    .diagnostics-layout{display:grid;grid-template-columns:minmax(0,1fr) minmax(0,1fr);gap:12px;align-items:stretch}
    .diagnostics-control-card{display:grid;grid-template-rows:auto auto 1fr}
    .diagnostics-control-form{display:grid;gap:11px;align-content:start}
    .diagnostics-switch-row{display:flex;align-items:center;justify-content:space-between;gap:12px;min-height:32px}
    .diagnostics-switch-text{display:grid;gap:2px}.diagnostics-switch-text strong{font-size:13px}.diagnostics-switch-text span{color:var(--tq-muted);font-size:12px}
    .diagnostics-save-row{display:flex;gap:8px;justify-content:stretch;border-top:1px solid var(--tq-line);padding-top:12px;margin-top:2px}
    .diagnostics-save-row .btn.primary{width:100%;min-width:132px}
    .diagnostics-allocator-column{display:grid;grid-template-rows:auto 1fr;gap:12px;align-items:stretch}
    .allocator-result-card{display:grid}
    .allocator-result-panel{background:var(--tq-panel-soft);border:1px solid var(--tq-line);border-radius:8px;padding:12px;display:grid;gap:6px;font-size:12px;color:var(--tq-muted);align-content:start}
    .allocator-result-panel strong{color:var(--tq-text);font-size:13px}
    .allocator-metric-row{display:grid;grid-template-columns:1fr auto;gap:8px;border-top:1px solid var(--tq-line);padding-top:8px}
```

- [ ] **Step 2: Replace Diagnostics responsive CSS**

Replace:

```css
    @media(max-width:720px){.diagnostics-form{grid-template-columns:1fr}.diagnostics-actions{justify-content:flex-start}}
```

with:

```css
    @media(max-width:720px){.diagnostics-layout{grid-template-columns:1fr}.diagnostics-allocator-column{grid-template-rows:auto auto}}
```

- [ ] **Step 3: Build to verify CSS string compiles**

Run:

```bash
rtk cmake --build build --target tcpquic_admin_http_test -j2
```

Expected: build succeeds.

## Task 3: Replace Diagnostics HTML With 50/50 Layout

**Files:**
- Modify: `src/runtime/admin_console.cpp`

- [ ] **Step 1: Replace the Diagnostics section HTML**

Replace the one-line Diagnostics section that starts with:

```html
        <section id="diagnostics" class="page"><div class="title-row"><div><h2>Diagnostics</h2><p class="subtitle">client/server 共享页面，展示并更新诊断状态和 allocator dump。</p></div></div><div class="grid"><div class="card span-6 diagnostics-card">
```

and ends before:

```html
      </section>
```

with this exact section:

```html
        <section id="diagnostics" class="page"><div class="title-row"><div><h2>Diagnostics</h2><p class="subtitle">client/server 共享页面，更新 trace、diag-stats；allocator dump 作为独立运维动作。</p></div></div><div class="diagnostics-layout"><div class="card diagnostics-control-card"><h3>诊断控制</h3><p class="subtitle">表单值就是当前 diagnostics 配置；保存后直接刷新控件状态。</p><div class="diagnostics-control-form"><div class="diagnostics-switch-row"><div class="diagnostics-switch-text"><strong>trace</strong><span>写入 trace log</span></div><label><input id="diagnostics-trace" type="checkbox"></label></div><div class="field"><label>trace_interval_sec</label><input id="diagnostics-trace-interval" type="number" min="1" max="86400"></div><div class="field"><label>trace_level</label><select id="diagnostics-trace-level"><option value="info">info</option><option value="debug">debug</option></select></div><div class="diagnostics-switch-row"><div class="diagnostics-switch-text"><strong>diag_stats</strong><span>stderr 周期统计</span></div><label><input id="diagnostics-diag-stats" type="checkbox"></label></div><div class="field"><label>diag_stats_interval_sec</label><input id="diagnostics-diag-interval" type="number" min="1" max="86400"></div><div class="diagnostics-save-row"><button class="btn primary" id="diagnostics-save">Save diagnostics</button></div></div></div><div class="diagnostics-allocator-column"><div class="card"><h3>Allocator dump</h3><div class="confirm"><strong>显式触发</strong><span>将 allocator 统计写到日志，并刷新下方结果摘要。</span><button class="btn danger" id="allocator-dump">Run allocator dump</button></div></div><div class="card allocator-result-card"><h3>Allocator 结果</h3><div class="allocator-result-panel" id="allocator-json"><strong>等待 allocator dump</strong><span>点击 Run allocator dump 后显示结果摘要。</span></div></div></div></div></section>
```

- [ ] **Step 2: Build to verify embedded HTML compiles**

Run:

```bash
rtk cmake --build build --target tcpquic_admin_http_test -j2
```

Expected: build succeeds.

## Task 4: Update Diagnostics JavaScript Rendering

**Files:**
- Modify: `src/runtime/admin_console.cpp`

- [ ] **Step 1: Replace `renderDiagnostics()`**

Replace:

```js
    async function renderDiagnostics() {
      try {
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
        setElementJson('diagnostics-json', data);
      } catch (error) {
        renderPanelError('diagnostics-json', error);
        throw error;
      }
    }
```

with:

```js
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
```

- [ ] **Step 2: Replace allocator output rendering**

Replace in `runAllocatorDump()`:

```js
        setElementJson('allocator-json', data);
```

with:

```js
        renderAllocatorResult(data);
```

Add this helper immediately before `async function runAllocatorDump()`:

```js
    function renderAllocatorResult(data) {
      const panel = document.getElementById('allocator-json');
      if (!panel) return;
      const rows = [
        ['status', text(data.status || 'dumped')],
        ['allocator', text(data.allocator || '-')],
        ['enabled', text(data.enabled)],
        ['available', text(data.available)],
        ['requested_current_bytes', text(data.requested_current_bytes || 0)],
        ['reserved_current_bytes', text(data.reserved_current_bytes || 0)],
        ['threads_current', text(data.threads_current || 0)]
      ];
      panel.innerHTML = `<strong>${escapeHtml(rows[1][1])}</strong><span>last allocator dump result</span>` +
        rows.map(([name, value]) => `<div class="allocator-metric-row"><span>${escapeHtml(name)}</span><span>${escapeHtml(value)}</span></div>`).join('');
    }
```

- [ ] **Step 3: Build to verify JS string compiles**

Run:

```bash
rtk cmake --build build --target tcpquic_admin_http_test -j2
```

Expected: build succeeds.

## Task 5: Verify Static Console Test Passes

**Files:**
- Test: `src/unittest/admin_http_test.cpp`

- [ ] **Step 1: Run Admin HTTP test**

Run:

```bash
rtk ./build/bin/Release/tcpquic_admin_http_test
```

Expected: exit code `0`.

- [ ] **Step 2: If test fails, inspect return code**

Run:

```bash
rtk ./build/bin/Release/tcpquic_admin_http_test; printf 'admin_http=%s\n' $?
```

Expected: `admin_http=0`. If not, inspect the matching `return <code>` in `src/unittest/admin_http_test.cpp` and fix the assertion or implementation mismatch.

## Task 6: Optional Visual Smoke Check Against Live Client

**Files:**
- No product file changes.

- [ ] **Step 1: Rebuild binary target that serves the console if needed**

If the live client process is not using the freshly built binary, restart is required before visual verification. If no restart is planned, skip this task and rely on static test.

- [ ] **Step 2: Open the client Admin Console**

Use the token file provided by the user:

```bash
rtk sed -n '1,120p' /run/user/1000/raypx2/client-admin-3147219.json
```

Open the `listen` address from that file in a browser and authenticate with the token value. Navigate to Diagnostics.

Expected: left `诊断控制` card and right allocator column occupy equal width. `Allocator dump` top aligns with left card top; `Allocator 结果` bottom aligns with left card bottom.

## Task 7: Final Verification

**Files:**
- Modified files from Tasks 1-4.

- [ ] **Step 1: Run final build and test**

Run:

```bash
rtk cmake --build build --target tcpquic_admin_http_test -j2
rtk ./build/bin/Release/tcpquic_admin_http_test
```

Expected: both commands exit `0`.

- [ ] **Step 2: Review diff scope**

Run:

```bash
rtk git diff -- src/runtime/admin_console.cpp src/unittest/admin_http_test.cpp docs/superpowers/specs/2026-07-03-diagnostics-layout-design.md docs/superpowers/plans/2026-07-03-diagnostics-layout-implementation.md
```

Expected: diff only covers Diagnostics Console layout, static tests, and the design/plan docs.

---

## Self-Review

- Spec coverage: Tasks 2-4 implement the 50/50 layout, remove diagnostics JSON, preserve client/server shared page, and keep allocator dump/result on the right.
- Test coverage: Tasks 1 and 5 update and run `tcpquic_admin_http_test`.
- Placeholder scan: no TODO/TBD placeholders are present.
- Type/name consistency: class names in Task 1 match CSS/HTML names in Tasks 2-3; `allocator-json` remains the allocator result target.
