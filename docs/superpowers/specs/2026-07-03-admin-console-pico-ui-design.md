# Admin Console Pico UI 改造设计

## 背景

当前 Admin Console 功能基本满足需求，但视觉层级、控件样式、表格密度和表单布局偏简陋。项目约束是继续保持单二进制发布模式：页面 HTML、CSS、JS 仍由 C++ 内嵌字符串提供，不依赖 CDN，不要求部署静态资源目录。

本次设计选择保留现有左侧导航、右侧内容区的整体结构，不引入拓扑式三栏重构。UI 基础样式参考 Pico CSS，并把固定版本的 Pico CSS vendored 到仓库后内嵌进二进制。

## 目标

- 保持当前 Admin API 页面功能和信息架构基本不变。
- 保留现有 `/console/`、`/console/style.css`、`/console/app.js` 静态入口。
- 使用本地 vendored Pico CSS 作为基础样式，不使用 CDN。
- 提升表格、表单、按钮、卡片、状态标识、顶部状态栏和危险操作区域的视觉质量。
- 保持运维工具的信息密度，避免营销页风格、过度装饰或大面积低信息内容。
- 继续支持 client/server 两种角色页面。

## 非目标

- 不引入 React、Vue、Svelte、Tailwind、Vite、Webpack 或 Node 运行时依赖。
- 不把 console 拆成外部静态资源目录。
- 不重写 Admin API 或新增后端接口。
- 不改变 token 存储策略、鉴权策略或现有 API 路由语义。
- 不把左侧导航改成三栏拓扑布局。

## 依赖与许可

使用 Pico CSS v2.1.1 作为固定版本。仓库新增：

- `third_party/pico/pico.min.css`
- `third_party/pico/LICENSE.md`
- 可选 `third_party/pico/VERSION`

Pico CSS 使用 MIT License。项目应保留原始 license 文件，并在后续升级时记录版本来源。运行时不访问 `picocss.com`、jsDelivr、npm registry 或其它外部网络地址。

## 架构

现有资源结构保持不变：

- `TqAdminConsoleHtml()` 返回 `/console/` 的 HTML。
- `TqAdminConsoleCss()` 返回 `/console/style.css` 的 CSS。
- `TqAdminConsoleJs()` 返回 `/console/app.js` 的 JS。
- `TqHandleConsoleStatic()` 继续在 `src/runtime/admin_http.cpp` 中分发上述资源。

CSS 采用两层结构：

1. Pico base：完整或近完整的 `pico.min.css`，作为基础 reset、排版、表单、按钮、表格和控件样式。
2. Admin override：项目自定义变量、布局、表格密度、状态 badge、侧边栏、顶部栏、操作区和响应式规则。

推荐实现方式是在源码层把两段 CSS 作为相邻字符串返回。若直接把完整 Pico minified CSS 放入 `admin_console.cpp` 导致 MSVC 字符串长度或可维护性问题，应沿用现有 JS 分段模式，把 Pico CSS 拆成多个 `R"CSS(...)"` 片段或生成独立的 C++ include 文件。

## 页面布局

保留当前结构：

- 左侧固定导航：显示品牌、角色相关页面入口、当前页面激活态。
- 右侧主区：顶部状态栏 + 页面内容。
- 页面内容继续使用卡片、表格、表单和 JSON 预览区。

调整方向：

- 左侧导航减少视觉噪音，使用清晰激活态和状态点。
- 顶部状态栏更像控制台工具条，包含 role、health、refresh、Refresh now、Pause refresh、Logout。
- Overview 页面保留指标卡，但统一尺寸、间距和数字层级。
- Peers/Connections/Tunnels 页面以高密度表格为主，优化表头、横向滚动、状态 badge 和 actions 列。
- Create/Edit Peer 表单保持在页面内，不改为全屏向导；字段分组更清晰。
- Config/Diagnostics 保留 JSON/textarea 工作流，提升等宽字体、边框、错误提示和危险操作确认。

## 视觉系统

使用 Pico 的 CSS 变量和语义控件作为基础，但项目 override 定义 Admin Console 自己的 token：

- 背景：浅灰工作区，白色内容面板。
- 文本：高对比主文本 + 中性 muted 文本。
- 状态色：healthy/ok 使用绿色，warning/reconnecting 使用琥珀色，error 使用红色，info 使用蓝色。
- 圆角：卡片和表单控件保持 6-8px，避免过度圆润。
- 字体：沿用系统 UI 字体；JSON/配置预览使用系统等宽字体。
- 密度：表格、toolbar、badge、按钮以运维场景为准，避免过大留白。

## HTML 调整

HTML 继续保持无构建步骤。允许做以下低风险调整：

- 给 `<html>` 增加 Pico 推荐的 `data-theme` 或 color-scheme 相关属性时，应默认浅色。
- 让按钮、输入、选择框、textarea 尽量使用 Pico 语义结构。
- 把重复的状态展示统一成 `.state` / `.badge` 类。
- 保留现有 DOM id，除非 JS 同步修改，避免破坏功能绑定。

## JavaScript 调整

JS 以最小改动为原则：

- API 调用、refresh、login、client/server 页面切换逻辑不重写。
- 只调整 class 名、状态 badge 渲染、表格空状态、错误提示文案容器等 UI 相关代码。
- 保持 `sessionStorage` token 策略不变。
- 保持 3 秒自动刷新和手动刷新按钮行为不变。

## 响应式行为

- 桌面端保持左侧导航 + 右侧内容区。
- 中窄屏下左侧导航可变为顶部横向/网格导航，沿用当前响应式思路。
- 表格使用横向滚动，不强行隐藏关键列。
- 表单字段在窄屏下单列显示。
- 顶部工具条允许换行，但按钮和状态 pill 不应互相遮挡。

## 错误处理与可访问性

- 登录失败、API 错误、JSON parse 错误保持可见，不只写入 console。
- 危险操作继续使用显式确认区域，不做无确认的一键 destructive action。
- 按钮、输入和导航应保留可见 focus 样式。
- 状态不能只靠颜色表达，应保留文本，如 `healthy`、`reconnecting`、`error`。
- 表格空状态显示明确文案，而不是空白区域。

## 测试与验证

实现后应至少验证：

- `admin_http_test` 中 `/console/`、`/console/style.css`、`/console/app.js` 内容类型和可访问性仍通过。
- C++ 编译通过，尤其关注 MSVC raw string literal 长度限制。
- console 页面在 client/server 两种角色下可登录、刷新、切页。
- Peers、Connections、Tunnels、Config、Diagnostics 页面不出现 JS 运行时错误。
- 桌面宽屏、窄屏布局无明显遮挡、溢出或按钮文字截断。
- 最终二进制运行时不访问外部 CSS/CDN。

## 方案取舍

已选择方案：完整 vendored Pico CSS + 项目 override。

原因：

- 与用户要求一致：参考 Pico，但不依赖 CDN。
- 维护简单：后续升级只需要替换 vendored 版本并检查 override。
- 风险可控：保留现有布局和 JS 功能，不引入前端构建链。
- 二进制体积增加有限，相比可维护性收益可以接受。

未选择方案：只提取 Pico 子集。该方案体积更小，但维护成本高，容易与 Pico 文档和默认控件行为不一致。

## 实施边界

本设计只覆盖 UI 视觉和布局精修。若后续需要新增拓扑视图、连接操作、历史图表或实时流量图，应作为独立功能设计，不混入本次 Pico UI 改造。
