# 后端经典模型学习地图：从“能跑”到“可恢复”

## 1. 这份地图解决什么问题

这不是框架大全，也不是“后端八股”清单。目标是建立一套识别问题的语言：看到需求时，先判断它属于哪种执行模型、会发生哪些失败，再选择成熟实现。

本文聚焦前端转 Java / Go 时最容易缺少的部分：异步任务、定时任务、批处理、长流程、消息、多 Worker、线程池、CPU / 内存预算、并发控制、缓存、查询模型、数据分片、分布式一致性与故障恢复。HTTP 语义、安全、普通表设计和索引基础不在本文展开。

先记住三个边界：

- **Scheduler（调度器）**决定“何时触发”。
- **Executor / Worker（执行器）**决定“由谁执行、并发多少、失败怎么办”。
- **Workflow（工作流）**决定“多个步骤如何保存状态、等待外部事件、恢复和补偿”。

一个可靠系统通常会组合它们，例如：Quartz 到点创建一条 Job，Worker 执行；Temporal 保存订单流程状态，由 Activity Worker 调用支付接口。

## 2. 先建立故障世界观

前端代码通常围绕一次页面生命周期思考；后端必须假设每条指令之间都可能出故障。

### 2.1 九条基本事实

1. 进程会在任意时刻退出，包括“外部扣款成功，但本地还没记成功”之间。
2. 请求超时只表示调用方没收到结果，不表示服务端没执行。
3. 消息、任务和请求都可能重复；可靠系统把重复当正常情况。
4. 顺序总有作用域。Kafka 只保证一个 partition 内有序，不保证整个 topic 全局有序。
5. `exactly once` 通常只在特定组件和事务边界内成立；跨数据库、MQ、第三方 API 后，仍要靠幂等与对账。
6. 重试会放大流量。下游越慢，上游无节制重试越可能把它压垮。
7. 锁有租期，持锁者可能暂停或失联；“我还在运行”不等于“我仍持有锁”。
8. 发布期间新旧代码会同时运行，队列里还可能有旧格式消息，长流程里还可能有旧版本状态。
9. 自动恢复不可能覆盖所有业务错误；系统必须提供查询、告警、人工修复和安全重放能力。

### 2.2 每个异步功能都要回答的问题

- 唯一业务标识是什么：`order_id`、`payment_id` 还是随机 `job_id`？
- 允许丢失、重复、乱序吗？允许到什么程度？
- 谁持久化待办事项？应用内存、数据库、消息代理还是工作流引擎？
- Worker 崩溃后，谁以及何时重新认领任务？
- 哪些错误可重试，哪些必须立即失败？总重试时长是多少？
- 副作用如何幂等？下游不支持幂等时如何查证结果？
- 多实例会不会同时执行？如果会，靠唯一约束、条件更新、行锁还是租约协调？
- 如何取消、暂停、重放、补偿和人工接管？
- 能否按业务 ID 查到当前状态、历次尝试和最后错误？
- 部署新版本时，旧消息与未完成流程能否继续执行？

## 3. 先用这张图识别模型

```text
请求必须立即拿结果？
├─ 是：同步请求；必须有 deadline、取消传播、隔离和过载保护
└─ 否
   ├─ 一个独立动作，稍后执行？→ Background Job / Task Queue
   ├─ 到某时刻或周期触发？→ Scheduler 产生 Job
   ├─ 有限的大数据集，分块处理？→ Batch + Checkpoint
   ├─ 持续到来的无界事件？→ Stream / Consumer Group
   └─ 多步骤，持续数小时到数月，需等待/重试/补偿？→ Durable Workflow

任务量或成本波动很大？→ 多队列 Worker Pool + 有界 in-flight + 资源准入
同一业务 key 必须有序？→ Partition / Key Affinity；不要要求全局串行
事件可能乱序或迟到？→ Event Time + Watermark + Window + Late-event Side Output
消费消息后要写数据库？→ Manual Ack + Inbox 本地事务
任务可能占用大量内存？→ Metadata 预检 + Weighted Semaphore + Streaming / Chunking

跨服务同时改 DB 并发事件？→ Transactional Outbox
重复执行会产生额外副作用？→ Idempotency / Deduplication
多个服务各有本地事务？→ Saga；不要假装它是一个数据库事务

读流量远大于写流量？→ 先索引，再考虑 Cache-Aside / Read Model
一个页面需要多个慢下游？→ Fan-out / Fan-in + 共享 deadline
深分页或数据持续变化？→ Keyset / Cursor Pagination
单库写入或容量到达上限？→ 最后才考虑 Partition / Sharding
必须以全部历史事实为真相源？→ 谨慎评估 Event Sourcing
```

## 4. 八种核心执行模型

### 4.1 同步请求：有截止时间的调用链

**识别信号**：用户正在等待结果；成功与失败要在本次请求内返回。

同步调用不是“直接调一下 API”，而是一条共享延迟预算的调用链：

```text
Browser → API → Service A → Service B → DB
          2s 总预算   余量递减       不能各自再等 2s
```

必须掌握：

- deadline 与 timeout：deadline 是绝对截止时间，timeout 是某一步最多等待多久。
- 取消传播：Java 使用线程中断/响应式取消或框架上下文；Go 用 `context.Context`。
- 有界并发：线程池、连接池和 goroutine 都不能无限增长。
- Bulkhead（舱壁）：给不同下游或业务分开并发配额，避免一个故障拖死全部请求。
- Load shedding（过载拒绝）：系统饱和时尽早返回 `429` / `503`，胜过排队到全部超时。

**不要使用**：为了“提高成功率”在调用链每层各重试三次。三层叠加可能把一次用户请求放大为 27 次下游调用。

**实现载体**：Java 可用 Resilience4j 的 TimeLimiter、Bulkhead、CircuitBreaker、Retry；Go 先掌握 `context`、有界 channel / semaphore、`golang.org/x/time/rate`，再按需要引入库。

### 4.2 Background Job：持久化待办事项

**识别信号**：接口只需接受任务，实际工作可稍后完成；任务要跨进程重启存活。

典型状态机：

```text
scheduled → available → running → succeeded
                         ├─ retry_wait → available
                         └─ discarded / dead_letter
```

核心不是启动一个线程，而是持久化以下信息：

- `job_id`、`job_type`、参数与参数版本
- `state`、`attempt`、`max_attempts`
- `scheduled_at`、`started_at`、`finished_at`
- `lease_owner`、`lease_expires_at`
- `last_error_code`、`last_error_message`
- 业务关联 ID 和 trace ID

Worker 一般通过“原子认领 + 租约”取得任务。Worker 崩溃、租约到期后任务会再次可见，所以默认按 **at-least-once** 设计。成功副作用与任务确认之间无法普遍做成一个原子操作，业务幂等仍不可少。

**适合**：发邮件、生成缩略图、导出报表、Webhook 投递、异步调用第三方 API。

**不适合**：步骤很多且需要等待人工审批的长流程；把几十个 Job 用回调手工串起来，会逐渐造出一个缺少可视化、恢复和版本机制的工作流引擎。

**成熟实现**：

- Java：JobRunr（数据库持久化、延迟/周期任务、重试、Dashboard）；更偏调度可选 Quartz。
- Go + PostgreSQL：River，可在业务事务中 `InsertTx`，减少双写问题。
- Go + Redis：Asynq，提供租约恢复、重试、周期任务与管理 UI；注意它仍是 `v0.x`，并且部分 Lua 脚本对 Redis Cluster 有限制。
- 云环境：SQS 等托管队列；要理解 visibility timeout、ack/delete、DLQ，而不是只会 SDK 调用。

### 4.3 Scheduler：时间只负责触发，不负责业务正确性

**识别信号**：“每天 02:00”“付款后 30 分钟”“2026-10-01 执行”。

需要区分三类时间语义：

- 固定频率：每 5 分钟一次，关心相邻触发时间。
- 日历时间：每个工作日 09:00，受时区、夏令时、节假日影响。
- 业务延迟：订单创建 30 分钟后过期，通常每个业务对象有独立的 `due_at`。

每个定时任务必须明确：

- **misfire**：停机期间错过的触发，是补跑一次、全部补跑还是跳过？
- **overlap**：上次没结束，下次是并行、跳过还是排队？
- **concurrency**：多副本应用是否都会触发？
- **timezone / DST**：时间按 UTC、用户时区还是业务时区解释？不存在或重复的本地时间怎么办？
- **trigger identity**：同一计划的同一次触发如何去重？可用 `(schedule_id, scheduled_at)` 唯一约束。

**选择**：

- 单进程、任务可丢：Spring `@Scheduled`、Go `go-co-op/gocron/v2`。
- Java、需要持久化 trigger、集群与 misfire 策略：Quartz。
- 已运行 Kubernetes、任务适合独立容器：Kubernetes CronJob；官方也明确要求 Job 幂等，因为控制器不能完全避免重复或漏掉边界情况。
- 大量“每个订单一个未来时间”：写入支持 delayed job 的任务队列，或由扫描器按 `due_at` 批量入队；不要给每个订单创建操作系统 cron。

### 4.4 Batch：有限数据集的分块、检查点与重启

**识别信号**：每天处理一批文件/账单/历史数据；数据量大到不能放进单个事务或内存。

经典结构是 `Reader → Processor → Writer`，每 N 条提交一个 chunk，并保存 checkpoint。重启时从最后成功边界继续，而不是从头盲跑。

要设计：

- Job Instance：例如 `settlement + business_date=2026-09-01`，参数共同决定一次业务运行。
- Step 与依赖：下载、校验、处理、汇总、发布结果。
- checkpoint：文件偏移、游标、最后主键或分片号。
- skip / retry：坏数据隔离到 reject 表，瞬时故障才重试。
- partition：按主键区间、日期或租户拆分；分片必须稳定且可重跑。
- restart：已完成 Step 是否跳过，失败 Step 从哪里继续。

