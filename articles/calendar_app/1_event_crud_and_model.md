# 01｜事件数据模型与基础 CRUD（Go 后端）

- 目标：建立 `事件(Event)` 的核心数据模型与基础 CRUD API，明确时区、全天事件、时间范围查询与版本控制。
- 适配：以 Go + PostgreSQL 为例，接口为 REST；规范只提及关键 iCalendar 映射，不展开细节。

## 核心思路
- 统一的时间语义：`start`/`end` + `timezone`，全天事件使用日期边界；拒绝存“本地时间偏移”的混乱。
- 读写分离的考虑：强一致的写（事务/乐观锁），读侧支持按时间窗口分页。
- 事件与日历的关系：`calendar_id` 外键，便于权限与分组。

## 数据模型（示例）
- Event：`id`, `calendar_id`, `title`, `description`, `location`, `start_ts`, `end_ts`, `tz`, `is_all_day`, `status`, `created_at`, `updated_at`, `version`。
- Index：`calendar_id + (start_ts, end_ts)`，支持时间范围检索；`version` 用于并发更新。
- 状态：`confirmed | tentative | cancelled`（与 iCalendar `STATUS` 映射）。

## API 设计（REST）
- POST `/events`：创建事件（校验时间、全天兼容、冲突策略）。
- GET `/events/:id`：读取单个事件（返回 ETag/Version）。
- GET `/events?calendar_id&start<=t1&end>=t2`：按窗口查询（含全天）。
- PATCH `/events/:id`：更新事件（乐观锁：`If-Match: <version>`）。
- DELETE `/events/:id`：删除事件（软删或硬删按业务选择）。

## Go 代码骨架（示意）
- `type Event struct { ... }`：结构体字段对齐上文模型。
- Handler：`CreateEvent`, `GetEvent`, `ListEvents`, `UpdateEvent`, `DeleteEvent`。
- 校验：时间区间合法、全天事件转换、版本校验。

## 边界与实践
- 时区与 DST：所有入库统一为 UTC，保留 `tz` 用于展示；全天事件用用户所在时区计算边界再转换为 UTC。
- 并发：读返回 `version`；更新需携带 `If-Match`，不匹配返回 412。
- 查询：限制最大窗口与分页，避免大范围扫库。

## 规范速记（只提关键）
- iCalendar `VEVENT` 与字段映射：`DTSTART/DTEND`, `STATUS`。
- RFC 5545（iCalendar）— 仅需理解事件的基本时间语义。

## 小结
- 打好“事件”这块地基，后续任务/循环都在此之上迭代。
- 下一篇：任务模型与 CRUD（差异点与 VTODO 映射）。