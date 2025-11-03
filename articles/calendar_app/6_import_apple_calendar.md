# 06｜导入 Apple Calendar：CalDAV 拉取与 ICS 解析

- 目标：从 Apple Calendar 通过 CalDAV 获取 ICS 数据，解析 `VEVENT/VTODO`，正确处理循环与例外并入库。
- 范围：只提关键协议与字段，不做协议详解；关注可靠的数据落地。

## 导入流程总览
- 发现：账号 → 主体（principal）→ 日历列表（`PROPFIND`）。
- 拉取：`REPORT calendar-query`/`sync-token` 增量获取 ICS。
- 解析：按 `UID` 聚合，解析 `VEVENT/VTODO` 与 `RRULE/EXDATE/RECURRENCE-ID`。
- 映射：转换为本地 Event/Task + Recurrence/Exception 模型。

## CalDAV 要点（只提关键）
- 认证：常用 Basic/应用专用密码；HTTPS 必须。
- 资源：日历集合下的 `.ics` 资源项；支持增量同步（`sync-token`）。
- 方法：`PROPFIND`（属性）、`REPORT`（查询），避免全量拉取。

## ICS 解析要点
- 事件：`DTSTART/DTEND/STATUS/UID`；任务：`VTODO` 的 `DUE/STATUS/PRIORITY/UID`。
- 循环：`RRULE/RDATE/EXDATE`；单次例外：`RECURRENCE-ID` 指定实例。
- 时区：`VTIMEZONE` 提供定义；需映射到系统时区数据库。

## 入库策略
- UID 映射：以 `UID` + 源日历标识作为幂等键；避免重复导入。
- 循环与例外：`RRULE` 落至 Recurrence；`RECURRENCE-ID` 作为 Exception 覆盖或 tombstone。
- 版本与更新：使用 ETag/`Last-Modified` 决定是否更新本地记录。

## 错误与兼容
- 非法规则：记录并跳过，保留原始 ICS 片段用于排查。
- 时区缺失：回退到 UTC 并打标。
- 长文本与附件：忽略非关键字段，确保核心时间与状态正确。

## 规范速记（只提关键）
- RFC 4791（CalDAV），RFC 4918（WebDAV），RFC 5545（iCalendar）。

## 小结
- 按增量拉取 + UID 幂等 + 正确映射循环/例外，保证导入稳定可靠。
- 下一篇：CalDAV Server 与 ICS 导出（分享订阅链接）。