**成熟实现**：Java 优先 Spring Batch。它明确不是 Scheduler，而是批处理与重启框架；用 Quartz、Kubernetes CronJob 或外部平台触发它。Go 没有同等事实标准；简单场景可用数据库 checkpoint + 有界 Worker Pool，复杂场景不要为了语言纯洁而重造 Spring Batch 或工作流引擎。

### 4.5 Durable Workflow：把长流程状态变成可恢复历史

**识别信号**：流程持续很久，包含多个远程调用、定时等待、人工信号、重试、取消或补偿。

Temporal 这类引擎把 Workflow 的事件历史持久化，Worker 崩溃后通过 replay 恢复状态。关键抽象：

- Workflow：确定性的流程控制代码；不直接做网络、文件或随机调用。
- Activity：可能失败的外部副作用，如扣款、发货、调用 LLM。
- Timer：可跨重启的等待，不占一个睡眠线程。
- Signal / Update：运行中的流程接收付款成功、人工批准、取消等外部输入。
- Query：读取流程当前状态。
- Retry / Timeout：通常配置在 Activity，而不是无脑重跑整个 Workflow。
- Compensation：前向恢复不成立时执行语义上的撤销，例如释放库存；它不是数据库 rollback。

**适合**：订单履约、保险理赔、开户审核、跨系统数据搬迁、持续数小时的 AI pipeline。

**不适合**：单个 100ms 的数据库更新；引擎的部署、存储、运维和确定性约束没有收益。

**最容易犯的错**：在 Workflow 代码里直接调用 HTTP、读取当前时间、生成随机数或依赖不确定遍历顺序，导致 replay 与历史不一致；这些操作应通过 SDK 的确定性 API 或 Activity 完成。

### 4.6 业务状态机与 Saga：显式表达允许的变化

**识别信号**：对象有清晰生命周期，且“当前状态能否执行某动作”是业务规则。

订单不应由各处随意 `setStatus("paid")`。应定义命令和合法迁移：

```text
CREATED --pay--> PAID --ship--> SHIPPED
   │               └--refund--> REFUNDED
   └--expire--> EXPIRED
```

数据库更新使用条件更新或版本号防止并发覆盖：

```sql
UPDATE orders
SET status = 'PAID', version = version + 1
WHERE id = :id AND status = 'CREATED' AND version = :version;
```

受影响行数为 0 表示状态已变化或版本冲突，不能当成功。

当一个业务动作跨多个服务、每个服务各有数据库时，使用 Saga 思考：一组本地事务通过事件或 orchestrator 前进，失败时继续重试或执行补偿。

- Choreography：服务监听事件自行推进。参与者少时简单；参与者增加后依赖难追踪。
- Orchestration：协调器显式命令每一步，适合复杂流程；协调器自身必须持久化和高可用，Temporal 可承担这个角色。

补偿必须单独设计且幂等。`refund()` 不是 `charge()` 的数学逆操作：退款可能失败、手续费可能不可逆、发出的邮件也收不回来。

### 4.7 Event Log / Stream：保存事实并由消费者独立推进

**识别信号**：事件持续产生；多个消费者以不同速度独立处理；需要重放、分区扩展或保留历史。

需要理解：

- Topic 是事件类别，partition 是有序与并行的基本单位。
- key 决定分区；需要同一订单有序时，用 `order_id` 作 key。
- Consumer Group 内一个 partition 同时交给一个成员；增加消费者超过 partition 数不会继续提升并行度。
- offset 表示消费进度，不表示外部副作用一定成功。
- schema 必须可演进；新增可选字段通常比重命名/删除安全。

队列与流不是同义词：任务队列强调“一项工作由一个 Worker 完成”；事件流强调“同一个事实可由多个消费者组各自处理和重放”。

**成熟实现**：Kafka / RocketMQ 适合持久事件流与大吞吐；RabbitMQ 更擅长灵活路由与工作队列。不要先按流行度选中间件，先确定保留、重放、顺序、路由和运维要求。

### 4.8 Fan-out / Fan-in 与 Pipes / Filters：分解并行工作

**识别信号**：一个请求或任务需要查询多个独立来源，或一份数据要经过多个可组合处理阶段。

Fan-out / Fan-in 把一项工作并行拆给多个执行单元，再聚合结果：

```text
                    ┌→ inventory ─┐
request → fan-out ──├→ price ──────├→ fan-in → response
                    └→ promotion ─┘
```

关键不是“开很多 goroutine / CompletableFuture”，而是：

- 所有子调用共享父 deadline，不能每个再获得一份完整超时。
- 并发数有上限，任务数大时分批或用 Worker Pool。
- 明确聚合语义：all-of、any-of、quorum，还是允许部分结果。
- 一个子任务失败后，取消无意义的剩余任务；无法取消的结果要安全丢弃。
- 慢节点决定尾延迟，可使用 per-dependency timeout，但不能超出总预算。

Pipes / Filters 则把串行处理拆成职责单一的阶段，例如 `decode → validate → enrich → persist`。每个 filter 只依赖稳定输入/输出 schema，可以单独扩容、替换或并行。跨进程 pipeline 要再次面对消息重复、顺序和中间状态持久化；不要把普通函数组合过早拆成五个微服务。

## 5. 八个横切可靠性模型

### 5.1 幂等与去重

幂等不是“检查存在再插入”，因为两个并发请求可能同时检查为不存在。正确落点通常是数据库唯一约束或原子条件更新。

API 幂等表可以建模为：

```text
idempotency_records
- scope                 # 接口/租户/操作类型
- idempotency_key
- request_hash          # 防止同一 key 被不同参数复用
- state                 # processing / succeeded / failed
- response_status
- response_body
- expires_at
UNIQUE(scope, idempotency_key)
```

消息消费者可使用 Inbox：在同一数据库事务中插入 `(consumer, message_id)` 唯一记录并更新业务表；唯一冲突表示已处理。注意“去重记录先提交、业务更新后失败”会丢消息，所以两者必须同事务。

### 5.2 重试、退避、抖动与错误分类

一个完整 Retry Policy 包含：

- 哪些错误可重试：超时、连接重置、`429`、部分 `5xx`；参数错误、权限错误和业务拒绝通常不可重试。
- 最大尝试次数或总耗时预算。
- exponential backoff：间隔逐步增大。
- jitter：随机化间隔，防止大批客户端同时重试。
- `Retry-After`：下游明确给出时优先尊重。
- 最终去向：失败状态、DLQ、告警与人工重放。

重试前必须回答“上一次是否可能已经成功”。答案为“可能”时，只能使用同一个幂等键重试，或先向下游查询结果。

### 5.3 Timeout、Circuit Breaker、Bulkhead、Rate Limit、Backpressure

它们解决不同问题：

| 机制 | 解决的问题 | 典型实现 |
| --- | --- | --- |
| Timeout | 单次调用不能无限等待 | deadline / context |
| Circuit Breaker | 已知下游持续失败时停止继续打它 | closed / open / half-open 状态机 |
| Bulkhead | 限制一个依赖占用的并发资源 | 独立线程池、semaphore |
| Rate Limit | 限制单位时间准入量 | token bucket / sliding window |
| Backpressure | 消费方跟不上时让生产方减速或拒绝 | 有界队列、阻塞、`429`、暂停拉取 |

Circuit Breaker 不限制并发，Retry 也不等于容错；它们经常要和 timeout、bulkhead 一起使用。

### 5.4 Transactional Outbox：消除数据库与 MQ 双写窗口

错误写法：

```text
提交订单 DB → 发布 OrderCreated
```

进程在两步之间崩溃就会永久漏事件。Outbox 做法是在一个本地数据库事务里同时写订单和 outbox 行，再由 relay 发布：

```text
DB transaction: orders + outbox_events
                         ↓
               Polling Publisher 或 CDC
                         ↓
                        MQ
```

Outbox 解决“不漏掉已提交业务对应的事件”，不自动解决重复。relay 在“MQ 已接受、outbox 尚未标记”时崩溃仍会重复发布，消费者必须幂等。

### 5.5 并发控制：乐观锁、悲观锁、租约与 Fencing Token

- 唯一约束适合保护“不允许出现两个”的不变量，例如同一订单只能有一笔有效付款。
- 乐观锁使用 `version` 或旧状态做条件更新，适合冲突少、事务短的场景；更新行数为 0 就是冲突。
- 悲观锁使用 `SELECT ... FOR UPDATE` 等行锁，适合必须先读后写且冲突较多的短事务；持锁期间不要调用远程 API。
- Mutex 依赖同一进程内共享内存。
- 数据库行锁只持续一个数据库事务。
- Lease 有到期时间，需要续约；旧持有者暂停过久后可能与新持有者同时运行。
- Leader Election 选出一个节点做周期扫描/维护，但业务执行仍应幂等。
- Fencing Token 是单调递增代数。资源只接受比已见 token 更新的写入，才能拒绝“过期持锁者”。

选择顺序：先问能否用唯一约束、条件更新或单数据库事务解决；再考虑数据库 advisory lock / lease；只有确实跨资源互斥时才引入分布式锁。Redis 官方文档也提醒锁依赖 TTL 和时钟，强正确性场景需要 fencing token，不能把“拿到锁”当永久所有权。

### 5.6 Reconciliation：用事实源修复派生状态

再可靠的事件链也需要对账。Reconciliation Job 周期比较事实源与本地状态，例如：

- 支付机构已扣款，本地订单仍未支付。
- 对象存储已有文件，数据库导出任务仍是 running。
- outbox 已发布很久，但关键消费者没有处理记录。

对账不是失败后的临时脚本，而是一等后台任务：有游标、速率限制、差异表、修复动作和审计记录。

### 5.7 Poison Message、DLQ 与 Redrive

