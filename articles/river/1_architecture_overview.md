# 01｜River 架构鸟瞰

- 目标：以鸟瞰视角理解 River（`github.com/riverqueue/river` v0.47.0，源码在 `../../codes/river/`）的整体架构、主要模块与数据流，回答两个问题：它能做什么、是怎么做到的。
- 适配：Go + PostgreSQL；自顶向下分层展开：最小使用示例 → 架构全貌 → 数据模型 → 三条核心数据流 → 模块速览与能力清单。

## 1. 定位与设计哲学

River 是 Go + PostgreSQL 的任务队列：**用已有的 Postgres 存任务，不引入 Redis/Kafka 等额外组件**。核心哲学是**事务性入队**（transactional enqueueing）——任务和业务数据写进同一个事务，事务提交则任务一定存在、回滚则一定不存在，避免了"业务写成功但任务丢了"这类分布式一致性问题。

一个进程里的 Client 同时承担两个角色：**生产者**（调用 `Insert` 入队）和**消费者**（内部 producer 拉取并执行 Worker）。

## 2. 最小使用示例

一个可运行的完整例子（依赖 `river`、`riverdriver/riverpgxv5`、`pgx` v5）：

```go
package main

import (
    "context"
    "log"
    "os"
    "os/signal"

    "github.com/jackc/pgx/v5/pgxpool"
    "github.com/riverqueue/river"
    "github.com/riverqueue/river/riverdriver/riverpgxv5"
    "github.com/riverqueue/river/rivermigrate"
)

// 任务参数：struct 序列化为 JSON 存进 river_job.args
type SendWelcomeEmailArgs struct {
    UserID int64 `json:"user_id"`
}

// Kind 是任务类型标识，存进 river_job.kind，按它匹配到对应 Worker
func (SendWelcomeEmailArgs) Kind() string { return "send_welcome_email" }

// Worker：只写执行逻辑，中间件/重试时间等有默认实现
type SendWelcomeEmailWorker struct {
    river.WorkerDefaults[SendWelcomeEmailArgs]
}

func (w *SendWelcomeEmailWorker) Work(ctx context.Context, job *river.Job[SendWelcomeEmailArgs]) error {
    log.Printf("sending welcome email to user %d", job.Args.UserID)
    return nil // 返回 error 则按 retry policy 进入 retryable
}

func main() {
    ctx := context.Background()

    dbPool, err := pgxpool.New(ctx, os.Getenv("DATABASE_URL"))
    if err != nil {
        log.Fatal(err)
    }

    // 建表（river_job 等）；生产环境一般并入应用自身的迁移流程
    migrator, err := rivermigrate.New(riverpgxv5.New(dbPool), nil)
    if err != nil {
        log.Fatal(err)
    }
    if _, err := migrator.Migrate(ctx, rivermigrate.DirectionUp, nil); err != nil {
        log.Fatal(err)
    }

    workers := river.NewWorkers()
    river.AddWorker(workers, &SendWelcomeEmailWorker{})

    client, err := river.NewClient(riverpgxv5.New(dbPool), &river.Config{
        Queues: map[string]river.QueueConfig{
            "default": {MaxWorkers: 10},
        },
        Workers: workers,
    })
    if err != nil {
        log.Fatal(err)
    }

    // 启动运行时：producer/elector/维护服务全部在本进程内以 goroutine 运行
    if err := client.Start(ctx); err != nil {
        log.Fatal(err)
    }

    // 入队：普通插入；需要和业务写库同事务时改用 InsertTx(ctx, tx, args, nil)
    if _, err := client.Insert(ctx, SendWelcomeEmailArgs{UserID: 42}, nil); err != nil {
        log.Fatal(err)
    }

    // 优雅停机：等在跑的任务结束；要强制取消在跑任务则用 StopAndCancel
    sigctx, stop := signal.NotifyContext(ctx, os.Interrupt)
    defer stop()
    <-sigctx.Done()
    if err := client.Stop(context.Background()); err != nil {
        log.Fatal(err)
    }
}
```

与后文的对应关系：`Insert` 走 5.1 的入队流；`Work` 由 producer 抢到任务后经 jobexecutor 调用（5.2 的消费流）；选主与维护服务（5.3）由 `Start` 自动带起，业务代码无感。

## 3. 顶层架构

