# PostgreSQL 笔记

- 定位：按主题积累的 PG 知识点，整理自学习 River（`github.com/riverqueue/river` v0.47.0）源码时展开的机制，与具体框架解耦
- 写法：每个主题自带术语解释、最小示例、适用边界，后续主题按编号追加

## 1. advisory lock（咨询锁）

**定义**：一把"只认名字、不管用途"的锁。调用方给一个 int64 当 key，Postgres 保证同一时刻只有一个会话能持有它，但它**不锁任何表或行**——锁保护什么由应用自己约定，数据库只提供"抢/放"原语。

**心智模型**：前台的一排寄存柜。柜子不保护任何东西，但所有参与者约定"拿到 42 号柜钥匙的人才是 Leader"，钥匙的归属就能当共识用。"advisory"（咨询性）即由此而来：数据库不强制语义，全靠参与者自觉先检查，所以也叫应用级锁。

### 1.1 API

全部在内存中操作，不落盘：

| 函数 | 粒度 | 行为 |
| --- | --- | --- |
| `pg_advisory_lock(key)` | 会话 | 阻塞等待直到拿到 |
| `pg_try_advisory_lock(key)` | 会话 | 立即返回 true/false，不等待 |
| `pg_advisory_xact_lock(key)` | 事务 | 事务提交/回滚时自动释放 |
| `pg_advisory_unlock(key)` | — | 主动释放会话级锁 |

key 是 int64；另有 `(int4, int4)` 两段式重载，可按"命名空间前缀 + 编号"组织，避免不同用途撞 key。

### 1.2 最小演示（两个 psql 会话）

```sql
-- 会话 A
SELECT pg_try_advisory_lock(42);   -- true，拿到
-- 会话 B
SELECT pg_try_advisory_lock(42);   -- false，立即返回，不阻塞
-- 会话 A 断开（退出或崩溃），锁随连接自动释放
-- 会话 B 重试
SELECT pg_try_advisory_lock(42);   -- true
```

### 1.3 应用示例：多副本部署下只跑一份的后台任务

场景：清理脚本随应用部署了 3 个副本，同一时刻只允许一个真正执行。

```go
const lockKey = 861337 // 全局约定的 int64，注意不要与其他用途撞 key

conn, err := pgx.Connect(ctx, os.Getenv("DATABASE_URL")) // 独占连接，见下方坑
if err != nil {
    log.Fatal(err)
}
defer conn.Close(ctx) // 连接断开锁自动释放，实例崩溃也不会留死锁

var acquired bool
if err := conn.QueryRow(ctx, "SELECT pg_try_advisory_lock($1)", lockKey).Scan(&acquired); err != nil {
    log.Fatal(err)
}
if !acquired {
    log.Print("another instance holds the lock; exiting")
    return
}
defer conn.Exec(ctx, "SELECT pg_advisory_unlock($1)", lockKey)

runCleanup(ctx) // 拿到锁才执行
```

**坑：必须用独占连接，不能用连接池**。会话级锁绑定在物理连接上——用池的话 `QueryRow` 一返回连接就还回池子，但锁跟着连接留在池里，连接活着锁就一直占着，等于泄漏。要么像上面用 `pgx.Connect` 的单连接，要么把拿锁/放锁/干活固定在同一条 acquire 出来的连接上。

事务级锁没有这个问题：`pg_advisory_xact_lock` 随事务结束自动释放，适合"事务内串行化一段逻辑"。现役例子：River 的 periodic_job_enqueuer 用两段式 key 的 `pg_advisory_xact_lock` 串行化 cron 入队，避免多实例同时判断"这个周期入没入过队"的竞态（periodic_job_enqueuer.go 的 `AdvisoryLockPrefix`）。

### 1.4 局限

- 状态二元（持/不持）：`SELECT` 查不到"谁持有、何时拿到、何时该放"，只有 `pg_locks` 视图能看到锁本身
- 没有 TTL：连接活着锁就在；进程假死（既不放锁也不断开）时无解，只能杀连接
- 需要可见状态、任期、过期语义时，一张租约表更合适——River 的选主用的就是表租约（`river_leader`）而非 advisory lock，对比见 [articles/river/1_architecture_overview.md](articles/river/1_architecture_overview.md) 的 5.3 节

## 2. UNLOGGED 表

**WAL 先行**：Postgres 的写路径是先写 WAL（write-ahead log，预写日志）再改数据页。WAL 是两件事的源头——崩溃恢复（重启后重放日志补齐数据）和流复制（standby 靠回放 WAL 保持同步）。

**UNLOGGED 表就是跳过 WAL 的表**：

```sql
CREATE UNLOGGED TABLE river_leader(...);   -- 建表时指定
ALTER TABLE some_table SET UNLOGGED;       -- 已有表转换，反向是 SET LOGGED
```

收益和代价都来自"没有 WAL"：