同一条坏消息如果无限重试，会阻塞分区或持续消耗资源。达到策略上限后进入 DLQ / discarded 状态。DLQ 至少保留：原始载荷、消息头、业务 ID、失败次数、最后错误、首次/末次失败时间和消费者版本。

Redrive（重放）前先修代码或数据，再决定原 ID 重放还是生成新的执行 ID；业务幂等键通常保持不变。DLQ 只是隔离区，不是垃圾桶，必须有告警和处理责任人。

### 5.8 可观测性与可操作性

异步系统至少暴露：

- Metrics：队列深度、最老任务等待时长、吞吐、成功率、重试率、DLQ 数、运行时长分位数、租约过期数。
- Logs：使用蛇形 message，并记录 `job_id`、业务 ID、`attempt`、`worker_id`、`trace_id`；不要把完整敏感 payload 打入日志。
- Traces：生产者 span 与消费者 span 通过传播上下文或 span link 关联。
- Admin：按业务 ID 查询、暂停队列、取消、重试、DLQ redrive、查看状态历史。

只看“服务进程存活”没有意义。队列消费者全部卡死时，HTTP 健康检查仍可能是绿色。

## 6. 多 Worker、时序与资源控制

这组问题的共同本质是：任务到来的速度、单次成本和完成时间都不稳定，而 CPU、内存、数据库连接和下游配额都有硬上限。可靠性不只是“不丢任务”，还包括系统过载时不会失控。

### 6.1 多 Worker 调度：先定义公平、顺序和认领协议

Competing Consumers（竞争消费者）让多个 Worker 从同一队列取任务，空闲 Worker 自动多做，适合任务成本接近且没有全局顺序要求的场景。真正的实现还要回答四个问题：

- **认领与恢复**：MQ 使用 ack / visibility timeout，数据库队列使用原子条件更新或 `FOR UPDATE SKIP LOCKED`，长任务使用 lease + heartbeat。Worker 失联后任务重新可见，因此 Handler 必须幂等。
- **顺序与亲和性**：需要同一订单顺序执行时，以 `order_id` 分区，或将同一 key 路由到 Keyed Serial Executor；不要为了局部顺序把全部任务降成单 Worker。
- **优先级与公平性**：单一 Priority Queue 可能让低优先级任务永久饥饿。多租户系统更适合每租户配额或 Weighted Fair Queue；紧急任务可以保留独立队列和并发预算。
- **停机与慢任务**：停止拉取新任务，等待 in-flight 到截止时间，再取消或让 lease 到期重投。对尾部极慢任务设置执行上限并记录原因，不能靠无限增加 Worker 掩盖。

Work Stealing 适合单进程内多个本地 Worker 平衡各自队列，不替代跨进程任务的持久化和租约。Distributed Semaphore 可限制整个集群对某个稀缺下游的并发，但必须考虑持有者崩溃后的租约释放；优先使用中间件提供的 queue concurrency 或数据库原子配额。

`worker_count`、prefetch、线程数和数据库连接池不能分别拍脑袋设置：它们共同决定最大 in-flight。若 40 个 Worker 每个预取 20 个任务，应用可能同时持有 800 份 payload，即使实际只有 40 个任务在执行。

### 6.2 时序模型：处理时间、事件时间与等待语义

“每隔一段时间”至少有以下不同语义：

| 模型 | 语义 | 典型用途 |
| --- | --- | --- |
| Fixed Rate | 按计划时间点触发，慢执行可能重叠或产生 misfire | 固定采样、周期报表 |
| Fixed Delay | 上一次结束后再等待固定时长 | 不允许重叠的轮询 |
| Delay Queue | 每项任务有独立 `due_at` | 订单过期、延迟通知 |
| Timing Wheel | 用分桶近似管理海量 timer | 百万级连接超时、延迟任务底层实现 |
| Debounce | 一段安静期后只执行最后一次 | 搜索联想、配置变更合并 |
| Throttle | 每个时间段最多执行一次或 N 次 | 高频进度上报、外部 API 限流 |
| Tumbling / Sliding Window | 按事件时间聚合不重叠/重叠窗口 | 指标统计、风控 |

流处理还要区分 processing time（系统收到事件的时间）与 event time（业务实际发生时间）。网络重试会让旧事件晚到，所以按 event time 聚合必须定义 watermark（系统认为事件时间已经推进到哪里）、allowed lateness 和 late-event 去向。要求严格顺序时，先明确顺序作用域，再用 partition key、sequence number 和有限 Reorder Buffer；不能等待一个可能永远不来的序号。

Deadline / TTL 也属于时序语义：deadline 表示“超过此时结果已无价值”，TTL 表示“数据或任务在此后应被淘汰”。它们都不等于后台操作会自动停止，代码仍要传播取消信号并检查状态。

### 6.3 消息队列：把确认点放在持久副作用之后

Producer 端：

1. 业务数据库与消息不能原子提交时，使用 Transactional Outbox，而不是提交 DB 后直接 publish。
2. 每条消息带稳定的 `message_id`、业务 key、`schema_version`、`occurred_at` 和 trace context。
3. 使用 publisher confirm / broker acknowledgement 确认代理已接收；响应丢失时仍可能重复发布。
4. payload 大小设硬上限。大文件放对象存储，消息只携带不可变对象 key、版本和校验值。

Consumer 端：

1. 关闭自动确认；先在本地事务中完成 Inbox 去重和业务更新，事务提交后再 ack。进程在 commit 后、ack 前退出会重复投递，Inbox 必须接住它。
2. 设置有限 prefetch / in-flight window。RabbitMQ prefetch 是未确认消息窗口；Kafka 用 `max.poll.records` 限制每批记录，并保证处理或心跳不会超过 `max.poll.interval.ms`，长处理可 pause partition 或把工作转交给持久任务队列。
3. 瞬时错误进入有上限、带退避的 retry tier；永久错误直接进入 DLQ。重放工具必须保留原业务幂等键并记录操作审计。
4. 关闭时先停止拉取，等待正在处理的事务完成并 ack，再关闭连接；不能收到终止信号后立刻杀掉整个进程。

不要只监控 queue depth。至少同时看 publish / consume rate、最老消息年龄、in-flight、ack latency、retry / DLQ rate、Kafka consumer lag，以及按 partition / tenant 的倾斜程度。

### 6.4 Thread Pool / Worker Pool：队列必须有界

一个执行池必须显式配置：并发数、队列容量、单任务截止时间、拒绝策略、取消传播、关闭流程，以及不同任务是否共享池。

- CPU-bound：并发通常从容器实际可用 CPU 数附近开始压测；增加线程只会增加调度和缓存失效。
- I/O-bound：可以高于 CPU 数，但上限应由数据库连接池、下游并发配额、内存和延迟目标决定，不能由“goroutine 很轻”决定。
- 不同故障域使用独立池或 semaphore，例如图片处理不能占光支付回调的执行资源。
- 队列满时只能阻塞上游、限时等待、降级或明确拒绝；无界排队只是把过载变成更晚发生的超时与 OOM。

Java `ThreadPoolExecutor` 使用 bounded `BlockingQueue`，并为 rejection 配置明确业务语义。Virtual Thread 适合大量阻塞 I/O，可提升吞吐但不会让单次调用更快，也不应用固定池复用；需要限制下游并发时在 virtual thread 内使用 `Semaphore`。长时间纯 CPU 任务仍使用有界平台线程池。

Go 的 goroutine 不是容量控制。使用 bounded channel / `x/sync/semaphore` 限制 in-flight，所有阻塞调用接收 `context.Context`，producer 在 `select` 中响应取消。不要先启动十万个 goroutine 再让它们排队抢 semaphore；应在创建 goroutine 前完成准入。

### 6.5 CPU 与内存预算：先准入，再执行

估算 Worker 内存不能只看 heap 总量，先写出近似预算：

```text
resident_memory ≈ active_tasks × per_task_working_set
                + prefetched_payloads
                + bounded_queue
                + cache
                + runtime / native_library overhead
```

任务成本差异大时，按“预计解压后字节数、图片像素数、批次行数”等单位使用 Weighted Semaphore；只限制任务个数会让一个 2 GB 文件和一个 20 KB 文件消耗同一许可。无法提前准确估算时先读取 metadata，超过硬上限直接拒绝或改走大任务专用队列。

控制手段按优先级组合：

1. Admission Control：资源预算不足时不接新任务或让 MQ 保持未投递状态。
2. Streaming / Chunking：边读边处理，批次提交，避免 `readAllBytes` / `io.ReadAll`。
3. Bounded Buffer：pipeline 每级只缓存有限项，让慢下游自然反压上游。
4. Spill to Disk / Object Storage：必须暂存的大中间结果落盘，并设置空间配额和清理协议。
5. Per-tenant Quota：限制单租户的并发、排队任务和累计字节数。
6. Load Shedding：达到 deadline 后已经无价值的任务不再执行；交互请求过载时返回 `429` / `503`。

Go 1.25 在 Linux 容器内默认让 `GOMAXPROCS` 感知 cgroup CPU limit，但 CPU request 不是 limit；显式覆盖后要自行保证正确。`GOMEMLIMIT` 是 GC 的软限制，不是防 OOM 的硬墙，应为非 Go 内存和瞬时峰值保留余量。Kubernetes CPU limit 通常表现为 throttling，memory limit 超出后可能由内核 OOM kill；应用内预算和基础设施 limit 两层都需要。

## 7. 五种数据访问与扩展模型

### 6.1 Cache-Aside + Singleflight：缓存读取与防击穿

**识别信号**：少量热点数据被反复读取，数据库延迟或连接数成为瓶颈，并且业务允许一个明确的短暂陈旧窗口。

Cache-Aside 的读路径：

```text
GET cache
├─ hit  → return
└─ miss → SELECT primary DB → SET cache with TTL → return
```