```mermaid
flowchart TB
    subgraph 用户代码
        APP[业务代码]
        W[Worker 实现<br/>每个 JobArgs 一个 Worker]
    end

    subgraph 根包 river["根包 river（公共 API）"]
        CLIENT["Client<br/>统一入口：Insert/Stop/JobCancel/Subscribe..."]
        PRODUCER["producer（每个队列一个）<br/>取任务、派发执行"]
        SUB["subscriptionManager<br/>对外发事件"]
        PERIODIC["PeriodicJobBundle<br/>cron 任务注册"]
    end

    subgraph internal["internal/（核心运行时）"]
        NOTIFIER["notifier<br/>LISTEN/NOTIFY 收发"]
        EXECUTOR["jobexecutor<br/>单任务执行：中间件+钩子+超时"]
        COMPLETER["jobcompleter<br/>批量写回终态"]
        ELECTOR["leadership.Elector<br/>选主"]
        MAINT["maintenance（仅 Leader 运行）<br/>scheduler/rescuer/cleaner/<br/>periodic_enqueuer/reindexer"]
    end

    subgraph driver["riverdriver（驱动抽象）"]
        PGX["riverpgxv5<br/>pgx v5 实现"]
        DBSQL["riverdatabasesql<br/>database/sql 实现"]
        SQLITE["riversqlite<br/>SQLite 实现"]
    end

    PG[("PostgreSQL<br/>river_job / river_queue /<br/>river_leader / river_notification")]

    APP -->|Insert/InsertTx| CLIENT
    CLIENT --> PRODUCER
    PRODUCER --> EXECUTOR
    EXECUTOR --> COMPLETER
    COMPLETER --> SUB -->|Event| APP
    W --- EXECUTOR
    PRODUCER & COMPLETER & CLIENT & MAINT & ELECTOR --> driver
    ELECTOR --> MAINT
    driver --> PG
    PG <-.->|pg_notify| NOTIFIER
    NOTIFIER --> PRODUCER & ELECTOR
    PERIODIC --> MAINT
```

关键分层：**根包**是对外 API，**internal** 是运行时机制，**riverdriver** 把所有 SQL 抽象成 `Executor` 接口（约 40 个操作，见 [river_driver_interface.go](../../codes/river/riverdriver/river_driver_interface.go)），所以同一套核心逻辑可以跑在 pgx、database/sql、SQLite 上。

## 4. 核心数据模型：river_job 状态机

所有机制都围绕 `river_job` 表的一个状态字段（[river_job.sql](../../codes/river/riverdriver/riverpgxv5/internal/dbsqlc/river_job.sql)）：

```mermaid
stateDiagram-v2
    [*] --> available: Insert（默认）
    [*] --> scheduled: ScheduledAt 在未来
    [*] --> pending: InsertOpts.Pending<br/>（提交/显式提升前不可见）
    scheduled --> available: scheduler 到期搬运
    retryable --> available: 到期后重试
    available --> running: producer 抢到<br/>（FOR UPDATE SKIP LOCKED）
    running --> completed: Work 成功
    running --> retryable: 出错且未超 max_attempts<br/>按重试策略定下次时间
    running --> discarded: 超过 max_attempts / 主动丢弃
    running --> cancelled: JobCancel
    completed --> [*]
    discarded --> [*]
    cancelled --> [*]
```

表上的其他字段支撑对应功能：`priority`（1-4）、`queue`、`unique_key + unique_states`（唯一性）、`errors jsonb[]`（每次失败的错误历史）、`metadata`（取消标记等）、`finalized_at`（终态时间，配套 CHECK 约束保证一致性）。

## 5. 数据流

### 5.1 入队（生产者侧）

```mermaid
sequenceDiagram
    participant App as 业务代码
    participant C as Client
    participant DB as Postgres
    participant P as producer(该队列)

    App->>C: InsertTx(tx, args)
    C->>C: args 序列化为 JSON，<br/>合并 InsertOpts（队列/优先级/唯一性/调度）
    C->>DB: JobInsertFull（随业务事务提交）
    C->>DB: pg_notify('river_insert', {queue})
    Note over C: notifylimiter 限流：<br/>同一队列短时间内只发一次，<br/>避免高吞吐下 notify 风暴
    DB--)P: LISTEN 收到通知
    P->>P: TriggerJobFetch（去抖）→ 立即拉取
```

要点：