| 维度 | 效果 |
| --- | --- |
| 写入 | 更快：省掉每笔写对应的 WAL 记录和 fsync |
| 崩溃 | 表被自动 TRUNCATE：没有 WAL 无法恢复，重启后只能清空保结构 |
| 复制 | 不到 standby：流复制的载体就是 WAL，副本上这张表永远为空 |
| 结构 | DDL 持久。区别于 TEMP 表（会话级、断开即整个消失、其他会话不可见） |

**适用判断**：数据可随时重建、或本来就允许丢，才用 UNLOGGED。典型：缓存表、可再生的聚合中间结果、领导权租约。River 把 `river_leader` 建成 UNLOGGED 正是这个逻辑——领导权本来就是"进程活着才有效"的状态，崩溃即失权是期望语义而非缺陷，顺手还降低了写放大。

## 3. 写放大、锁竞争、大扫描：容量问题的三张面孔

三种让数据库变慢的机理，且会互相喂养（见 3.4）。真正压垮 Postgres 的从来不是高频的简单索引读，而是这三样。

### 3.1 写放大：一次逻辑写变成多次物理写

**机制**，三层叠加：

1. **UPDATE 不是原地改**：MVCC 下每次更新写一个新行版本、旧版本标死——逻辑改 1 个字段，物理写一整行
2. **每个索引跟着写**：表上 N 个索引，一次行变更产生 N 次索引结构写。唯一例外是 HOT 更新（改的列不在任何索引里、新版本放得进同一页），所以更新"被索引的列"远贵于普通列
3. **WAL 先行**：每笔写先落 WAL；checkpoint 后首次修改某页还要整页写入（full_page_writes），物理写量量级上再翻倍

放大系数 ≈ `2 × (1 + 索引数)`：6 个索引的表，1000 次 insert/秒 ≈ 每秒 1.4 万次物理写。

**次生灾害是膨胀（bloat）**：死版本要等 vacuum 回收，回收跟不上就堆在表和索引里，占着更多数据页——写放大转成读放大。

**发现**：`pg_stat_user_tables` 中 n_tup_upd 远超 n_tup_ins；WAL 生成速率高；表/索引大小与行数不匹配；autovacuum 频繁运行。

**治**：索引能少则少（每个索引都收"写税"）；避免更新索引列；fillfactor 给 HOT 留空间；用 DDL 代替 DELETE——按时间分区后 `DROP PARTITION` 是 O(1) 元数据操作，逐行 DELETE 是 O(n) 写放大。River 的 river_job（7 个索引 × 任务生命周期多次状态 UPDATE）是典型写放大源，对应 cleaner 分批删 + reindexer 治索引膨胀。

### 3.2 锁竞争：并发一上来吞吐反而崩

**机制**：MVCC 让读不挡写，但**写写同一行必须串行**，且行锁持到事务结束——持锁时长 = 事务时长，不是语句时长。

- **单行热点**（如 `UPDATE counters SET n = n + 1`）：所有请求串行在同一行上。持锁 0.5ms → 吞吐天花板 2000/s，**加核无效**；100 并发时人均排队约 50ms
- **队列型竞争**：100 个 worker 抢"下一条任务"时，`FOR UPDATE` 让 99 个等锁；`FOR UPDATE SKIP LOCKED` 改成"抢不到就跳过去拿别的"——等待变分流，这是数据库队列不用排队锁的根本原因
- **DDL 锁最凶**：`ALTER TABLE` 拿 ACCESS EXCLUSIVE，**连读都挡**；非 CONCURRENTLY 的 `CREATE INDEX` 挡写。大表上一条 DDL 能停摆全表

**发现**：并发升高吞吐不升反降（弹性悬崖）；`pg_stat_activity.wait_event` 出现 `Lock:transactionid`；单线程很快、多线程就慢。

**治**：SKIP LOCKED；缩短事务（不在事务里调外部 API、不把大批写塞进长事务）；热点行分桶；`lock_timeout` + 重试；DDL 走 CONCURRENTLY 或低峰窗口。

### 3.3 大扫描：只读也在伤害全库

**机制**，三条伤害路径：

- **缓存污染**：扫描读入的新页把别人的"热页"挤出缓存（淘汰按使用计数：每页 0-5，被访问 +1、清扫经过 -1，归零者被选中淘汰），OLTP 查询集体下沉到磁盘
- **资源挤占**：顺序扫一张 10GB 表，磁盘带宽与并行 worker 被它独占
- **卡住 vacuum**：长查询开着快照，比它更新的死版本不能回收 → 膨胀 → 下次扫描更大（与 3.1 咬合）

**缓存污染的确切边界**（精度注记）：`shared_buffers` 是固定大小的内存页池（默认 128MB = 16384 个 8KB 页），所有连接共享，命中与未命中差 100-1000 倍延迟（内存 µs vs 磁盘 100µs-ms）。Postgres 是双层缓存——磁盘 → 内核页缓存 → `shared_buffers`。对 `shared_buffers` 层，现代 PG 有防御：表大于缓冲池 1/4（默认配置约 32MB）时，顺序扫描启用 **256KB 环形缓冲区**（32 个槽循环使用，扫过的页很快被自己的下一页覆盖，只占池子一小块）；VACUUM 同为 256KB 环，COPY FROM / CREATE TABLE AS 用 16MB 环——"全表扫挤爆 shared_buffers"在纯顺序扫描下基本被封住。仍受污染的两处：**内核页缓存**（无此保护，10GB 顺序读照样显著置换其他文件的页）；**随机访问模式**（索引驱动的回表不受环形缓冲区保护）。