写路径通常先提交数据库，再删除缓存。选择删除而不是“顺手更新缓存”，可以减少多并发写导致旧值覆盖新值的窗口，但不能消除所有竞态：较早开始的 cache miss 可能在删除之后把旧数据库结果回填。删除失败、旧值晚回填都要求 TTL 兜底；更严格时使用带数据版本的条件回填、延迟再次失效或 CDC / Outbox 失效。无法接受陈旧数据的读路径不要经过缓存。

必须掌握：

- TTL 表达可接受的最大陈旧时间，不是随手写的常数；给 TTL 加随机扰动，避免大量 key 同时过期。
- Negative Cache 短暂缓存“不存在”，防止恶意或热点不存在 ID 穿透数据库；创建数据时要主动失效。
- Singleflight / Request Coalescing 让同一进程内同一个 key 的并发 miss 只回源一次；多实例需要 Redis 短租约、逻辑过期或概率提前刷新等方案。
- 热点永不过期会隐藏失效问题；缓存无界增长会把数据库容量问题变成 Redis 内存问题。
- 缓存是派生数据，原则上可丢弃重建；不要只写缓存再异步写事实库，除非明确选择并能承担 write-behind 的丢数据窗口。

**成熟实现**：Java 可用 Spring Cache + Caffeine（本地）或 Spring Data Redis；Go 可用 Caffeine 思路的本地缓存库、`go-redis`，进程内重复调用抑制用 `golang.org/x/sync/singleflight`。框架只提供机制，key、TTL、一致性和失效仍是业务设计。

### 6.2 CQRS + Materialized View：为写入和查询建立不同模型

**识别信号**：写侧需要规范化数据和严格不变量，但读侧需要跨表聚合、搜索、排序或直接适配页面 DTO。

CQRS 的最低成本形式只是代码层分离：Command 修改聚合，Query 返回专用 DTO，仍使用同一个数据库。只有读写负载、schema 或存储技术确实不同，才拆成独立 read store：

```text
Command → Write Model → Outbox/Event → Projector → Read Model → Query
```

Materialized View 是预先计算的查询结果，可以是 PostgreSQL materialized view、普通 projection 表、Redis 结构或搜索索引。独立 read model 通常最终一致，必须定义：

- 用户写后立刻读，是等待 projection、临时读主库，还是接受旧值？
- Projector 如何幂等、保存消费进度和处理乱序？
- read model 损坏后能否从事实源重建？重建期间怎样提供服务？
- schema 变更是原地迁移，还是建立 `v2` projection 后切流？

**不要默认组合 Event Sourcing**。普通数据库当前状态 + Outbox 已能支撑大量 CQRS 场景。

### 6.3 Keyset / Cursor Pagination：稳定而高效地继续读取

`LIMIT/OFFSET` 适合数据少、页数浅、需要跳到任意页的后台界面。深分页时数据库仍要计算并跳过前面的行；并发插入/删除还可能导致重复或漏项。

Keyset Pagination 使用上一页最后一条记录的排序键继续：

```sql
SELECT id, created_at, total_amount
FROM orders
WHERE (created_at, id) < (:cursor_created_at, :cursor_id)
ORDER BY created_at DESC, id DESC
LIMIT :page_size;
```

要点：

- 排序必须稳定且唯一，所以通常用 `(created_at, id)`，不能只用可能重复的时间。
- 建立与 `WHERE + ORDER BY` 匹配的复合索引。
- Cursor 是不透明 token，编码排序值、方向和必要的查询版本；服务端校验，客户端不要拼 SQL 值。
- 过滤条件不能在翻页中途变化；需要稳定快照时还要额外定义一致性边界。

### 6.4 Partition / Sharding / Consistent Hashing：把容量边界显式化

**识别信号**：经过索引优化、归档、纵向扩容、缓存与读副本后，单库的写吞吐、存储或维护窗口仍达到硬边界。

常见策略：

- Range：按时间或 ID 范围，范围查询友好，但新数据容易形成热点。
- Hash：按 `hash(shard_key)` 均匀分布，点查稳定，但范围查询和扩容迁移更复杂。
- Directory / Lookup：维护 `tenant_id → logical_shard` 映射，方便迁移大租户，但路由表成为关键依赖。
- Geographic：按数据驻留与访问区域分片，再在区域内二次分片。

Consistent Hashing 更常用于缓存节点或无状态分区，减少节点增减时需要迁移的 key；数据库分片通常还需要虚拟分片或目录层，因为事务、热点租户、迁移审计比纯 key 分布更重要。

分片前必须回答：shard key 是否出现在主要查询里？跨分片事务、唯一约束、分页与聚合怎么处理？热点如何迁移？路由变更期间是否双写？备份恢复以单 shard 还是全局为单位？

分片是长期架构成本，不是数据库慢的第一解法。

### 6.5 Event Sourcing：以不可变事件作为事实源

普通审计日志是在当前状态之外记录“谁改了什么”；Event Sourcing 则不直接覆盖当前状态，而是追加领域事件，并通过 replay 得到状态：

```text
AccountOpened → MoneyDeposited → MoneyWithdrawn → current balance
```

适合：业务天然由事件表达、必须保留意图与完整历史、需要按时间重建状态或生成新 projection，例如账本、交易风控、复杂审计领域。

必须设计：

- 每个 aggregate stream 的顺序版本，使用 `UNIQUE(stream_id, version)` 做乐观并发控制。
- 事件不可修改；schema 演进使用 upcaster 或兼容读取，不能直接改历史 JSON。
- snapshot 只是 replay 加速缓存，不是真相源。
- projection 可丢弃重建，因此处理器必须幂等且可保存 checkpoint。
- 修正错误使用新的 compensating event，不能 `UPDATE` 旧事件。

不适合普通 CRUD。它会引入事件版本、replay、projection 延迟、删除隐私数据和排障方式的系统性复杂度。CQRS 不等于 Event Sourcing，两者可以独立使用。

## 8. 模型选择表

| 问题 | 首选模型 | 不要先做什么 |
| --- | --- | --- |
| 用户必须立刻拿结果 | 同步请求 + deadline | 无界等待、层层重试 |
| 接口快速返回，稍后完成一个动作 | 持久化 Job Queue | `new Thread` / 裸 goroutine |
| 单机缓存每分钟清理，丢一次无所谓 | 进程内 scheduler | 部署分布式调度平台 |
| 集群每天生成唯一账单 | 持久化 scheduler → 幂等 Job | 每个副本 `@Scheduled` 直接做业务 |
| 数百万行日终处理 | Batch + chunk + checkpoint | 一个大事务、一次全读入内存 |
| 等待付款/审批数天 | Durable Workflow | 数据库状态 + 大量散落回调和 cron |
| DB 更新后可靠通知其他服务 | Transactional Outbox | DB commit 后直接 publish |
| 跨服务业务事务 | Saga + 幂等 + 补偿 | 分布式大事务作为默认方案 |
| 多消费者独立消费并可重放 | Event Stream | 把流当普通工作队列 |
| 多 Worker 抢数据库任务 | `FOR UPDATE SKIP LOCKED` + lease | `SELECT` 后再无条件 `UPDATE` |
| 多租户任务共享 Worker | 分队列配额 + Weighted Fairness | 只用一个全局 Priority Queue |
| 同一业务 key 必须有序 | partition / key affinity + 单 key 串行 | 把整个系统降成单 Worker |
| 消费速度跟不上生产速度 | 有界 in-flight + backpressure | 增大无界内存队列 |
| 海量任务成本差异很大 | Weighted Semaphore + 大任务队列 | 只按任务数量限制并发 |
| 乱序事件按发生时间统计 | event-time window + watermark | 按服务器收到时间直接聚合 |
| MQ 消费产生数据库副作用 | manual ack + Inbox 本地事务 | 自动 ack 后再写数据库 |
| 防止同一请求重复创建资源 | Idempotency Key + unique constraint | 先查再插 |
| 修复罕见漏事件/未知结果 | Reconciliation | 假设主链路永不失效 |
| 热点读取压垮数据库 | Cache-Aside + Singleflight | 给所有查询无差别加缓存 |
| 写模型合理但页面查询复杂 | CQRS + Materialized View | 为页面破坏写侧不变量和范式 |
| 大数据列表持续翻页 | Keyset / Cursor Pagination | 深 `OFFSET` 分页 |
| 一个请求查询多个独立下游 | 有界 Fan-out / Fan-in | 无界 goroutine / 公共线程池 |
| 单库写入或存储确实到达上限 | Sharding + 稳定路由层 | 未测量瓶颈就分库分表 |
| 完整历史本身就是业务事实源 | Event Sourcing + Projection | 把普通审计日志包装成 Event Sourcing |

## 9. 十三个练习项目

不要把每个项目分别用 Java 和 Go 重写一遍。选一个主语言完成，另一个语言只阅读等价框架的 API；学习目标是模型，不是重复 CRUD。项目一到七和十到十三是主线，项目八、九用于按需拓宽数据架构边界。

每个项目都必须产出可运行代码，而不是只有设计稿：数据库 migration、接口或消息 schema、Worker / Handler、可重复的启动方式、管理或查询入口、关键指标，以及一份故障演练记录。先完成最小正确版本，再加入扩展项；不要一开始写通用平台。

### 项目一：可靠的延迟任务服务

**目标**：订单创建 30 分钟未支付则关闭；支付后立即阻止关闭；关闭后异步发送 Webhook。

**建议栈**：

- Go：PostgreSQL + River；如果现有系统已强依赖 Redis，可用 Asynq。
- Java：PostgreSQL + JobRunr。开源版默认不能加入 Spring 业务事务；需要同事务入队可购买 Transaction Plugin，或者在业务事务中写 `scheduled_commands`，由 relay / 周期扫描器补充创建 Job。

**精确流程**：

