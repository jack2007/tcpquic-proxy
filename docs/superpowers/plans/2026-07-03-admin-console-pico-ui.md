# Admin Console Pico UI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refresh the embedded Admin Console UI with locally vendored Pico CSS while preserving the current left navigation and single-binary delivery model.

**Architecture:** Keep the existing `/console/`, `/console/style.css`, and `/console/app.js` handlers. Vendor Pico CSS v2.1.1 into `third_party/pico/`, generate a checked-in C++ byte-array include from `pico.min.css`, and have `TqAdminConsoleCss()` return Pico base CSS followed by project-specific Admin Console overrides. UI changes stay in `src/runtime/admin_console.cpp`; backend API behavior stays unchanged.

**Tech Stack:** C++17, cpp-httplib Admin HTTP server, embedded HTML/CSS/JS string resources, Pico CSS v2.1.1, existing `tcpquic_admin_http_test` executable.

---

## File Structure

- Create: `third_party/pico/pico.min.css`  
  Vendored Pico CSS v2.1.1 source CSS. This is the human-readable source artifact for future upgrades.
- Create: `third_party/pico/LICENSE.md`  
  Pico MIT license copied from the v2.1.1 tag.
- Create: `third_party/pico/VERSION`  
  Contains `v2.1.1` and the upstream URLs used to fetch the files.
- Create: `src/runtime/admin_console_pico_css.inc`  
  Generated C++ byte-array include produced from `third_party/pico/pico.min.css`. This file is included by `admin_console.cpp`, so the final binary does not read CSS from disk.
- Modify: `src/runtime/admin_console.cpp`  
  Combine Pico CSS with Admin Console overrides. Keep the existing embedded HTML and JS in this file.
- Modify: `src/unittest/admin_http_test.cpp`  
  Add assertions proving the embedded CSS includes Pico variables, keeps Admin Console layout classes, and the HTML stays on the chosen left/right layout.

## Task 1: Lock UI Contract With Failing Assertions

**Files:**
- Modify: `src/unittest/admin_http_test.cpp`

- [ ] **Step 1: Add assertions for Pico embedding and retained layout**

In `src/unittest/admin_http_test.cpp`, inside the first block in `main()` that already reads `html`, `css`, and `js`, add these assertions immediately after:

```cpp
        if (css.find(".span-8") == std::string_view::npos) return 309;
```

Add:

```cpp
        if (css.find("--pico-font-family") == std::string_view::npos) return 509;
        if (css.find("--tq-bg") == std::string_view::npos) return 510;
        if (css.find(".shell{display:grid;grid-template-columns:232px 1fr") == std::string_view::npos) return 511;
        if (css.find(".table-scroll") == std::string_view::npos) return 513;
        if (html.find("data-theme=\"light\"") == std::string_view::npos) return 514;
        if (html.find("<aside class=\"sidebar\">") == std::string_view::npos) return 515;
        if (html.find("<main class=\"main\">") == std::string_view::npos) return 516;
```

These assertions intentionally fail before implementation because Pico variables and `data-theme="light"` are not embedded yet. Do not add a second `.sidebar` CSS assertion here; the existing test already checks that class and would make the new return code unreachable.

- [ ] **Step 2: Build and run the focused test to verify failure**

Run:

```bash
rtk cmake --build build --target tcpquic_admin_http_test -j2
rtk ./build/bin/Release/tcpquic_admin_http_test
```

Expected: the executable exits non-zero with return code `509` or `514`, depending on which assertion is reached first. If `build/` does not exist on Linux, configure it first with:

```bash
rtk cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```

Then rerun the two commands above. On Windows, use the existing README flow with `build-x64` and `--config Release`, then run the generated `tcpquic_admin_http_test.exe`.

- [ ] **Step 3: Commit the failing UI contract test**

Run:

```bash
rtk git add src/unittest/admin_http_test.cpp
rtk git commit -m "test: lock admin console pico ui contract"
```

Expected: one commit containing only `src/unittest/admin_http_test.cpp`.

## Task 2: Vendor Pico CSS And Generate Embedded Include

**Files:**
- Create: `third_party/pico/pico.min.css`
- Create: `third_party/pico/LICENSE.md`
- Create: `third_party/pico/VERSION`
- Create: `src/runtime/admin_console_pico_css.inc`

- [ ] **Step 1: Fetch Pico v2.1.1 files into `third_party/pico/`**

Run:

```bash
rtk mkdir -p third_party/pico
rtk curl -L https://raw.githubusercontent.com/picocss/pico/v2.1.1/css/pico.min.css -o third_party/pico/pico.min.css
rtk curl -L https://raw.githubusercontent.com/picocss/pico/v2.1.1/LICENSE.md -o third_party/pico/LICENSE.md
```

Expected: both files exist and `third_party/pico/LICENSE.md` contains the MIT license text for Pico CSS.

- [ ] **Step 2: Record the vendored version**

Create `third_party/pico/VERSION` with exactly:

```text
version: v2.1.1
pico_min_css: https://raw.githubusercontent.com/picocss/pico/v2.1.1/css/pico.min.css
license: https://raw.githubusercontent.com/picocss/pico/v2.1.1/LICENSE.md
```