- **正常路径靠 LISTEN/NOTIFY 唤醒，轮询做兜底**（producer 有 jitter 轮询循环，所以 notify 丢失只是延迟、不丢任务；驱动不支持 LISTEN 时退化为纯轮询模式）
- 事务内插入意味着**提交前其他消费者看不到这条任务**，天然原子

#### 通知为什么由 client 发，而不是触发器

早期版本（迁移 002）用数据库触发器实现唤醒：`river_job` 上的 AFTER INSERT 触发器，新行 state 为 `available` 时 `pg_notify('river_insert', {queue})`。004 迁移删掉了触发器，通知职责移到 client 侧的 Go 代码（`Insert` 完成后、同一事务连接上调 `NotifyMany`）。

触发器方案的两个问题：

- **无法限制频率**：Postgres 只去重同一事务内完全相同的通知，所以每个插入事务至少发一条 notify。高频小事务下通知量与事务数成正比，容易刷成通知风暴
- **每行过一次触发器函数**：批量插入按行数放大执行开销

client 侧的替代实现（client.go 的 `maybeNotifyInsertForQueues`）：

- **内存限流**：进程内的 `notifylimiter`（`map[队列]上次发送时间` + 一把锁），间隔小于 `FetchCooldown` 就不发——producer 本来就不会比冷却期更频繁地 fetch，通知发得更快是浪费
- **原子性保留**：`pg_notify` 在事务内调用时随 commit 才投递、回滚不发，所以在插入的同一事务上发送，语义与触发器等价
- **多队列一次往返**：payload 拼成数组，一条 `unnest` + `pg_notify` 语句批量发出

限流只到实例级、不做跨实例协调，这是刻意的 ROI 取舍：`pg_notify` 是广播、唤醒幂等——任何一条通知叫醒的是**所有**实例的 producer，多实例重复发送只是冗余而非错误；全局通知速率的上界 = 实例数 × 队列数 / 冷却窗口，与插入速率无关（触发器方案的致命处正是通知量 ∝ 插入事务数、没有上界）。为全局去重引入跨实例协调（如 Redis）成本远超收益——何况被限流掉的插入并不丢，最多晚一个轮询周期被兜底捞起。

代价是覆盖面收窄：触发器对**任何写入方**生效（包括绕过 River 直插 `river_job` 的工具），client 侧只覆盖走 API 的插入——第三方直插靠 producer 轮询兜底（最坏约 1s 延迟）。

### 5.2 消费（producer → executor → completer）

```mermaid
flowchart LR
    subgraph producer["producer（每队列 1 个主 goroutine）"]
        F["fetch 循环<br/>notify 唤醒 + 定时轮询"]
        D["dispatchWork<br/>受队列 maxWorkers 限制"]
    end
    subgraph exec["jobexecutor（每任务 1 个 goroutine）"]
        U["反序列化 args<br/>构建 WorkUnit"]
        MW["中间件链<br/>→ Job Hooks<br/>→ Work(ctx)"]
        ST["超时 & 卡死监控<br/>watchStuck"]
    end
    subgraph comp["jobcompleter"]
        B["批量聚合<br/>JobSetStateIfRunning"]
    end

    F -->|JobGetAvailable:<br/>state=available AND scheduled_at<=now<br/>ORDER BY priority,scheduled_at,id<br/>FOR UPDATE SKIP LOCKED<br/>同事务置为 running| D --> MW
    MW -->|jobResultCh| B -->|写回终态| PG[("river_job")]
    B -->|CompleterJobUpdated| SM["subscriptionManager<br/>→ Subscribe 事件流"]
```

三个组件各管一段，职责单一：

- **producer**（[producer.go](../../codes/river/producer.go)）：只管"取多少、派给谁"，通过 `SKIP LOCKED` 保证多消费者实例抢同一批任务不冲突、不阻塞
- **jobexecutor**（[internal/jobexecutor](../../codes/river/internal/jobexecutor/)）：单任务生命周期——中间件、hook、超时控制、出错时按 retry policy 算下次执行时间、卡死检测
- **jobcompleter**（[internal/jobcompleter](../../codes/river/internal/jobcompleter/)）：把执行结果**异步批量**写回 DB（`JobSetStateIfRunning` 只在任务仍是 running 时生效，防止覆盖人工取消等并发操作），并驱动事件流

### 5.3 维护（选主 + Leader 独占的后台服务）

多客户端实例中同时只能有一个 Leader 运行维护任务，避免重复劳动：