**数量级**：1 亿行 × 100B ≈ 10GB ≈ 125 万页（8KB）。索引点查约读 4 页（B 树深度），约 0.05ms；无索引全扫冷盘 500MB/s，约 20s——相差四五个数量级。深层 `OFFSET 100000` 同样是扫过并丢弃前 10 万行，分页要用 keyset（`WHERE id > :last`）。

**发现**：`pg_stat_statements` 中 shared_blks_read 巨大、读的行数远多于返回行数；`EXPLAIN ANALYZE` 见 Seq Scan；IO wait 高；表年龄（age）持续增长说明 vacuum 追不上。

**治**：B 树 / 部分索引 / 覆盖索引（INCLUDE 争取 index-only scan）；keyset 分页；预聚合；分区裁剪；分析查询挪只读副本；`statement_timeout` 兜底。

**反向陷阱**：Seq Scan 不总是坏——查询命中表的很大比例时它快于"索引 + 随机回表"，判断标准是"读了多少比例"，不是"有没有走索引"。

### 3.4 三者互相喂养

长事务/长扫描 → 挡 vacuum → 膨胀 → 扫描更大、写放大更重；索引多 → 写放大更大 → 膨胀更快。容量事故的典型剧本是螺旋恶化而不是单点爆炸。排查时先定位哪一环在转：**写放大看 WAL 与 bloat，锁看 wait_event，扫描看 pg_stat_statements**。

## 4. 常用进阶 SQL 与性能常识（基础 CRUD 之外）

针对"只会 create/select/update/insert/delete"的起点，把前几节引用过的语法在这里展开，并补上高频常识。原则同前：机制、最小示例、边界。

### 4.1 行锁查询：FOR UPDATE / SKIP LOCKED / NOWAIT

普通 `SELECT` 走 MVCC 快照，不加锁。`FOR UPDATE` 给选出的行加行锁——**读的时候顺手锁住，防止别人并发修改**，锁持到事务结束：

```sql
BEGIN;
SELECT * FROM accounts WHERE id = 1 FOR UPDATE;  -- 别人 UPDATE 这行会被阻塞
UPDATE accounts SET balance = balance - 100 WHERE id = 1;
COMMIT;  -- 锁随提交释放
```

用途是"读-改-写"模式（先读余额、再算、再扣）。**必须在事务里用**：自动提交模式下语句一结束锁就释放，等于没锁。

`SKIP LOCKED`：被别的会话锁住的行**直接跳过，不排队**：

```sql
SELECT * FROM river_job
WHERE state = 'available'
ORDER BY id LIMIT 10
FOR UPDATE SKIP LOCKED;   -- 10 个 worker 并发取任务，各拿各的，互不等待
```

这是"数据库当队列"的核心原语，也是 3.2 里"等待变分流"的实现。对比：普通 `FOR UPDATE` 会让 9 个 worker 排队等第 1 个。

**认领的完整写法**：`SKIP LOCKED` 不需要额外判断"有没有拿到锁"——返回集就是拿到锁的行，没抢到的根本不进结果集。生产写法把它和后续 UPDATE 合成一条语句（River 的 `JobGetAvailable` 即此形状）：

```sql
WITH locked_jobs AS (
    SELECT * FROM river_job
    WHERE state = 'available' AND queue = @queue
    ORDER BY priority, scheduled_at, id
    LIMIT @max_to_lock
    FOR UPDATE SKIP LOCKED
)
UPDATE river_job
SET state = 'running', attempt = attempt + 1
FROM locked_jobs                       -- 只 join 已加锁的行
WHERE river_job.id = locked_jobs.id
RETURNING river_job.*;                 -- 返回的就是本次认领成功的任务
```

两个机制支撑它成立：锁是**事务级**资源（挂到 COMMIT/ROLLBACK，同事务内再更新自己锁的行不阻塞、不冲突）；单语句形式自带隐式事务，autocommit 下也安全。若拆成两条语句写（SELECT 取清单、UPDATE 按清单更新），**必须显式包在 BEGIN/COMMIT 里**——否则 SELECT 一结束锁就释放，清单可能被别的会话抢走。

边界：若要在更新时校验**状态值本身**（比如只在任务仍是 running 时才写回完成态，防止覆盖并发取消），那是条件更新 `UPDATE ... WHERE id = ? AND state = 'running'`（CAS），靠 WHERE 条件而非锁保证——两种模式解决不同问题。

`NOWAIT`：不等，拿不到直接报错——适合"立即让调用方知道失败"的场景。

选型一句话：**要串行 → `FOR UPDATE`；要分流 → `SKIP LOCKED`；失败要立刻可见 → `NOWAIT`**。

### 4.2 索引：CONCURRENTLY 建、特殊形态、代价

