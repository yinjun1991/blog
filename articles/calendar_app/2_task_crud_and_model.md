# 02｜任务数据模型与基础 CRUD（与事件的差异）

- 目标：定义 `任务(Task)` 的核心字段与 CRUD，阐明与事件的差异与 iCalendar `VTODO` 的最小映射。
- 范围：关注截止时间、完成状态、优先级与标签；提醒/子任务等高级特性不展开。

## 差异认知
- 任务是状态驱动：`status`（`needsAction | inProcess | completed | cancelled`）。
- 时间语义不同：常用 `due`（截止）而非 `start/end`；可能无明确持续时长。
- 列表交互：更多按状态与优先级过滤，而非时间窗口检索。

## 数据模型（示例）
- Task：`id`, `calendar_id`, `summary`, `description`, `due_ts`, `completed_at`, `status`, `priority`, `tags`, `created_at`, `updated_at`, `version`。
- Index：`calendar_id + status`；`due_ts` 支持按截止时间排序。
- 优先级：1（最高）-9（最低），与 iCalendar `PRIORITY` 映射。

## API 设计（REST）
- POST `/tasks`：创建任务（默认 `status=needsAction`，可选 `priority`）。
- GET `/tasks/:id`：读取任务（返回 `version`）。
- GET `/tasks?calendar_id&status&due_before&due_after`：过滤列表。
- PATCH `/tasks/:id`：更新状态/截止时间/优先级（乐观锁）。
- DELETE `/tasks/:id`：删除任务（软删优先）。

## Go 代码骨架（示意）
- `type Task struct { ... }`：字段与上文一致。
- Handler：`CreateTask`, `GetTask`, `ListTasks`, `UpdateTask`, `DeleteTask`。
- 校验：状态机合法性（`needsAction→inProcess→completed`）。

## 与事件的统一抽象
- 可定义 `Item` 抽象层：共享 `id/calendar_id/title/version/created_at`；事件/任务作特化。
- 权限复用：沿用事件的日历维度权限控制。

## 规范速记（只提关键）
- iCalendar `VTODO` 字段：`DUE`, `COMPLETED`, `STATUS`, `PRIORITY`。
- RFC 5545（iCalendar）— 任务相关字段的最小集合。

## 小结
- 任务 = 状态+截止时间的模型；查询以状态为轴心。
- 下一篇：循环规则建模与实例生成策略。