```mermaid
flowchart TB
    subgraph elector["Elector（每个 client 内）"]
        A["尝试 INSERT river_leader<br/>（UNLOGGED 表 + TTL，秒级）"]
        A -->|当选| L["Leader：周期续约<br/>（LeaderAttemptReelect）"]
        A -->|冲突| FLW["Follower：监听 river_leadership<br/>+ 定期探测，Leader 失效立刻补位"]
    end
    L --> QML["QueueMaintainerLeader"]
    QML --> M["QueueMaintainer 启动一组服务"]
    subgraph M["维护服务"]
        SCH["job_scheduler<br/>scheduled/retryable 到期 → available"]
        RES["job_rescuer<br/>卡在 running 的僵尸任务 → retryable/discarded"]
        CLN["job_cleaner<br/>删除超过保留期的终态任务"]
        PJE["periodic_job_enqueuer<br/>cron 定时入队（带周期内唯一性）"]
        QC["queue_cleaner / reindexer / ..."]
    end
```

#### 选主机制：单行租约的抢、续、让

`river_leader` 是一张**单行租约表**：INSERT 抢位、UPDATE 续约、过期释放。`name` 列是 PRIMARY KEY 且 CHECK 死等于 `'default'`——全库最多一行，即全局唯一的 Leader 槽位。

当选是一条原子 INSERT（每个实例的 elector 默认每 5s 尝试一次）：

```sql
INSERT INTO river_leader(leader_id, elected_at, expires_at)
VALUES (@leader_id, now(), now() + @ttl)   -- ttl 默认 15s
ON CONFLICT (name) DO NOTHING
RETURNING *;
```

有返回行即当选；冲突被 `DO NOTHING` 吞掉即本轮落选。尝试前先在同一个事务里 `LeaderDeleteExpired` 清掉过期死行，前任的过期租约不挡路。

Leader 持续续约（默认每 5s 一次），SQL 带三重防护：

```sql
UPDATE river_leader SET expires_at = now() + @ttl
WHERE elected_at = @elected_at      -- 只认自己这个任期（fencing）
  AND expires_at >= now()           -- 租约未过期
  AND leader_id = @leader_id;
```

`elected_at` 兼任任期号：租约过期被别人顶替后，新行的任期不同，旧 Leader 的续约必然落空，无法"复活"自己；续约失败或超时，Leader 主动让位并停掉维护服务。TTL 默认 15s = 选主间隔 5s + 10s 余量。

崩溃切换的时间线（默认值）：

```text
t=0    A INSERT 成功当选，expires_at = t+15s
t=5s   A 续约 → t+20s；t=10s 再续 → t+25s
t=12s  A 进程崩溃，续约停止，死行仍在
t=25s  租约过期，B 的 elect 循环 DeleteExpired + INSERT 当选
```

崩溃场景最坏约 20s（TTL + 一个选主间隔），维护任务幂等所以无伤。优雅停机走快路径：`LeaderResign` 在删行的同时 `pg_notify('river_leadership', {action: 'resigned'})`，follower LISTEN 到广播立刻抢位，不等过期。

两个设计取舍：

- **UNLOGGED 表**：领导权本来就是"进程活着才有效"的状态，崩溃即失权，无需持久化，还省 WAL
- **表租约 vs advisory lock**：advisory lock 只表达"持/不持"，绑定连接生命周期，查不到当前 Leader 是谁、何时当选、何时过期；租约行是可见的数据——`expires_at` 是显式 TTL，`elected_at` 提供任期 fencing，`leader_id` 标识持有者，排障时一条 SELECT 看清全局

（UNLOGGED 表与 advisory lock 的机制细节展开在 [pg.md](../../pg.md)）

选主不是为任务分发（那是 `SKIP LOCKED` 干的），只为**去重跑后台维护**：搬到期任务、清理垃圾、跑 cron。

#### 卡死检测：`attempted_at` 为什么够用

rescuer 判定僵尸任务的条件是 `state='running' AND attempted_at < now() - 窗口`，执行期间没有任何心跳刷新。这成立的前提是"卡死"的两种故障已被分层处理：

- **Work 挂起**（死循环、外部调用不返回，进程还活着）：jobexecutor 的 `watchStuck` + JobTimeout（默认 1 分钟）在进程内超时取消 context，轮不到 DB 层
- **进程死亡**（OOM、kill -9，无人写回终态）：合法任务最长跑 JobTimeout，"开始时间超过窗口仍 running"即可断定进程已死。窗口默认 1h，配置更大的 JobTimeout 时自动抬为 `JobTimeout + 1h`（[client.go](../../codes/river/client.go)）

