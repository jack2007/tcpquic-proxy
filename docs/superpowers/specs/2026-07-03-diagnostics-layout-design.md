# Diagnostics 页面布局设计

## 背景

Admin Console 的 Diagnostics 页面在 client/server 共用。当前页面的问题是诊断配置控件、保存按钮、allocator 操作和结果展示在视觉上没有形成稳定的栅格关系，字段和按钮容易显得未对齐。用户已通过 visual companion 确认采用 C 方案的 50/50 双列布局。

## 目标

1. 左侧展示 diagnostics 配置控制，占内容区一半宽度。
2. 右侧展示 allocator 相关内容，占内容区一半宽度。
3. 右侧 `Allocator dump` 位于上方，顶边与左侧 `诊断控制` 卡片顶边对齐。
4. 右侧 `Allocator 结果` 位于下方，底边与左侧 `诊断控制` 卡片底边对齐。
5. 移除 diagnostics 的“当前状态”JSON 展示，因为 UI 表单本身已经表达当前配置。
6. 保持 client/server 页面共用同一实现。

## 页面结构

Diagnostics 页面内容区保留标题区域，标题下方使用 2 列布局：

- 左列：`诊断控制` 卡片。
- 右列：allocator 列，内部上下排列 `Allocator dump` 和 `Allocator 结果` 两张卡片。

桌面布局使用 `grid-template-columns: minmax(0, 1fr) minmax(0, 1fr)`，左右各占 50%。右列使用纵向 grid，并拉伸到与左列同高，使 `Allocator dump` 与左列顶边对齐，`Allocator 结果` 与左列底边对齐。

窄屏布局降级为单列：先显示 `诊断控制`，再显示 `Allocator dump`，最后显示 `Allocator 结果`。

## 组件设计

### 诊断控制

诊断控制卡片包含：

- `trace` 开关。
- `trace_interval_sec` 数值输入。
- `trace_level` 下拉选择。
- `diag_stats` 开关。
- `diag_stats_interval_sec` 数值输入。
- `Save diagnostics` primary 按钮。

按钮放在卡片底部，宽度填满左列，保持 primary 蓝色。表单值就是当前 diagnostics 配置；保存成功后刷新控件值，不再额外展示同内容 JSON。

### Allocator dump

右上卡片包含：

- 简短危险操作提示。
- `Run allocator dump` danger 按钮。

该卡片只负责触发 allocator dump，不混入 diagnostics 保存动作。

### Allocator 结果

右下卡片展示最近一次 allocator dump 的摘要结果。结果区采用浅色面板和键值行，避免使用黑底 JSON 预览。首版可以继续由现有 allocator JSON 数据渲染出简要摘要；如果字段较多，保留可滚动浅色区域。

## 数据流

1. `renderDiagnostics()` 调用 `GET /diagnostics`，将返回值写入左侧表单控件。
2. `saveDiagnostics()` 从左侧表单读取值，调用 `PATCH /diagnostics`，成功后刷新表单控件。
3. `runAllocatorDump()` 调用 `POST /memory/allocator:dump`，将结果写入右下 `Allocator 结果` 区。

## 错误处理

- diagnostics 读取或保存失败时，在 `Allocator 结果` 之外的页面状态区域显示现有错误提示，不改变表单布局。
- allocator dump 失败时，只更新 allocator 结果区或现有错误展示，不影响左侧 diagnostics 表单。

## 测试

更新 `tcpquic_admin_http_test` 的静态 Console 断言：

- Diagnostics 页面存在 50/50 布局相关 class。
- 不再包含 `id="diagnostics-json"` 的当前状态 JSON 预览。
- `Save diagnostics` 位于 diagnostics 控制卡内。
- `Allocator dump` 和 allocator 结果区域仍存在。
- JS 仍调用 `GET /diagnostics`、`PATCH /diagnostics` 和 `POST /memory/allocator:dump`。

## 非目标

- 不改 Admin API 行为。
- 不新增 diagnostics 字段。
- 不改变 allocator dump 接口。
- 不处理其他页面布局。
