# 07｜CalDAV Server 与 ICS 导出（分享订阅链接）

- 目标：提供只读订阅链接（ICS Feed）与最小可用的 CalDAV 服务端，实现跨产品分享与同步。
- 范围：只覆盖常用方法与缓存/权限设计；不做协议详解。

## ICS 导出（订阅链接）
- 路由：`GET /calendars/:id/ics?token=...`（只读 token）。
- 内容：构建 `VCALENDAR`，包含 `VEVENT/VTODO` 与最小字段。
- 循环：优先导出 BaseItem + 规则（`RRULE/RDATE/EXDATE`），必要时展开近期窗口。
- 性能：缓存生成结果，配合 `ETag`/`If-None-Match`；长列表分页或分片。

## CalDAV Server（最小子集）
- 资源结构：日历集合与条目资源（事件/任务）。
- 必要方法：
  - `OPTIONS/PROPFIND`：发现与属性。
  - `REPORT calendar-query`：按时间窗口检索。
  - `GET`：读取条目（ICS 格式）。
  - 可选 `PUT/DELETE`：写入/删除（若支持双向）。
- 增量：`sync-token` 支持客户端增量同步。

## 权限与安全
- Token 化订阅链接：随机高熵 token 与可撤销机制。
- 速率限制与防抓取：按 IP/token 限流；日志审计。
- 多日历：不同 `calendar_id` 提供独立链接；共享范围可控。

## 代码结构建议（Go）
- Handler：`GetICSFeed`, `CalDAVOptions`, `PropFind`, `CalendarQuery`, `GetItem`, `PutItem`, `DeleteItem`（按需）。
- 序列化：统一 ICS 构建器，复用模型与循环规则。
- 缓存：内存 + 持久化二级缓存，过期与失效策略明确。

## 规范速记（只提关键）
- RFC 4791（CalDAV），RFC 4918（WebDAV），RFC 5545（iCalendar）。

## 小结
- 提供只读订阅是最低成本的分享形式；CalDAV 服务端在此基础上逐步增强。
- 系列完结：从模型到导入导出，已具备上线所需最小能力。