**生产建索引必须加 CONCURRENTLY**：

```sql
CREATE INDEX CONCURRENTLY idx_users_email ON users (email);
```

普通 `CREATE INDEX` 会让该表**写入阻塞**（不挡读），大表上可能停写数分钟；带 `CONCURRENTLY` 不阻塞写入，代价是更慢（扫两遍）且**不能在事务块里执行**；执行失败会留下失效索引，检查 `pg_index.indisvalid` 并删掉重建。

三种"用更精细的索引换更快查询"的形态：

```sql
-- 部分索引：只索引关心的行，索引更小、更新成本更低
CREATE INDEX ON orders (created_at) WHERE state = 'pending';

-- 覆盖索引：把查询要读的列装进索引，命中后不必回表（index-only scan）
CREATE INDEX ON orders (user_id) INCLUDE (total, created_at);

-- 表达式索引：按函数结果查时
CREATE INDEX ON users (lower(email));
```

**代价**：每个索引都给写收税（见 3.1），且占空间。"建了一堆没人用的索引"是最常见反模式——`pg_stat_user_indexes.idx_scan = 0` 的就是从未被用过的。

### 4.3 分区：删旧数据从 O(n) 变 O(1)

分区 = 逻辑一张表，物理多张子表（PG 10+ 声明式）：

```sql
CREATE TABLE events (
    id bigserial,
    created_at timestamptz NOT NULL,
    payload jsonb
) PARTITION BY RANGE (created_at);

CREATE TABLE events_2026_09 PARTITION OF events
    FOR VALUES FROM ('2026-09-01') TO ('2026-10-01');
```

两个收益：

- **分区裁剪**：`WHERE created_at >= '2026-09-01'` 只扫对应子表，其余跳过——规划器自动做
- **DROP PARTITION**：清理旧数据 = `DROP TABLE events_2026_06;`，O(1) 元数据操作；逐行 `DELETE` 是 O(n) 写放大，删完还要等 vacuum 回收

**坑**：主键/唯一约束必须包含分区键；分区数控制在几百以内，过多会拖慢规划。

### 4.4 写操作的常用武器

**Upsert**（有则更新、无则插入，一条语句原子完成）：

```sql
INSERT INTO users (id, name) VALUES (1, 'a')
ON CONFLICT (id) DO UPDATE SET name = EXCLUDED.name;   -- DO NOTHING 则静默跳过
```

**RETURNING**（写操作直接返回结果行，省一次查询；INSERT/UPDATE/DELETE 都支持）：

```sql
INSERT INTO orders (user_id) VALUES (1) RETURNING id;
DELETE FROM orders WHERE id = 1 RETURNING *;
```

**COPY**（批量导入比逐条 INSERT 快一个数量级；服务端路径需权限，psql 客户端侧用 `\copy`）：

```sql
COPY users FROM '/tmp/users.csv' WITH (FORMAT csv);
```

**多行插入**：`VALUES (...), (...), (...)` 一条语句插多行，比循环单条快得多。

**分批删除**：SQL 没有 `DELETE ... LIMIT`；大表删数据要分批循环，避免长事务锁住大片行、并给 vacuum 留回收窗口：

```sql
DELETE FROM river_job WHERE id IN (
    SELECT id FROM river_job
    WHERE finalized_at < now() - interval '7 days'
    LIMIT 1000
);
```

### 4.5 EXPLAIN：看一条 SQL 实际怎么执行

```sql
EXPLAIN SELECT ...;                     -- 只给计划，不执行
EXPLAIN (ANALYZE, BUFFERS) SELECT ...;  -- 真执行，附实际耗时与缓存命中
```

看计划抓四个点：

- **访问方式**：Seq Scan（全表扫）/ Index Scan（走索引）/ Bitmap Heap Scan（批量回表）；大表查询期望后者
- **连接方式**：Nested Loop（小数据量）/ Hash Join（大表对拼）
- **估算 vs 实际**：`rows=10 (actual rows=100000)` 这种量级偏差 = 统计信息过期，先 `ANALYZE 表名` 再看
- `ANALYZE` 会**真的执行**（含写操作），测 UPDATE/DELETE 时包在事务里看完回滚

### 4.6 超时与观测：出问题先看这几个

**三个必设超时**（防一条坏查询拖垮全库；可会话级或 `ALTER ROLE app SET ...` 配到角色）：

```sql
SET statement_timeout = '5s';                     -- 单条语句执行上限
SET lock_timeout = '2s';                          -- 等锁上限：拿不到就报错重试，而不是无限排队
SET idle_in_transaction_session_timeout = '60s';  -- 踢掉"开了事务却不干活"的连接
```

**常用维护命令**：

```sql
VACUUM (ANALYZE) river_job;            -- 回收死版本 + 刷新统计信息（大表分批、低峰执行）
REINDEX INDEX CONCURRENTLY idx_name;   -- 重建膨胀的索引
```

**观测视图**（对照 3.4 的排查入口）：