1. `POST /orders` 在数据库事务中创建 `orders`。Go / River 使用同一个 `tx` 调 `InsertTx` 插入 `ExpireOrder(order_id)`；Java / JobRunr 开源版在同一事务写 `scheduled_commands`，relay 再创建 Job。命令的执行时间都是 `expires_at`。
2. Worker 使用条件更新：仅把 `status=CREATED AND expires_at<=now()` 更新为 `EXPIRED`。
3. 关闭成功后创建 `DeliverWebhook(order_id, event_version)`；任务唯一键使用 `order_id + event_version + endpoint_id`。
4. Webhook 请求携带稳定 `Idempotency-Key`；超时后使用同一个 key 重试。
5. 永久 `4xx` 进入 discarded / DLQ；`429` 和瞬时 `5xx` 按 `Retry-After` 或带 jitter 的退避重试。

**必须能解释**：为什么取消 Job 不是正确性的唯一保障（取消与执行可能并发）；为什么仍需订单状态条件更新；为什么框架声称单次执行也不能替代 Webhook 幂等。

**故障演练**：在更新订单后、确认 Job 成功前强制退出 Worker；同时启动多个 Worker；让 Webhook 服务先超时但实际落库；把 Worker 停机一小时再恢复。观察重复、misfire、租约恢复和最终状态。

### 项目二：可重启的账单文件批处理

**目标**：每日导入一个百万行 CSV，校验、入账、汇总；坏行不阻塞整批；失败后从 checkpoint 继续。

**建议栈**：Java + Spring Batch + PostgreSQL，由 Kubernetes CronJob 或 Quartz 触发。

**数据模型**：

- `file_manifest(file_id, sha256, business_date, state, total_rows)`，`sha256` 唯一。
- `import_rejects(file_id, line_no, reason_code, raw_record)`。
- Spring Batch 自带的 Job / Step execution 与 execution context 表。

**精确流程**：

1. 用 `file_id + business_date` 定义 Job Instance，重复上传同文件不创建第二次业务导入。
2. 每 500～2000 行一个 chunk；Reader 保存文件偏移/行号，Writer 在事务内批量 upsert。
3. 格式错误写 reject 表并 skip；数据库短暂死锁可 retry；字段语义错误不可 retry。
4. 汇总 Step 只在导入 Step 完成后运行；最终生成可核对的总行数、成功数、拒绝数、金额合计。

**必须能解释**：scheduler 与 batch 的边界；chunk 大小如何影响吞吐、回滚成本和锁时间；为什么 checkpoint 与业务写入要有一致边界。

### 项目三：可等待、可补偿的订单履约 Workflow

**目标**：创建订单 → 预留库存 → 等待付款（最长 24 小时）→ 扣款 → 创建发货单；取消或超时后释放库存；人工可查询和取消。

**建议栈**：Temporal + Java SDK 或 Go SDK；库存、支付、物流各用一个本地模拟服务和独立数据库。

**精确流程**：

1. 以 `order_id` 作为 Workflow ID，避免同一订单并行启动两条主流程。
2. `ReserveInventory`、`ChargePayment`、`CreateShipment` 都是 Activity，并接受稳定的业务幂等键。
3. Workflow 用 durable timer 等待付款，用 Signal 接收 `payment_confirmed` / `cancel_requested`。
4. 网络错误自动重试 Activity；库存不足、卡被拒等业务错误标成 non-retryable。
5. 付款超时执行 `ReleaseInventory`；扣款后建发货单永久失败时进入人工处理，不假装所有副作用都可自动回滚。
6. 为运行中 Workflow 暴露 Query：当前阶段、最近错误、已完成的补偿。

**故障演练**：在每个 Activity 前后杀 Worker；流程等待中重启整套应用；重复发送 Signal；Activity 成功但响应丢失；升级 Workflow 代码后 replay 旧历史。

**必须能解释**：event history、replay、确定性、Activity 与 Workflow 的边界、长 timer 为什么不占线程、补偿为什么也要幂等。

### 项目四：Outbox + 消费者 Inbox + 对账

**目标**：订单事务提交后可靠发布 `OrderCreated`；积分与通知两个消费者独立处理；支持重放和对账。

**建议栈**：PostgreSQL + Debezium Outbox Event Router + Kafka。若暂时不想部署 CDC，第一版可用 polling publisher，第二版再替换 relay，业务事务与事件 schema 不变。

**Outbox 关键字段**：

```text
event_id, aggregate_type, aggregate_id, aggregate_version,
event_type, payload, occurred_at
```

**精确流程**：

1. 创建订单与插入 outbox event 在同一数据库事务中完成。
2. relay 发布时以 `aggregate_id` 作为 partition key，保留同一订单的事件顺序。
3. 每个消费者在自己的数据库中用 `(consumer_name, event_id)` 唯一 Inbox 去重，并与业务更新同事务提交。
4. 事件 envelope 带 `event_id`、`event_type`、`schema_version`、`occurred_at`、`trace_id`。
5. 对账任务比较订单事实源、outbox 与关键消费者处理结果，差异写入 reconciliation 表后再安全重放。

**必须能解释**：Outbox 解决什么、不解决什么；为什么 producer confirm 后仍可能重复；offset commit 与业务事务的窗口；为什么 schema 删除字段比新增可选字段危险。

### 项目五：抗击穿的商品读取服务

**目标**：实现商品详情读写 API，在热点 key 过期和 Redis 故障时仍保护 PostgreSQL，并能说清数据最多陈旧多久。

**建议栈**：PostgreSQL + Redis；Java 使用 Spring Data Redis / Lettuce，Go 使用 `go-redis`；Go 进程内合并回源可直接用 `golang.org/x/sync/singleflight`。

**精确流程**：

1. `GET /products/{id}` 先读 `product:{id}:v1`；miss 时按 product ID 做 singleflight，再查 PostgreSQL并以 `base_ttl + random_jitter` 回填。
2. 不存在的商品写短 TTL negative cache；`PUT /products/{id}` 创建或更新后必须删除正、负缓存 key。
3. 更新路径先提交 PostgreSQL，事务提交后 `DEL` 缓存；对强失效要求高时，在同事务写 `cache_invalidations` Outbox，由 relay 重试删除。
4. Redis 超时采用很短 deadline 并回源数据库；数据库回源再受 semaphore 和速率限制保护，不能在 Redis 故障时让全部请求穿透。
5. 暴露 hit rate、miss rate、load duration、singleflight shared count、回源并发和 Redis error rate。

**故障演练**：让同一个热点 key 在 500 个并发请求前同时过期；停止 Redis；制造删除缓存失败；在更新和读取间交错执行，测量实际陈旧窗口与数据库 QPS。

**必须能解释**：Cache-Aside 为什么只能提供有界最终一致；singleflight 为什么只抑制同一进程重复调用；TTL、主动失效与 Outbox 分别解决什么。

### 项目六：订单查询 Read Model 与 Cursor API

**目标**：写侧保持规范化订单、订单项和付款表，读侧提供运营列表：按状态、用户和时间筛选，返回金额汇总，并支持稳定翻页。

**建议栈**：先全部使用 PostgreSQL：业务表 + Outbox + `order_list_projection`。只有明确需要全文检索、相关性排序时，再把 read model 换成 Elasticsearch / OpenSearch。

**精确流程**：

1. Command 只修改写模型并产生 `OrderCreated`、`OrderPaid`、`OrderCancelled` 等 Outbox 事件。
2. Projector 以 `order_id` upsert `order_list_projection`，保存 `last_event_version`；只接受更大的 aggregate version，重复与旧事件直接忽略。
3. `GET /admin/orders?status=&after=&limit=` 按 `(created_at DESC, order_id DESC)` 查询，并建立包含过滤字段和排序字段的复合索引。
4. `after` 编码最后一行的 `created_at + order_id + query_version`，返回 `items + next_cursor`；修改过滤条件必须重新开始分页。
5. 提供 rebuild 命令创建 `order_list_projection_v2`，从事实源或保留事件重放，核对行数与校验和后原子切换查询版本。

**必须能解释**：CQRS 为何不等于两套数据库；read-your-writes 如何处理；为什么 `(created_at, id)` 比单独时间稳定；projection 为什么必须可重建。

### 项目七：有总预算的价格聚合 API

**目标**：并行请求库存、基础价格、促销和会员权益，500ms 内返回报价；允许促销不可用时降级，但基础价格失败必须整体失败。

**建议栈**：Go 使用 `context` + `errgroup` / semaphore；Java 使用有界 Executor + `CompletableFuture`，并用 Resilience4j 配置 timeout、bulkhead 与 circuit breaker。

**精确流程**：

1. Handler 创建 500ms 总 deadline；每个下游获得不超过剩余预算的子 timeout。
2. 只并行启动相互独立的调用；促销依赖基础价格时，应在价格返回后再进入下一阶段，不能伪并行。
3. 库存、价格是 required result；促销、权益是 optional result。聚合器显式返回 `degraded_components`，不能默默把未知当 0。
4. 每个下游使用独立并发配额；请求取消或 required result 失败时取消尚未完成的子调用。
5. 记录整体延迟、各分支延迟、取消数、降级率和 breaker 状态。

**故障演练**：分别让一个下游变慢、挂死、持续 `500`、忽快忽慢；验证总耗时不会成为所有 timeout 之和，也不会因大量慢请求耗尽公共线程池。

**必须能解释**：fan-out 与 pipeline 的区别；all-of、partial result 的业务语义；timeout、bulkhead 与 circuit breaker 为什么不能相互替代。

### 项目八：多租户数据分片实验

**目标**：将租户数据路由到两个 PostgreSQL shard，支持增加第三个 shard、发现热点租户，并安全迁移一个租户。

**建议栈**：两个或三个 PostgreSQL 实例 + 一个独立 `shard_map` 数据库；路由封装在单独 repository / DAO 层，业务代码禁止直接选择 DataSource。

