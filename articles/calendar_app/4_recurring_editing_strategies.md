# 04｜循环实例的修改策略（单次修改 vs 系列修改）

- 目标：说明如何对循环的一次实例或整个系列进行修改，并在数据层保持一致性。
- 范围：只讨论后端模型与 API；不涉及 UI 细节。

## 修改选项
- 仅修改此实例：对某次发生进行覆盖（时间/标题等），不影响其他实例。
- 修改整个系列：变更 BaseItem 或 `RRULE`，所有未被覆盖的实例随之变化。

## 数据建模
- Exception 表：记录 `occurrence_id/occurrence_start` + 覆盖字段（如 `start_ts`, `end_ts`, `title`）。
- `occurrence_id` 生成：由 BaseItem `id` + 发生日期派生（稳定且可索引）。
- 规则变更：更新 BaseItem/Recurrence；必要时迁移已存在的 Exception 的参照。

## API 设计
- PATCH `/events/{id}?occurrence=<ISO-datetime>`：单次修改（写入 Exception）。
- PATCH `/events/{id}`：系列修改（更新 BaseItem/Recurrence）。
- 并发控制：单次与系列均需 `If-Match`；系列修改可能冲突，需要合并策略。

## 冲突与一致性
- 当系列规则变更导致某次实例不再存在：保留该 Exception 为“孤例”或清理（按业务选）。
- 对已被单次覆盖的实例，系列修改不应覆盖异常字段（尊重用户选择）。

## 验证用例
- 先建每日循环，再修改第 3 天为不同时间；列表应显示覆盖后的时间。
- 修改系列开始时间，观察此前异常是否仍生效。

## 规范速记（只提关键）
- RFC 5545：`RECURRENCE-ID` 概念可借鉴用于定位实例。

## 小结
- 单次修改通过 Exception 覆盖；系列修改调整 BaseItem/Recurrence 并与异常合并。
- 下一篇：循环实例删除策略（单次/系列/未来全部）。