| 视图 | 回答什么问题 |
| --- | --- |
| `pg_stat_activity` | 现在谁在跑什么、卡在哪（wait_event） |
| `pg_locks` | 谁持有 / 等待哪把锁 |
| `pg_stat_user_tables` / `pg_stat_user_indexes` | 表增删改多少、vacuum 跟没跟上、索引有没有被用过 |
| `pg_stat_statements` | 哪些 SQL 最耗时 / 最读盘（需先 `CREATE EXTENSION`） |

## 5. 事务与隔离级别

**事务**：`BEGIN ... COMMIT/ROLLBACK` 把多条语句绑成原子单元；没写 BEGIN 时每条语句自带隐式事务（autocommit）。未提交的修改对别人不可见，回滚不留半成品。

**隔离级别**（PG 就三种，READ UNCOMMITTED 被当作 READ COMMITTED）：

| 级别 | 快照时机 | 挡住的 | 代价 |
| --- | --- | --- | --- |
| READ COMMITTED（默认） | **每条语句**取一次 | 脏读 | 同一事务两次查可能不同；先读后写有丢失更新风险 |
| REPEATABLE READ | 事务第一次查询时固定 | 脏读、不可重复读、幻读 | 并发写冲突时报序列化失败（SQLSTATE 40001） |
| SERIALIZABLE | 同上 + SSI 依赖检测 | 上者 + 写偏斜等 | 更容易 40001，必须配重试逻辑 |

两个直击要害的点：

- **默认级别下"先读再写"是竞态**：`SELECT balance` → 应用里算 → `UPDATE`，中间别人可能已改。安全写法：`FOR UPDATE` 锁行（4.1）、条件更新 `UPDATE ... WHERE balance >= 100`（单语句内原子判断）、或乐观锁版本号。注意单语句 UPDATE 本身是安全的——等锁后 PG 会在新版本上**重新评估 WHERE**
- **40001 不是 bug 是机制**：REPEATABLE READ / SERIALIZABLE 的冲突让事务直接失败，正确姿势是**捕获 40001、整个事务重试**

**事务要短**：事务开着就持有快照和锁——长事务同时喂大锁竞争（3.2）和 vacuum 阻塞（3.3）。不在事务里调外部 API，不把大批写塞进一个事务。

## 6. MVCC 可见性

**机制**：每行存在多个物理版本。UPDATE = 写新版本 + 旧版本标死；DELETE = 标死。每个事务持一个快照；一个版本可见 = "由快照之前已提交的事务创建、且未被快照之前已提交的事务删除"。

**收益**（读写的核心卖点）：**读不阻塞写、写不阻塞读**——普通 SELECT 永远不等锁，读的是一致性快照；只有写写同一行才排队。

**代价**：

- 死版本要等 vacuum 回收，回收跟不上就是膨胀、读放大（3.1 / 3.3）
- **任何老快照都钉住 vacuum 水位**：只要还有快照"看得到"某个死版本，它就不能删。长查询、长事务、`idle in transaction` 的连接全都算

**观测**（找最老的事务）：

```sql
SELECT pid, state, xact_start, now() - xact_start AS age, query
FROM pg_stat_activity ORDER BY xact_start LIMIT 10;
```

`xact_start` 很老的行就是风险源；用 `idle_in_transaction_session_timeout`（4.6）自动清理。

**实用推论**：`xmin` / `xmax` 是表上的隐藏版本字段（调试用）；`ctid` 是物理位置、会随 UPDATE 变化，别当稳定 id 用；"自己改的事务内立即可见、别人提交前不可见"是这套规则的直接结果。

## 7. 连接池与 PgBouncer

**PG 是进程模型**：一个连接 = 一个后端进程，几 MB 内存起步 + `work_mem` 等按操作分配。连接是**贵**资源。`max_connections` 默认 100。

**第一层：应用侧池**（pgxpool / HikariCP / database/sql）。要点：

- 池不是越大越好：池大 → PG 上并发进程多 → 锁竞争和上下文切换反而降吞吐。经验起点：**每实例连接数 ≈ 核数的几倍**，压测调优
- 真正的问题是**乘法**：总连接 = 实例数 × 每池大小。50 个 Pod × 10 = 500 远超默认上限 → `too many clients`

**第二层：PgBouncer**。应用与 PG 之间的代理，把大量应用连接复用成少量 PG 连接（500 → 8），连接总数与实例数解耦。

| 池模式 | 连接归还时机 | 兼容性 |
| --- | --- | --- |
| session | 客户端断开 | 完全透明，复用率低，实例多的场景基本没用 |
| transaction（常用） | 每个事务结束 | 复用高；**会话级功能全部不可用** |
| statement | 每条语句结束 | 复用最高，罕见使用 |

transaction 模式的禁区——凡是"跨语句记住会话状态"的功能都不行：`LISTEN/NOTIFY`、**会话级** advisory lock（事务级 `pg_advisory_xact_lock` 可以）、`SET` 会话参数、临时表、会话级 prepared statement（PgBouncer 1.21+ 可协议级支持，需开启配置）。用到这些时：这部分连接走 session 模式或绕开代理直连。