**精确流程**：

1. `shard_map(tenant_id, logical_shard, state, version)` 维护 lookup routing；请求没有可信 `tenant_id` 时拒绝执行。
2. 每个 shard 的表与迁移版本一致，主键或唯一约束包含 `tenant_id`；普通业务查询必须命中单 shard。
3. 管理端全局统计使用有并发上限的 fan-out，并显式标识失败 shard；不要在用户请求路径做跨 shard join。
4. 迁移租户时短暂冻结该租户写入，复制数据并校验行数/校验和，事务更新 shard map 后再开放写入；第一版不实现难以证明正确的双写迁移。
5. 对路由缓存设置版本与失效机制；旧路由写入必须被目标 shard 的租户归属检查拒绝。

**故障演练**：关闭一个 shard；让某租户产生远高于其他租户的流量；迁移过程中终止复制；增加 shard 后比较 `hash % N` 与 lookup / virtual shard 需要移动的数据量。

**必须能解释**：为什么 shard key 由查询和事务边界决定；为什么分片不能解决慢 SQL；跨 shard 唯一性、事务、分页和备份恢复分别增加了什么成本。

### 项目九：Event-Sourced 钱包

**目标**：钱包支持开户、入金、扣款和冻结；所有余额变化以不可变事件保存，同时维护余额与交易明细 projection。

**建议栈**：Java 优先用稳定版 Axon Framework + Axon Server / 持久 Event Store；Java 或 Go 也可用 KurrentDB 官方 SDK，但要注意 KurrentDB 当前是 source-available 的 KLv1，并非 OSI 开源许可证。用 PostgreSQL 自建 append table 只适合作为受控学习实验，不应直接当生产 Event Store。

**核心事件**：`WalletOpened`、`MoneyDeposited`、`MoneyWithdrawn`、`WalletFrozen`、`WithdrawalReversed`。

**精确流程**：

1. Command Handler 读取 wallet stream，replay 得到当前状态，验证余额与冻结规则，然后以 expected stream version 追加事件。
2. 两个并发扣款只有一个能在相同期望版本上成功；另一个重新读取后重新验证，不能只把 append 机械重试。
3. Projector 分别维护 `wallet_balance_projection` 和 `wallet_transactions_projection`，用 event position / ID 保证重复投递安全。
4. 每隔合理事件数创建 snapshot；删除全部 snapshot 后仍必须能从事件完整恢复。
5. 增加新事件字段时保持旧事件可读；业务纠错追加 reversal event，不修改历史事件。

**故障演练**：并发扣款、projection 中途退出、重复投递、从零 rebuild、旧版本事件 replay、新 projection 与旧 projection 并行切换。

**必须能解释**：审计表与 Event Sourcing 的根本区别；stream version 如何保护业务不变量；snapshot 为什么不是事实源；何时这个复杂度不值得。

### 项目十：资源感知的多 Worker 任务服务

**目标**：接收缩略图、报表和 Webhook 三类任务；多个实例竞争执行，同时保证 CPU 密集任务不会占满机器、单租户不会挤占全部资源、同一报表不会并发生成，并支持平滑停机。

**建议栈**：Go + River + PostgreSQL。River 负责持久化、任务租约、重试和队列；你只实现业务路由、资源准入与 Handler，不自研通用任务队列。Java 可用 JobRunr 完成同一项目。

**最小代码结构**：

```text
cmd/api                 # 接收任务、查询状态
cmd/worker              # 注册 queue 与 handler、处理停机
internal/jobs           # ThumbnailArgs / ReportArgs / WebhookArgs
internal/handlers       # 三种业务处理器
internal/admission      # weighted permit、租户配额
internal/repository     # job_requests 与业务幂等操作
internal/observability  # queue_wait、run_duration、in_flight
migrations
```

**数据与队列**：

- `job_requests(job_id, tenant_id, request_key, kind, state, estimated_cost, object_key, created_at)`，唯一键为 `(tenant_id, request_key)`。
- `cpu` queue 执行缩略图，Worker 数从容器有效 CPU 数开始压测；`io` queue 执行 Webhook；`bulk` queue 生成报表。三者分别配置 `MaxWorkers`，不共享一个无界池。
- 大 payload 存 MinIO / S3，任务参数只带 `object_key`、`version` 和 checksum。

**核心代码形状**：先取得资源许可，再分配大对象；许可失败应延迟重试，不能在内存中等待大量任务。

```go
type CostedJob interface {
	TenantID() string
	CostUnits() int64
	Run(context.Context) error
}

func (h *Handler) Work(ctx context.Context, job CostedJob) error {
	release, err := h.admission.Acquire(ctx, job.TenantID(), job.CostUnits())
	if err != nil {
		return err
	}
	defer release()
	return job.Run(ctx)
}
```

第一版用进程内 `semaphore.Weighted` 控制单实例内存成本；第二版增加 `resource_leases(lease_id, tenant_id, units, owner, expires_at)`，通过事务和到期回收实现跨实例租户配额。这个租约只用于容量准入，不保护业务不变量；业务正确性仍由唯一约束或条件更新保证。

**精确流程**：

1. `POST /jobs` 校验类型、对象 metadata 和租户配额，在事务中写 `job_requests` 并用 River `InsertTx` 入队。
2. Worker 从对应 queue 取任务，检查 deadline；过期任务直接标记 `expired`，不再浪费 CPU。
3. 取得 weighted permit 后执行。报表按 cursor 分块查询并流式写对象存储，不能先把全部行读进内存。
4. 相同 `(tenant_id, request_key)` 返回既有 `job_id`；Handler 更新最终业务结果时也用条件更新。
5. 收到 `SIGTERM` 后停止 fetch，给 in-flight 固定宽限期；超时任务取消或让 River lease 到期后由其他 Worker 重投。

**故障演练**：同时提交 1000 个小任务和 10 个大任务；让一个租户持续灌入任务；执行中 kill Worker；缩短停机宽限期；故意把 prefetch 调大，观察 RSS 和队列等待时间。

**阅读源码**：对照本目录 River 的 job fetch / lease / retry 实现和 Asynq 的 processor / recoverer，画出“认领—续租—完成—失联恢复”的时序图。

**必须能解释**：为什么 Worker 数、prefetch、连接池和内存预算必须一起计算；为什么 priority 不等于 fairness；为什么进程内 semaphore 不能提供集群总配额；为什么容量租约不能替代业务幂等。

### 项目十一：确认、重试和重放都正确的 MQ 消费者

**目标**：订单服务发布 `GenerateInvoice`，账单服务消费后写 PostgreSQL；允许重复投递，不允许重复生成账单；支持分级退避、DLQ、人工重放和平滑停机。

**建议栈**：RabbitMQ + PostgreSQL。Java 使用官方 RabbitMQ Java Client / Spring AMQP，Go 使用 `amqp091-go`。RabbitMQ 更容易直接观察 ack、prefetch、redelivery 和 routing；完成后再把同一业务 Handler 接到 Kafka，比较 offset / rebalance 语义。

**消息与表**：

```json
{
  "message_id": "uuid",
  "type": "GenerateInvoice",
  "schema_version": 1,
  "tenant_id": "t-1",
  "order_id": "o-1",
  "occurred_at": "2026-09-02T10:00:00Z",
  "trace_id": "..."
}
```

```text
consumer_inbox(consumer_name, message_id, processed_at,
               payload_hash, result_ref)
invoices(invoice_id, tenant_id, order_id, state, object_key, version)
message_failures(message_id, queue, attempt, error_code,
                 first_failed_at, last_failed_at, payload_ref)
UNIQUE(consumer_name, message_id)
UNIQUE(tenant_id, order_id)
```

**RabbitMQ topology**：主 exchange / queue 为 `invoice.x` / `invoice.q`；建立 `invoice.retry.5s`、`invoice.retry.30s`、`invoice.retry.5m` 三个带 TTL 的队列，过期后 dead-letter 回主 exchange；超过上限路由到 `invoice.dlq`。不要用一个包含任意 TTL 的重试队列，否则队首长延迟消息会挡住后面的短延迟消息。

**最小代码结构**：

```text
publisher/confirmed_publisher
consumer/runner             # fetch、manual ack/nack、shutdown
consumer/invoice_handler    # 纯业务入口
repository/inbox
repository/invoice
retry/classifier            # retryable / permanent
admin/redrive               # 查询、校验、审计、重放
```

**关键事务与确认顺序**：

```text
receive message
  → BEGIN
  → INSERT consumer_inbox             # 唯一冲突：已经成功处理
  → INSERT/UPDATE invoices             # 同一数据库本地事务
  → COMMIT
  → ACK RabbitMQ
```

若 Inbox 唯一冲突，核对 payload hash 后直接 ack；同一 `message_id` 携带不同 payload 必须告警。业务事务失败则不 ack，根据错误类型 publish 到固定 retry tier 或 DLQ；只有 retry publish 获得 broker confirm 后才 ack 原消息，避免搬运过程中丢失。

**精确流程**：

1. Producer 先用 Outbox 发布，publisher 开启 confirm；连接断开且结果未知时以同一 `message_id` 重发。
2. Consumer 关闭 auto-ack，prefetch 从 `并发数 × 每 Worker 少量窗口` 开始压测，并用 payload 字节预算校验不会超内存。
3. `invalid_schema`、权限或业务拒绝直接 DLQ；连接失败、`429`、短暂 `5xx` 才进入 retry tier。
4. Redrive API 要求操作者、原因和筛选条件；重放沿用原 `message_id`，不会再次生成账单。
5. 收到停止信号后 cancel consumer，等待当前事务与 ack 完成，再关闭 channel / connection。

