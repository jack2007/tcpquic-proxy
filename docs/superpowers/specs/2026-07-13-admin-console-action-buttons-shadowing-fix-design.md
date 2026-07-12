# Admin Console Action 按钮变量遮蔽修复设计

## 背景

Admin Console 的 client Peers 页面能够显示 peer 数据行，但 actions 列没有 Edit/Delete 按钮。实际浏览器复现表明，页面加载两个 peer 后生成了两行数据，但按钮数量为零，并抛出 `TypeError: tr is not a function`。

Connections 页面的 Detail 按钮使用相同代码模式，存在同一问题。

## 根因

前端使用全局 `tr(key)` 函数取得当前语言的翻译文本，同时在 action 单元格渲染循环中把 `<tr>` 行元素参数命名为 `tr`。局部参数遮蔽全局翻译函数，按钮模板调用 `tr('action.edit')`、`tr('action.delete')` 或 `tr('action.detail')` 时，实际把 DOM 元素当作函数调用，导致渲染中断。

## 修复范围

- 将 Peers action 渲染循环的行元素变量从 `tr` 改为语义明确的 `rowElement`。
- 将 Connections action 渲染循环做相同修改。
- 保持全局翻译函数、翻译键和 API 行为不变。
- 英文继续显示 `Edit`、`Delete`、`Detail`。
- 中文继续显示 `编辑`、`删除`、`详情`。

不进行全局翻译函数重命名，不调整表格布局、按钮样式或 CRUD API。

## 测试设计

1. 先增加回归断言，要求两个 action 渲染循环使用 `rowElement`，且不得使用会遮蔽翻译函数的 `forEach((tr, index)` 模式。
2. 断言英文和中文翻译表均包含 Edit、Delete、Detail 对应文案。
3. 运行相关 Admin HTTP 单元测试，确认修复前新增断言按预期失败，修复后通过。
4. 构建 Release 二进制并启动隔离的 client 测试进程。
5. 使用 Firefox 实际登录 Admin Console，分别切换英文和中文：
   - Peers 每行应出现对应语言的 Edit/Delete 按钮。
   - Connections 有数据时，每行应出现对应语言的 Detail 按钮。
   - 渲染过程不得再抛出 `TypeError: tr is not a function`。

## 风险与兼容性

本修复只改变 JavaScript 局部变量名，不改变 DOM 属性、事件绑定、翻译键或后端接口，兼容性风险较低。Connections 可能在测试环境没有数据；自动化浏览器验证应明确区分“没有 connection 行”和“有行但按钮缺失”，单元回归断言始终覆盖该渲染路径。