**判断标准**：实例少 → 应用侧池直连，别加组件；实例多/弹性伸缩、总连接逼近 `max_connections` → 上 PgBouncer（代价是多一跳、多一个组件，部署时应靠近应用）。云上有托管替代（RDS Proxy、Supavisor 等），原理相同。

## 8. 表：类型、形态与常用配置

**先纠正概念**：MySQL 的"存储引擎"（InnoDB / MyISAM）在 PG 里没有对等物——PG 只有一种内置表存储实现（heap：行式、MVCC），方言是 `CREATE TABLE ... USING heap`，扩展可提供其它实现（如列存 columnar）。真正要选的是三个维度：**持久性变体**（下节）、**表形态**（8.2）、**存储参数**（8.3）。索引倒是可插拔的：默认 btree，按数据形态另有 gin（jsonb/数组包含）、gist、brin（超大顺序数据）等。

### 8.1 持久性变体

| 类型 | 写法 | 崩溃后 | 复制到 standby | 适用 |
| --- | --- | --- | --- | --- |
| 永久表 | `CREATE TABLE`（默认） | 完好 | 会 | 默认选择 |
| UNLOGGED | `CREATE UNLOGGED TABLE` | 自动清空 | 不会 | 可重建的数据（机制见 2） |
| TEMP | `CREATE TEMP TABLE` | 会话结束即消失 | 不会 | 会话内中间结果；默认 `ON COMMIT PRESERVE ROWS`（可改 `ON COMMIT DROP`）；数据页走独立的小缓冲池（`temp_buffers`），不挤占 `shared_buffers` |

### 8.2 表形态（各一句话）

- **分区表**：逻辑一张、物理多张——裁剪查询 + `DROP PARTITION` 清理（4.3）
- **外部表（FDW）**：`CREATE FOREIGN TABLE`，数据在外部系统（别的 PG、文件、对象存储），PG 只是个查询入口
- **物化视图**：`CREATE MATERIALIZED VIEW`，结果落盘、`REFRESH` 更新——用存储与新鲜度换重复计算的性能
- **TOAST**：不是可选形态，是自动机制——大字段超出页容量后自动压缩/移到附属表，对用户透明；大字段的读写放大多半是它在起作用

### 8.3 常用存储参数（建表 `WITH (...)` 或 `ALTER TABLE t SET (...)`）

| 参数 | 默认 | 何时调 |
| --- | --- | --- |
| `fillfactor` | 100 | 更新频繁的表降到 70-90，给 HOT 留页内空间（少写索引，机制见 3.1） |
| `autovacuum_vacuum_scale_factor` | 0.2 | 大表调小，或改用 `autovacuum_vacuum_threshold` 固定行数——触发条件是"阈值 + 比例 × 行数"，1 亿行的表要改 2000 万行才触发真空，太晚 |
| `autovacuum_analyze_scale_factor` | 0.1 | 同上；统计过期会让规划器选错计划（4.5） |
| `parallel_workers` | 0（自动） | 大表的分析类扫描想显式指定并行度 |

```sql
-- 高频更新表：留 HOT 空间 + 让 vacuum 更早触发
CREATE TABLE counters (...) WITH (fillfactor = 70, autovacuum_vacuum_scale_factor = 0.02);
```

### 8.4 其它常用表级设置

```sql
ALTER TABLE t ALTER COLUMN payload SET STATISTICS 1000;  -- 抬高该列统计精度（默认 100），规划器估算更准
ALTER TABLE t ALTER COLUMN blob SET STORAGE EXTERNAL;    -- 关压缩、强制行外存储，控 TOAST 行为
ALTER TABLE t SET TABLESPACE fast_disk;                  -- 换物理位置（罕见）
```

索引也有 `fillfactor`（默认 90）。

## 9. 常用索引类型

建索引的姿势（CONCURRENTLY、部分/覆盖/表达式）在 4.2；这节是**类型选型**。

| 类型 | 结构 | 擅长的查询 | 典型场景 |
| --- | --- | --- | --- |
| btree（默认） | 平衡树 | `=` `<` `>` `BETWEEN` `ORDER BY`、前缀 `LIKE 'abc%'`（非 C locale 需 `text_pattern_ops`） | 绝大多数查询 |
| hash | 哈希 | 只支持 `=` | 超长等值键（uuid、长串），比 btree 小 |
| gin | 倒排 | 包含类：`jsonb @>`、数组 `@>`、全文检索、`pg_trgm` 模糊匹配 | 标签/属性搜索 |
| gist | 通用搜索树 | 几何、范围类型、最近邻、`EXCLUDE` 排除约束 | 地理位置、时段不重叠约束 |
| spgist | 空间分区树 | 特殊分布文本/点 | 小众 |
| brin | 块级摘要 | 超大表且物理有序（时间列） | 十亿行日志表，索引只有几 MB |

要点：