**Kafka 对照实验**：用 `order_id` 作为 key；关闭自动提交，在数据库事务成功后提交 offset。让单条处理超过 `max.poll.interval.ms`，观察 rebalance 和重复处理；再改成 pause / resume 或把长工作转成项目十的持久 Job。不要在内存线程池处理完之前提前提交 offset。

**故障演练**：在 DB commit 后、ack 前 kill 进程；在 retry publish confirm 前后断网；制造 poison message；把 prefetch 放大 100 倍；重放整个 DLQ 两次。

**必须能解释**：publisher confirm 为什么不是 exactly-once；manual ack 放在事务前后分别会怎样；prefetch 与 Worker 并发有什么区别；DLQ 为什么必须有 redrive 审计；Kafka partition 内有序为何不等于业务副作用只发生一次。

### 项目十二：CPU 与内存有硬预算的媒体处理流水线

**目标**：接收对象存储中的大图片，校验、生成三种缩略图并上传；即使同时到来大量超大图片，Pod 也不 OOM，API 和 Webhook Worker 也不被拖死。

**建议栈**：Go + River + MinIO + libvips。Go 负责可靠任务和资源编排，成熟的 libvips 负责图片解码与缩放；不要自己实现图片算法。也可用 Java + JobRunr + libvips 子进程完成。

**执行 pipeline**：

```text
read metadata → validate limits → acquire pixel/memory permits
→ stream object to quota-controlled temp file
→ libvips transform → stream upload → persist result → cleanup
```

**硬边界**：

- 上传对象大小、解码后像素数、宽高、输出格式和处理 deadline 都有上限；防止小压缩包解码成巨大内存对象。
- `image_cpu` queue 的并发从有效 CPU 数开始；libvips 内部并发也要限制，不能“外层 8 个任务 × 每个内部 8 线程”。
- weighted permit 按估算的解码内存或像素数计算，大图消耗多个单位；许可在打开解码器前获取。
- 临时目录有总字节配额和每任务目录；用 `defer` / `finally` 清理，启动时扫描并删除已过期孤儿目录。
- API 只写任务和返回 `202 + job_id`，不在 HTTP 请求线程内做变换。

**最小代码结构**：

```text
internal/media/metadata.go       # 只读取 header，计算成本
internal/media/limits.go         # 大小、像素、格式、deadline
internal/media/transform.go      # CommandContext 调 libvips
internal/storage/reader.go       # 流式下载
internal/storage/writer.go       # 流式上传
internal/resources/budget.go     # CPU slot、weighted memory、temp quota
internal/jobs/thumbnail.go       # 编排与状态更新
```

子进程使用 `exec.CommandContext`，stderr 限长保存，退出码映射成 retryable / permanent；context 取消时必须终止整个子进程组。上传结果使用包含输入对象 version 和转换参数 hash 的确定 key，使重复执行覆盖同一结果，而不是生成多份文件。

**观测与验收**：记录 queue wait、active tasks、permit wait、估算/实际输入字节、转换耗时、子进程 RSS、temp bytes 和 OOM count。用固定容器 limit 连续提交不同尺寸图片，证明吞吐下降时 RSS 仍有上界；再关闭 MinIO，证明有界 buffer 会反压而不是持续下载到内存。

**必须能解释**：为什么限制任务数不足以限制内存；为什么 streaming 仍可能在解码阶段占大内存；为什么容器 memory limit 不是应用的内存预算；为什么 GOMEMLIMIT 不能限制 libvips 的 native memory。

### 项目十三：处理乱序与迟到数据的事件时间聚合

**目标**：设备每 10 秒上报温度，但事件可能乱序、重复或延迟 10 分钟；按设备计算 5 分钟平均值和最近 15 分钟滑动最大值，并把过晚事件放入可查询出口。

**建议栈**：Java + Kafka + Apache Flink。Flink 已提供 event time、watermark、window、checkpoint 和状态后端；这个项目不应在 Go 中手写一个流处理引擎。

**输入与输出**：

```text
DeviceReading(event_id, device_id, sequence, event_time, temperature,
              schema_version, received_at)
DeviceWindow(device_id, window_start, window_end, count, avg, max,
             last_updated_at)
LateReading(event_id, device_id, event_time, watermark, reason)
```

Kafka 输入 topic 以 `device_id` 为 key。同一设备通常进入同一 partition，但重试仍会重复；source 后按 `event_id` 去重并给 state 设置 TTL。输出以 `(device_id, window_start, window_type)` 作为幂等 key，写 compacted Kafka topic；若落 PostgreSQL，使用同一复合唯一键 upsert。

**最小代码结构**：

```text
model/DeviceReading.java
source/ReadingDeserializer.java
time/ReadingWatermarkStrategy.java
pipeline/TemperatureAggregationJob.java
window/Aggregates.java
sink/WindowUpsertSink.java
sink/LateReadingSink.java
```

**精确流程**：

1. 解析失败或未知 schema 进入 `readings.invalid`，不能让单条坏消息无限重启 Job。
2. 分配 event-time timestamp，watermark 使用“最大已见 event time - 2 分钟”，并配置 idle partition，避免一个无数据 partition 阻止全局 watermark 前进。
3. `keyBy(device_id)` 后创建 5 分钟 tumbling window 和 15 分钟 sliding window；允许迟到 10 分钟，并把超过 allowed lateness 的记录送到 side output `readings.late`。
4. 开启 checkpoint，重启后恢复 source offset、dedupe state 与 window state。外部 PostgreSQL sink 仍按复合键幂等 upsert，不能仅因 Flink checkpoint 就假设任意外部副作用 exactly-once。
5. 增加告警支线：连续高温必须持续 30 秒才发出，低于阈值后解除；用 keyed state + timer 实现 debounce，而不是为每个设备创建线程和 `sleep`。

**故障演练**：随机打乱 20% 事件、重复 10%、延迟一批超过 10 分钟；停止一个 partition 的生产；checkpoint 中途 kill TaskManager；分别改变 watermark 和 allowed lateness，比较结果正确性、等待时间与 state 大小。

**必须能解释**：processing time 与 event time 的区别；watermark 为什么是完整性与延迟的权衡；partition 顺序为何仍不能消除迟到；window state 为什么需要 TTL / 清理；checkpoint 的一致性边界在哪里。

## 10. Java / Go 工具边界

| 能力 | Java | Go | 选择依据 |
| --- | --- | --- | --- |
| 进程内定时 | Spring `@Scheduled` | `go-co-op/gocron/v2` | 可接受进程重启丢触发 |
| 持久化调度 | Quartz | 通常选基础设施调度或持久 Job Queue | 需要 trigger、misfire、集群 |
| 后台任务 | JobRunr | River / Asynq | 已有 PostgreSQL 选 River；已有 Redis 才优先 Asynq |
| 进程内执行池 | bounded `ThreadPoolExecutor`；阻塞 I/O 可用 virtual thread + `Semaphore` | bounded channel + goroutine + `x/sync/semaphore` | 并发和排队都必须有上限 |
| 批处理 | Spring Batch | 无统一事实标准 | 大规模可重启批处理优先成熟框架 |
| 长工作流 | Temporal Java SDK | Temporal Go SDK | 持续时间长、需等待/信号/补偿 |
| 调用韧性 | Resilience4j | `context` + 有界并发 + 按需库 | 先理解策略再套装饰器 |
| 缓存 | Spring Cache + Caffeine / Spring Data Redis | `go-redis` + `singleflight` | 先确定陈旧窗口、失效和回源保护 |
| Read Model | PostgreSQL projection，必要时 Elasticsearch / OpenSearch | 同左 | CQRS 是模型边界，不由语言框架决定 |
| Cursor Pagination | PostgreSQL 复合索引 + 不透明 cursor | 同左 | 排序键必须稳定且唯一 |
| 数据分片 | 优先数据库原生/托管能力，路由封装在 DAO | 同左 | 单库确实到达边界后再引入 |
| Event Sourcing | Axon Framework / KurrentDB Java SDK | KurrentDB Go SDK | 仅用于事件历史是事实源的领域 |
| 工作队列 MQ | Spring AMQP / RabbitMQ Java Client | `amqp091-go` | 学习 ack、confirm、prefetch、DLQ |
| 事件流 | Spring Kafka / 原生客户端 | franz-go 等客户端 | 中间件由语义与团队运维能力决定 |
| 事件时间窗口 | Apache Flink / Kafka Streams | 优先使用独立流处理基础设施 | 不在业务服务中自研 watermark 与状态恢复 |
| 运行时资源 | JVM heap / native memory + 容器 limit | `GOMAXPROCS`、`GOMEMLIMIT` + 容器 limit | 运行时限制不能替代应用准入和有界队列 |
| 可观测性 | OpenTelemetry + Micrometer | OpenTelemetry + Prometheus client | 统一传播 trace context 和业务 ID |

选型时不要把“少部署一个组件”视为绝对优势。JobRunr / River 可复用业务数据库，入门简单；River 原生支持事务入队，JobRunr 则需 Pro Transaction Plugin 或 Outbox 才能与业务事务原子提交。数据库队列在高吞吐下可能争用业务数据库。Redis 队列延迟低，但增加 Redis 的持久化与一致性边界。Temporal 能显著减少长流程自研代码，但引入专门服务、事件历史和确定性约束。

## 11. 利用本目录已有源码学习

当前目录已有三个很好的实现样本：

- [`river`](./river)：先读 `riverdriver/riverpgxv5/internal/dbsqlc/river_job.sql` 中的 `FOR UPDATE SKIP LOCKED`，再读 `internal/jobexecutor/job_executor.go` 的成功/失败状态转换、`internal/leadership/doc.go` 的数据库租约。
- [`asynq`](./asynq)：先读 `processor.go` 的 dequeue、lease 与成功确认，再读 `recoverer.go` 如何回收 lease 过期任务，最后看 `internal/base/base.go` 的 Redis key 与任务状态。
- [`rocketmq`](./rocketmq)：先从 `store/.../CommitLog` 与 consumer queue 的关系理解“顺序追加日志 + 派生消费索引”，不要一开始逐包通读整个项目。

