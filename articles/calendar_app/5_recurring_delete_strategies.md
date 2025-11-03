# 05｜循环实例的删除策略（单次/系列/未来全部）

- 目标：明确循环的删除语义与数据模型：删除单次、删除整个系列、删除从某次开始的未来所有。
- 范围：仅后端；强调一致性与可恢复性（审计/软删）。

## 删除选项
- 仅删除此实例：在 `exdate` 增加该发生时间，或以 Exception 记录“tombstone”。
- 删除整个系列：标记 BaseItem `status=cancelled` 或软删；保留审计记录。
- 删除未来全部：在 `RRULE` 设置 `UNTIL` 为某一发生时间前一刻，或剪断规则。

## 数据建模
- `exdate[]`：最轻量的单次剔除；适用于无单次修改的场景。
- Exception tombstone：当该实例曾被修改过，使用异常记录“删除”。
- 审计：`deleted_at` 与操作人，便于恢复与排查。

## API 设计
- DELETE `/events/{id}?occurrence=<ISO-datetime>`：仅此实例（更新 `exdate` 或写 tombstone）。
- DELETE `/events/{id}`：全系列删除（软删优先）。
- DELETE `/events/{id}?scope=future&from=<ISO-datetime>`：删除未来全部（调整 RRULE 或新增终止条件）。

## 一致性与再生成
- 删除后再进行窗口展开：确保被剔除的实例不返回；已修改的实例用 tombstone 优先。
- 系列删除时，清理或保留历史异常需明确定义（默认保留）。

## 规范速记（只提关键）
- RFC 5545：`EXDATE` 用于单次排除；`CANCELLED` 状态语义。

## 小结
- 单次删用 `exdate`/tombstone；系列删用软删；未来删用规则剪断。
- 下一篇：Apple Calendar 导入（CalDAV/ICS 解析与循环处理）。