- 拿不准就用 btree
- **多列索引看列序**：`(a, b)` 服务 `a` 和 `a AND b`，不服务单查 `b`（最左前缀）；等值列在前、范围列在后
- brin 的代价：只定位"可疑块"，要回表复核，适合"数据天然按时间追加、查询也按时间"的表
- 验证：`EXPLAIN` 看是否真用了（4.5）；`pg_stat_user_indexes.idx_scan = 0` 的索引考虑删除（4.2）

## 10. 分区

入门在 4.3，这节补全运营细节。

**三种策略**：

```sql
PARTITION BY RANGE (created_at)   -- 范围（时间/ID），最常用
PARTITION BY LIST (region)        -- 枚举（地区/租户组）
PARTITION BY HASH (user_id)       -- 均匀打散，无自然范围时
```

**运营要点**：

- **DEFAULT 分区**接住没匹配的值防插入报错——但容易忘记建新分区导致数据全落 default，且日后补建分区会因 default 已有冲突行而失败
- **DETACH** 比 DROP 可控：`ALTER TABLE ... DETACH PARTITION ... CONCURRENTLY`（PG 14+，不能在事务里）先摘出、再决定归档还是删除
- **索引**：在分区表上建索引 = 每个分区各建一份；也可以只给热分区单独建
- **自动建分区**：PG 无内置定时器——用 pg_partman 或 cron + `CREATE TABLE`
- **验证裁剪**：`EXPLAIN` 输出应显示只扫少数分区（`Subplans Removed: N`）

**何时不分区**：表不到几千万行、也没有"按时间归档/清理"需求时收益很小；分区数控制在百级以内，每个查询规划都要过一遍所有分区，太多会拖慢规划。唯一约束必须包含分区键（4.3）。

## 11. 主从与只读副本

**机制**：主库持续把 WAL 流给备库，备库重放——物理复制、整实例级别，备库是主库的完整拷贝（只读，可开 `hot_standby` 对外提供读）。

**同步 vs 异步**：

- 异步（默认）：主库提交不等备库——主库宕机可能丢最后一段 WAL
- 同步（`synchronous_standby_names`）：提交等备库确认，零丢失——代价是写入延迟绑上备库，备库慢 = 主库卡
- 折中：`ANY 1 (a, b, c)`，任一备库确认即可

**应用侧要点**：

- **读副本扩展读，不扩展写**——写全部仍压主库
- **复制延迟 = 可能读到旧数据**：典型是"读己之写"（写完订单去列表页看不到）。对策：关键读走主库、会话粘性、或读前等待 LSN 追平
- **观测**：主库 `pg_stat_replication`（看 `replay_lag` 等）；备库 `pg_last_wal_replay_lsn()`
- **副本上的慢查询会反向拖累主库**：开 `hot_standby_feedback` 时，副本长查询会让主库保留死版本（挡 vacuum，见 6）——把分析查询挪到副本不等于零成本
- 备库可做整实例备份、可设**延迟备库**（`recovery_min_apply_delay`）留出误操作的后悔窗口
- 故障切换不是 PG 自带技能：手动脚本或 Patroni / pg_auto_failover 等（云托管自带）；要处理连接串切换与脑裂（fencing）

**逻辑复制**（另一条路）：按表复制、可跨大版本、可部分表同步——适合"只同步几张表"或迁移，不等于高可用方案。

## 12. VACUUM 深入与 autovacuum 调优

**VACUUM 做什么**：扫描表和索引，把死版本占用的空间标记为"可复用"。注意语义——**空间不还给操作系统**，是留给后续写入复用（阻止膨胀增长）。`VACUUM FULL` 才把空间还回 OS，但拿 ACCESS EXCLUSIVE 锁（停读停写）+ 需要等量磁盘空间，生产禁用；在线重写用 pg_repack。索引的膨胀单独治：`REINDEX CONCURRENTLY`（4.6）。

**autovacuum 的触发条件**（每张表）：死元组数 > `阈值 + 比例 × 行数`。

| 参数（可全局、也可按表覆盖） | 默认 | 调法 |
| --- | --- | --- |
| `autovacuum_vacuum_scale_factor` | 0.2 | 大表降到 0.05 或按表设固定阈值——否则 1 亿行的表要改动 2000 万行才开始 vacuum |
| `autovacuum_vacuum_cost_limit` | 200 | SSD 上调大（如 1000-2000）：默认节流面向机械盘，快盘上 vacuum 追不上死版本产生速度 |
| `autovacuum_max_workers` | 3 | 脏表多可加；受 cost_limit 总量分配约束 |
| `autovacuum_freeze_max_age` | 2 亿 | 防事务 ID 回卷的强制 vacuum 线；监控 `age(datfrozenxid)` |

**必须监控的两件事**：

```sql
-- 1) 有没有表长期没被 vacuum（n_dead_tup 只涨不降）
SELECT relname, n_live_tup, n_dead_tup, last_autovacuum, last_autoanalyze
FROM pg_stat_user_tables ORDER BY n_dead_tup DESC LIMIT 20;

-- 2) 回卷安全线：接近 2 亿要关注；接近 20 亿数据库会拒绝写入、强制停机做紧急 vacuum
SELECT datname, age(datfrozenxid) FROM pg_database ORDER BY 2 DESC;
```