`attempted_at` 在抢任务的同一个 UPDATE 里顺手写入，执行期间零额外写。心跳方案（执行期间周期刷新 `touched_at`，如 BullMQ 锁续期、Temporal activity heartbeat）面向**时长无界**的任务——无法声明 JobTimeout，只能靠"还在跳"证明活着；代价是每个运行中任务的定时写放大，且框架心跳检测不出 Work 级挂起（心跳 goroutine 活着就照跳），JobTimeout 依然省不掉。SQS 的 VisibilityTimeout 与 River 同构，同样不做心跳。**任务时长有界可声明 → `attempted_at`；支持无界长任务 → 心跳**——这是任务时长模型的选择，不是口味差异。

## 6. 模块速览

| 模块 | 位置 | 核心功能 |
|---|---|---|
| Client | [client.go](../../codes/river/client.go) | 对外门面：入队（4 种 Insert 变体）、任务管理（Cancel/Delete/Retry/Get）、生命周期、订阅事件、动态增删队列 |
| producer | [producer.go](../../codes/river/producer.go) | 每队列一个；fetch/dispatch 循环、队列暂停恢复、优雅停机（等在跑任务结束） |
| jobexecutor | [internal/jobexecutor](../../codes/river/internal/jobexecutor/) | 单任务执行：中间件 → hooks → `Work()`，超时/卡死/错误处理/重试时间计算 |
| jobcompleter | [internal/jobcompleter](../../codes/river/internal/jobcompleter/) | 终态批量写回 + 事件分发 |
| notifier | [internal/notifier](../../codes/river/internal/notifier/) | 一条专用连接上的 LISTEN，分发三个 topic：`river_insert` / `river_control`（取消、暂停）/ `river_leadership` |
| leadership | [internal/leadership](../../codes/river/internal/leadership/) | 基于 `river_leader` 表 + TTL 的选主、续约、辞职 |
| maintenance | [internal/maintenance](../../codes/river/internal/maintenance/) | Leader 独占的后台服务组（见上图） |
| riverdriver | `codes/river/riverdriver/` | 驱动抽象 + pgx v5 / database/sql / SQLite 三个实现；SQL 全部以 sqlc 管理 |
| rivertype | `codes/river/rivertype/` | 跨包共享类型：`JobRow`、状态常量、hook/插件接口 |
| rivermigrate / rivertest | 根下子模块 | 迁移管理 / 测试断言工具 |

## 7. 由架构推出的能力清单

对照上面的机制，River 开箱提供：

| 能力 | 依靠的机制 |
|---|---|
| 事务性入队（与业务数据同事务） | `InsertTx` + Postgres 事务可见性 |
| 至少一次执行 + 自动重试 | `retryable` 状态 + retry policy（默认指数退避）+ `max_attempts` 后丢弃 |
| 定时/延迟任务 | `scheduled` 状态 + Leader 的 scheduler 到期搬运 |
| cron 周期任务 | periodic_job_enqueuer + 周期内唯一性 |
| 任务唯一性 | `unique_key` + `unique_states` 位图，DB 层保证 |
| 多实例横向扩展 | `FOR UPDATE SKIP LOCKED` 抢占，无中心协调 |
| 取消运行中/排队任务 | `JobCancel` → `river_control` 通知 → executor 取消 context |
| 队列暂停/恢复、优先级、多队列 | `river_queue` 元数据 + fetch 排序规则 |
| 卡死检测（进程崩溃遗留 running 任务） | Leader 的 rescuer 定期扫描 |
| 事件流（insert/start/fail/retry/complete...） | completer → subscriptionManager → `Subscribe` channel |
| 优雅停机（等任务跑完 vs 强制取消） | `Stop` / `StopAndCancel` 两段式关闭 |
| 自定义中间件、hook、错误处理器、重试策略 | jobexecutor 的扩展点（middleware / hook 接口） |

## 8. 一句话总结

**River = 一张带状态机的 `river_job` 表 + 抢占式消费循环 + LISTEN/NOTIFY 加速唤醒 + 每实例一个的 Leader 维护后台**。所有可靠性保证（不丢、不重跑调度错乱、崩溃恢复）最终都落在 Postgres 的事务和行锁语义上。