源码阅读采用固定问题，而不是按文件顺序阅读：任务如何原子认领？Worker 死亡怎样发现？重试时间放哪？完成状态何时写？旧 Worker 是否可能晚到写入？指标和管理入口在哪里？完成项目十时再增加四个问题：fetch 是否受并发空位约束？prefetch 的 payload 存在哪里？停机先停止哪一层？执行池饱和后任务去哪？

## 12. 二十二周学习顺序

| 周 | 主题 | 产出 |
| --- | --- | --- |
| 1 | timeout、deadline、取消、有界并发 | 一个会正确取消的同步聚合 API |
| 2 | 幂等、唯一约束、乐观锁 | 幂等创建 API 与订单状态机 |
| 3～4 | Job Queue、lease、retry、DLQ | 完成项目一 |
| 5 | Scheduler、misfire、overlap、时区 | 给项目一补停机恢复与周期扫描 |
| 6～7 | chunk、checkpoint、restart、partition | 完成项目二 |
| 8 | Cache-Aside、TTL、Singleflight | 完成项目五 |
| 9 | CQRS、Projection、Cursor Pagination | 完成项目六 |
| 10 | Fan-out / Fan-in、总延迟预算、部分结果 | 完成项目七 |
| 11～12 | Temporal、replay、signal、compensation | 完成项目三 |
| 13 | Outbox / Inbox、partition、offset | 完成项目四 |
| 14～15 | confirm、ack、prefetch、retry tier、DLQ / redrive | 完成项目十一 |
| 16 | Worker Pool、租约、fairness、graceful shutdown | 完成项目十 |
| 17 | weighted permit、streaming、临时空间预算 | 完成项目十二 |
| 18～19 | event time、watermark、window、late event | 完成项目十三 |
| 20 | 对账、观测、故障演练、混合版本部署 | 仪表盘、操作手册；为主线项目写决策记录 |
| 21（选修） | shard key、路由、热点与迁移 | 完成项目八 |
| 22（选修） | Event Sourcing、stream version、projection | 完成项目九 |

学习是否完成，不以“Demo 正常跑通”为标准，而以能否回答以下问题为准：

- 在任意两步之间 kill 进程，系统会丢、重、卡住，还是恢复？
- 看到超时，能否判断结果是失败还是未知？
- 同一个业务操作来两次，会产生几次副作用？保证落在哪个唯一约束或幂等协议上？
- 停机一小时后启动，错过的触发如何处理？
- 下游故障十分钟，重试是否使流量更糟？
- 热点缓存同时失效，回源并发是否受控？数据最多陈旧多久？
- 翻到下一页时，新增和删除的数据会不会导致重复或漏项？
- 一个聚合请求的总耗时为何不会变成各子调用 timeout 之和？
- shard 路由变化后，旧实例能否把数据写到错误位置？
- 旧消息、旧 Workflow 与新代码是否兼容？
- 失败任务由谁发现，怎样查询、告警、修复和重放？

## 13. 官方资料索引

按学习顺序阅读，先读概念和失败语义，再读 Quickstart：

- [Go `context`](https://pkg.go.dev/context)
- [Go：Pipelines 与取消传播](https://go.dev/blog/pipelines)
- [Java：`ThreadPoolExecutor` 的排队与拒绝策略](https://docs.oracle.com/en/java/javase/17/docs/api/java.base/java/util/concurrent/ThreadPoolExecutor.html)
- [Java：Virtual Threads 的适用边界与并发限制](https://docs.oracle.com/en/java/javase/26/core/virtual-threads.html)
- [Go：容器感知的 `GOMAXPROCS`](https://go.dev/blog/container-aware-gomaxprocs)
- [Go：GC 与 `GOMEMLIMIT`](https://go.dev/doc/gc-guide)
- [Kubernetes：CPU / Memory request 与 limit](https://kubernetes.io/docs/concepts/configuration/manage-resources-containers/)
- [Resilience4j：Retry、Circuit Breaker、Bulkhead 等模块](https://resilience4j.readme.io/docs/getting-started)
- [AWS：Exponential Backoff and Jitter](https://aws.amazon.com/blogs/architecture/exponential-backoff-and-jitter/)
- [Stripe：Idempotent Requests](https://docs.stripe.com/api/idempotent_requests)
- [PostgreSQL `SELECT`：`SKIP LOCKED` 适用于 queue-like table](https://www.postgresql.org/docs/current/sql-select.html)
- [Redis：Cache-Aside 与缓存击穿保护](https://redis.io/docs/latest/develop/use-cases/cache-aside/)
- [Go `singleflight`：合并重复函数调用](https://pkg.go.dev/golang.org/x/sync/singleflight)
- [Microsoft：CQRS Pattern](https://learn.microsoft.com/en-us/azure/architecture/patterns/cqrs)
- [PostgreSQL：Materialized Views](https://www.postgresql.org/docs/current/rules-materializedviews.html)
- [PostgreSQL：大 `OFFSET` 的成本](https://www.postgresql.org/docs/current/queries-limit.html)
- [Stripe：Cursor-based Pagination](https://docs.stripe.com/api/pagination)
- [Microsoft：Pipes and Filters Pattern](https://learn.microsoft.com/en-us/azure/architecture/patterns/pipes-and-filters)
- [Microsoft：Sharding Pattern](https://learn.microsoft.com/en-us/azure/architecture/patterns/sharding)
- [Microsoft：Event Sourcing Pattern](https://learn.microsoft.com/en-us/azure/architecture/patterns/event-sourcing)
- [Axon Framework：Event Store 基础设施](https://docs.axoniq.io/axon-framework-reference/development/events/infrastructure/)
- [KurrentDB：事件原生数据库与 Java / Go SDK](https://docs.kurrent.io/server/latest/)
- [Quartz：Trigger 与 misfire](https://www.quartz-scheduler.org/documentation/quartz-2.3.0/tutorials/tutorial-lesson-04.html)
- [go-co-op/gocron v2](https://github.com/go-co-op/gocron)
- [Kubernetes CronJob：并发、漏调度与幂等提醒](https://kubernetes.io/docs/concepts/workloads/controllers/cron-jobs/)
- [JobRunr 文档](https://www.jobrunr.io/en/documentation/)
- [River 文档](https://riverqueue.com/docs)
- [Asynq 项目](https://github.com/hibiken/asynq)
- [SQS Visibility Timeout 与 DLQ](https://docs.aws.amazon.com/AWSSimpleQueueService/latest/SQSDeveloperGuide/sqs-visibility-timeout.html)
- [Spring Batch：框架边界与使用场景](https://docs.spring.io/spring-batch/reference/spring-batch-intro.html)
- [Temporal：Workflow Execution、event history 与 replay](https://docs.temporal.io/workflow-execution)
- [Temporal：Retry Policy](https://docs.temporal.io/encyclopedia/retry-policies)
- [AWS：Saga Patterns](https://docs.aws.amazon.com/prescriptive-guidance/latest/cloud-design-patterns/saga-patterns.html)
- [AWS：Transactional Outbox](https://docs.aws.amazon.com/prescriptive-guidance/latest/cloud-design-patterns/transactional-outbox.html)
- [Debezium Outbox Event Router](https://debezium.io/documentation/reference/stable/transformations/outbox-event-router.html)
- [RabbitMQ：Acknowledgements 与 Publisher Confirms](https://www.rabbitmq.com/docs/confirms)
- [RabbitMQ：Consumer Prefetch](https://www.rabbitmq.com/docs/consumer-prefetch)
- [Kafka：核心概念、partition 与事件顺序](https://kafka.apache.org/documentation/)
- [Kafka：Consumer Configs 的 poll 与超时配置](https://kafka.apache.org/documentation/#consumerconfigs)
- [Apache Flink：Event Time 与 Watermark](https://nightlies.apache.org/flink/flink-docs-stable/docs/dev/datastream/event-time/generating_watermarks/)
- [Apache Flink：Window 聚合](https://nightlies.apache.org/flink/flink-docs-stable/docs/learn-flink/streaming_analytics/)
- [Redis：Distributed Locks 与 fencing token 提醒](https://redis.io/docs/latest/develop/clients/patterns/distributed-locks/)
- [OpenTelemetry：Traces、Metrics、Logs](https://opentelemetry.io/docs/concepts/signals/)

## 14. 最后形成的判断习惯

遇到新需求时，不先搜索“Java/Go 怎么实现某功能”，先写出：

1. 业务事实源与状态机。
2. 执行模型：同步、Job、Schedule、Batch、Stream 或 Workflow。
3. 时序与顺序：processing time 还是 event time，顺序按哪个 key，迟到多久仍接收。
4. 资源预算：最大 in-flight、CPU、内存、连接、临时空间和单租户配额。
5. 数据访问模型：直接查询、缓存、Projection、Cursor 或 Shard。
6. 交付语义：允许丢失、重复、乱序和陈旧的边界。
7. 事务边界与无法原子化的窗口。
8. 幂等键、重试分类、超时和最终失败去向。
9. 崩溃恢复、对账、人工操作与可观测性。
10. 最后才是成熟框架与部署方式。

这套顺序的价值不在于记住更多名词，而在于形成条件反射：看到“定时任务”就追问 misfire 和 overlap；看到“多 Worker”就追问认领、租约、公平性和资源预算；看到“发消息”就追问 confirm、ack、幂等和 DLQ；看到“按时间统计”就追问 event time、watermark 和迟到数据；看到“开线程 / goroutine”就追问队列上限、取消和过载行为；看到“加缓存”就追问陈旧窗口、击穿和失效；看到“分片”就先验证单库是否真的到达边界；看到“长流程”就想到状态持久化、等待、补偿和版本演进。