**关键认知**：vacuum 追不上通常不是它慢，而是**被挡住**——长事务/老快照（6）、复制槽、`hot_standby_feedback`（11）。只要有东西钉住回收水位，vacuum 就白跑。

## 13. 查询规划器

流程：SQL → 解析 → 用**统计信息**估算每种执行计划的成本 → 选最便宜的。两个推论：统计信息不准 = 计划选错；成本常数不符硬件 = 计划选错。

**统计信息**：`ANALYZE` 采集行数、值分布直方图、物理相关性；autovacuum 自动保底（触发条件同上一节的 analyze 版：50 + 0.1 × 行数）。列级精度：`ALTER TABLE t ALTER COLUMN c SET STATISTICS 1000`（8.4）；两列相关用扩展统计：

```sql
CREATE STATISTICS s (dependencies) ON city, postal_code FROM addresses;  -- 单列统计假设两列独立，会估错
```

**常调的成本参数**（决定"索引 vs 全表扫"的取舍）：

| 参数 | 默认 | 何时调 |
| --- | --- | --- |
| `random_page_cost` | 4.0 | **SSD 必调**到 1.1-2.0：默认假设随机读比顺序读贵 4 倍（机械盘时代），SSD 上会系统性低估索引 |
| `seq_page_cost` | 1.0 | 成本基准，通常不动 |
| `effective_cache_size` | 4GB | 告诉规划器"PG + OS 缓存大约可用多少"（是估算、不是分配），设成总内存的 50-75% |
| `work_mem` | 4MB | 排序/哈希**每个操作**的内存上限，不足则落盘（EXPLAIN 里看 temp files）；全局调大要谨慎——每连接每操作都独立占用 |
| `default_statistics_target` | 100 | 统计精度默认值，个别列用 SET STATISTICS 覆盖 |

**计划出错的常见原因**（按发生率）：统计过期（先 `ANALYZE`，信号是 EXPLAIN 的估算 vs 实际偏差）→ 参数化查询的通用计划（同一 SQL 不同参数最优计划不同；PG 前几次用定制计划、之后可能切通用计划）→ 列间相关未建模 → 成本常数不符硬件。

PG 没有官方 hint；要强制干预用 pg_hint_plan 扩展或改写 SQL。记住规划器选的是"估算成本最低"，不是"你认为最快"——Seq Scan 有时就是对的（4.5）。

## 14. 观测：慢查询与索引命中率

**A. 累计最耗资源的 SQL —— pg_stat_statements**（首选工具；需 `shared_preload_libraries` 预加载 + 重启，再 `CREATE EXTENSION pg_stat_statements;`）：

```sql
-- 总耗时榜：谁吃掉最多资源，优化优先级看它
SELECT calls, total_exec_time, mean_exec_time, rows, query
FROM pg_stat_statements ORDER BY total_exec_time DESC LIMIT 20;

-- 单次最慢：ORDER BY mean_exec_time DESC
-- 读盘最狠：ORDER BY shared_blks_read DESC

SELECT calls, shared_blks_read, shared_blks_hit, query
FROM pg_stat_statements ORDER BY shared_blks_read DESC LIMIT 20;
```

三条查询看三列就够：`calls`（频率）、`mean_exec_time`（单次成本）、`shared_blks_read`（真读盘量）。SQL 按指纹归一化（字面量变 `$1`），`SELECT pg_stat_statements_reset();` 清零。

**B. 此刻在跑的 —— pg_stat_activity**：

```sql
SELECT pid, now() - query_start AS duration, state, wait_event_type, wait_event, query
FROM pg_stat_activity
WHERE state <> 'idle' AND now() - query_start > interval '1s'
ORDER BY duration DESC;
```

`wait_event_type = Lock` → 锁竞争（3.2）；`IO` → 卡在读写盘。

**C. 索引有没有被用 —— pg_stat_user_indexes**：

```sql
SELECT relname, indexrelname, idx_scan,
       pg_size_pretty(pg_relation_size(indexrelid)) AS size
FROM pg_stat_user_indexes ORDER BY idx_scan ASC LIMIT 20;
```

`idx_scan = 0` = 从未被扫描：删除候选——**例外**：唯一索引/主键索引即使没人查询也在保证约束，不能删。

**D. 表级缓存命中率 —— pg_statio_user_tables**：

```sql
SELECT relname, heap_blks_read, heap_blks_hit,
       round(100.0 * heap_blks_hit / nullif(heap_blks_hit + heap_blks_read, 0), 2) AS hit_pct
FROM pg_statio_user_tables ORDER BY heap_blks_read DESC LIMIT 20;
```

命中率低的表 = 工作集超出缓存，或正被大扫描冲刷（3.3）。两点注意：它只统计 `shared_buffers`（不含 OS 页缓存）；命中率是**线索不是目标**——最终看延迟，别为凑 100% 做优化。索引版在 `pg_statio_user_indexes`。

## TODO

- [ ] 后续主题按编号追加（候选：锁机制全景与死锁排查、WAL 与 checkpoint、备份与 PITR）