- [ ] **Step 3: Generate a C++ include from the vendored CSS**

Run:

```bash
rtk xxd -i third_party/pico/pico.min.css > src/runtime/admin_console_pico_css.inc
```

Expected: `src/runtime/admin_console_pico_css.inc` defines:

```cpp
unsigned char third_party_pico_pico_min_css[] = {
```

and:

```cpp
unsigned int third_party_pico_pico_min_css_len =
```

- [ ] **Step 4: Normalize generated symbols for stable C++ use**

Edit `src/runtime/admin_console_pico_css.inc` so the first declaration starts with:

```cpp
static const unsigned char kTqAdminConsolePicoCss[] = {
```

and the length declaration becomes:

```cpp
static const unsigned int kTqAdminConsolePicoCssLen =
```

Do not change the generated byte values.

- [ ] **Step 5: Commit vendored Pico assets**

Run:

```bash
rtk git add third_party/pico/pico.min.css third_party/pico/LICENSE.md third_party/pico/VERSION src/runtime/admin_console_pico_css.inc
rtk git commit -m "vendor: add embedded pico css"
```

Expected: one commit containing only Pico vendor files and the generated C++ include.

## Task 3: Embed Pico In `TqAdminConsoleCss()`

**Files:**
- Modify: `src/runtime/admin_console.cpp`

- [ ] **Step 1: Include `<string>` and the generated Pico include**

At the top of `src/runtime/admin_console.cpp`, change:

```cpp
#include "admin_console.h"

namespace {
```

to:

```cpp
#include "admin_console.h"

#include <string>

namespace {

#include "admin_console_pico_css.inc"
```

- [ ] **Step 2: Rename current CSS string to Admin override CSS**

Change:

```cpp
constexpr std::string_view kConsoleCss = R"CSS(
```

to:

```cpp
constexpr std::string_view kConsoleAdminCss = R"CSS(
```

- [ ] **Step 3: Replace the root variables and base selectors in the Admin override**

Inside `kConsoleAdminCss`, replace the current `:root`, universal selector, `body`, `.shell`, `.sidebar`, `.brand`, `.nav`, `.dot`, `.main`, `.topbar`, `.status`, `.actions`, `.filters`, `.pill`, `.btn`, `.content`, `.page`, `.title-row`, heading, `.subtitle`, `.grid`, `.card`, span, metric, toolbar, form, table, state, drawer, callout, confirm, JSON preview, and media rules with this override block:

```css
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
```

- [ ] **Step 4: Update `TqAdminConsoleCss()` to return Pico plus overrides**

Replace:

```cpp
std::string_view TqAdminConsoleCss() {
    return kConsoleCss;
}
```

with:

```cpp
std::string_view TqAdminConsoleCss() {
    static const std::string css =
        std::string(reinterpret_cast<const char*>(kTqAdminConsolePicoCss), kTqAdminConsolePicoCssLen) +
        "\n" +
        std::string(kConsoleAdminCss);
    return css;
}
```

- [ ] **Step 5: Build and run the focused test**

Run:

```bash
rtk cmake --build build --target tcpquic_admin_http_test -j2
rtk ./build/bin/Release/tcpquic_admin_http_test
```

Expected: the test still fails at return code `514` because HTML does not yet have `data-theme="light"`. It should no longer fail at `509`, `510`, `511`, `512`, or `513`.

- [ ] **Step 6: Commit CSS embedding**

Run:

```bash
rtk git add src/runtime/admin_console.cpp
rtk git commit -m "feat: embed pico css in admin console"
```

Expected: one commit modifying only `src/runtime/admin_console.cpp`.

## Task 4: Update Embedded HTML For Pico-Compatible Shell

**Files:**
- Modify: `src/runtime/admin_console.cpp`

- [ ] **Step 1: Set explicit light theme and color scheme metadata**

In the `kConsoleHtml` string, replace:

```html
<html lang="zh-CN">
```

with:

```html
<html lang="zh-CN" data-theme="light">
```

Then replace:

```html
  <meta name="viewport" content="width=device-width, initial-scale=1">
```

with:

```html
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta name="color-scheme" content="light">
```

- [ ] **Step 2: Rename the browser title away from interaction-design wording**

Replace:

```html
  <title>raypx2 Admin Console Interaction Design v4</title>
```

with:

```html
  <title>raypx2 Admin Console</title>
```

- [ ] **Step 3: Update the sidebar subtitle to production wording**

Replace:

```html
        <p>v4: 只展示当前 Admin API 可直接获得的信息。</p>
```

with:

```html
        <p>Embedded operations console for the local Admin API.</p>
```

- [ ] **Step 4: Build and run the focused test**

Run:

```bash
rtk cmake --build build --target tcpquic_admin_http_test -j2
rtk ./build/bin/Release/tcpquic_admin_http_test
```

Expected: `tcpquic_admin_http_test` exits with code `0`.

- [ ] **Step 5: Commit HTML shell updates**

Run:

```bash
rtk git add src/runtime/admin_console.cpp
rtk git commit -m "style: refine admin console html shell"
```

Expected: one commit modifying only `src/runtime/admin_console.cpp`.

## Task 5: Improve Table Empty States And Status Rendering

**Files:**
- Modify: `src/runtime/admin_console.cpp`
- Modify: `src/unittest/admin_http_test.cpp`

- [ ] **Step 1: Add failing assertions for empty state and status helpers**

In `src/unittest/admin_http_test.cpp`, inside the same initial `html/css/js` block, add these assertions after the Task 1 assertions:

```cpp
        if (js.find("function statusClass(value)") == std::string_view::npos) return 517;
        if (js.find("function emptyRow(colspan, message)") == std::string_view::npos) return 518;
        if (js.find("No rows returned") == std::string_view::npos) return 519;
```

- [ ] **Step 2: Run the focused test and verify failure**

Run:

```bash
rtk cmake --build build --target tcpquic_admin_http_test -j2
rtk ./build/bin/Release/tcpquic_admin_http_test
```

Expected: the executable exits non-zero with return code `517`.

- [ ] **Step 3: Add JS helpers**

In `kConsoleJsStorage`, after:

```js
    function stateClass(value) {
      const normalized = String(value || '').toLowerCase();
      if (['connected','healthy','ok','enabled','active','running'].some(word => normalized.includes(word))) return 'ok';
      if (['error','failed','denied','disabled'].some(word => normalized.includes(word))) return 'err';
      return 'warn';
    }
```

add:

```js
    function statusClass(value) {
      return stateClass(value);
    }

    function emptyRow(colspan, message) {
      return `<tr><td colspan="${colspan}" class="empty-cell">${escapeHtml(message || 'No rows returned')}</td></tr>`;
    }
```

- [ ] **Step 4: Update `renderRows` to render an empty state**

Find `function renderRows(tbody, rows, columns)`. Replace its initial body:

```js
      tbody.innerHTML = rows.map(row => `<tr>${columns.map(column => `<td>${formatCell(column, row[column])}</td>`).join('')}</tr>`).join('');
```

with:

```js
      if (!rows.length) {
        tbody.innerHTML = emptyRow(columns.length, 'No rows returned');
        return;
      }
      tbody.innerHTML = rows.map(row => `<tr>${columns.map(column => `<td>${formatCell(column, row[column])}</td>`).join('')}</tr>`).join('');
```

- [ ] **Step 5: Add CSS for the empty cell**

Inside `kConsoleAdminCss`, after:

```css
    tbody tr:hover{background:#fbfcfd}
```

add:

```css
    .empty-cell{color:var(--tq-muted);text-align:center;padding:18px 8px;background:#fbfcfd}
```

- [ ] **Step 6: Run the focused test**

Run:

```bash
rtk cmake --build build --target tcpquic_admin_http_test -j2
rtk ./build/bin/Release/tcpquic_admin_http_test
```

Expected: `tcpquic_admin_http_test` exits with code `0`.

- [ ] **Step 7: Commit UI rendering polish**

Run:

```bash
rtk git add src/runtime/admin_console.cpp src/unittest/admin_http_test.cpp
rtk git commit -m "style: improve admin console table states"
```

Expected: one commit containing the JS/CSS empty-state polish and matching test assertions.

## Task 6: Final Verification

**Files:**
- Inspect: `src/runtime/admin_console.cpp`
- Inspect: `src/runtime/admin_http.cpp`
- Inspect: `src/unittest/admin_http_test.cpp`
- Inspect: `third_party/pico/`

- [ ] **Step 1: Verify no external Pico CDN reference exists**

Run:

```bash
rtk rg -n "cdn.jsdelivr|picocss.com|@picocss/pico|https://cdn" src/runtime third_party/pico docs/superpowers/specs docs/superpowers/plans
```

Expected: matches may exist only in docs/spec or plan text. There must be no match in `src/runtime/admin_console.cpp`, `src/runtime/admin_http.cpp`, or generated runtime resources.

- [ ] **Step 2: Verify Pico is embedded in the served CSS**

Run:

```bash
rtk cmake --build build --target tcpquic_admin_http_test -j2
rtk ./build/bin/Release/tcpquic_admin_http_test
```

Expected: `tcpquic_admin_http_test` exits with code `0`.

- [ ] **Step 3: Build the main binary**

Run:

```bash
rtk cmake --build build --target tcpquic-proxy -j2
```

Expected: build succeeds. If the project’s main target name differs in the local build directory, run `rtk cmake --build build -j2` and confirm the full project builds.

- [ ] **Step 4: Inspect changed files**

Run:

```bash
rtk git diff --stat HEAD~5..HEAD
rtk git status --short
```

Expected: the recent commits include only files from this plan. Existing unrelated dirty files may still appear in `git status --short`; do not modify or revert them.

- [ ] **Step 5: Record verification result in final response**

Report these exact items:

```text
Verified:
- tcpquic_admin_http_test
- main build target or full build command used
- no runtime CDN references in src/runtime
```

If a command fails, report the failing command, exit behavior, and the first actionable error line.
