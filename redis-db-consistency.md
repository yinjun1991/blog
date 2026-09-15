# Redis 与 DB 的数据一致性：操作模式、失效兜底与常见坑

- 定位：回答「Redis 和 DB 的一致性怎么维护」：四种操作（查询、创建、更新、删除）各用什么顺序，缓存失效失败如何兜底，竞态窗口如何收窄，常见坑如何防。
- 前提：Redis 中的数据是派生数据，DB 是事实源。两个系统无法原子提交，因此不存在无条件强一致的方案；所有模式都在回答两个问题——不一致窗口多大、窗口内靠什么兜底。
- 关联：学习地图 [7.1 Cache-Aside](./backend-classic-models-learning-map.md) 已覆盖读路径、singleflight、负缓存与 TTL；[5.4 Transactional Outbox](./backend-classic-models-learning-map.md) 是失效重试的载体；实践练习为学习地图项目五。写路径、失效可靠性与竞态处理是本文重点。

## 1. 总览

```text
读路径：GET cache ─ 命中 ───────────────────────────→ 返回
                └ 未命中 → SELECT DB → SET cache（TTL + 抖动）→ 返回

写路径：BEGIN → UPDATE DB ─┬→ COMMIT ─→ DEL cache ─ 失败 → 进入重试
                          │
                          └→ 同事务写 cache_invalidations（outbox）
```

两条原则：读走缓存懒加载；写后让缓存失效（删除），不直接更新缓存值。

## 2. 四种操作的模式

### 2.1 查询：Cache-Aside

```go
func (s *UserService) GetUser(ctx context.Context, id int64) (*User, error) {
    if u, ok := s.cache.Get(ctx, key(id)); ok {
        return u, nil
    }
    u, err := s.db.GetUser(ctx, id)
    if errors.Is(err, sql.ErrNoRows) {
        // 负缓存：把“不存在”也缓存，防穿透
        s.cache.Set(ctx, key(id), nil, 30*time.Second)
        return nil, ErrNotFound
    }
    if err != nil {
        return nil, err
    }
    // TTL 加抖动：避免大量 key 同时过期
    s.cache.Set(ctx, key(id), u, jitter(10*time.Minute))
    return u, nil
}
```

### 2.2 创建：只写 DB，清理负缓存

新建数据不主动写缓存，第一次查询时懒加载回填。理由：写了可能没人读，浪费内存；新增数据也不存在覆盖旧值的竞态。

注意：如果创建之前有人查过该 id，缓存里会留下负缓存（空值占位），创建成功后必须删除它，否则短期内读不到新数据。

```go
func (s *UserService) CreateUser(ctx context.Context, in CreateUserInput) (*User, error) {
    u, err := s.db.InsertUser(ctx, in)
    if err != nil {
        return nil, err
    }
    // 该 id 可能留有“不存在”的负缓存；不清理则新用户短期内查不到
    if err := s.cache.Delete(ctx, key(u.ID)); err != nil {
        log.Warn("cache_delete_failed", "key", key(u.ID), "err", err) // 进重试，见 3.2
    }
    return u, nil
}
```

### 2.3 更新：先提交 DB，再删缓存

```go
func (s *UserService) UpdateUser(ctx context.Context, id int64, patch Patch) error {
    if err := s.db.UpdateUser(ctx, id, patch); err != nil {
        return err
    }
    if err := s.cache.Delete(ctx, key(id)); err != nil {
        // 业务已成功，删除失败不能返回错误给用户；记录后进重试（见 3.2）
        log.Warn("cache_delete_failed", "key", key(id), "err", err)
    }
    return nil
}
```

**为什么删除而不是更新缓存值**：删除是幂等的、与顺序无关——两个并发写无论谁先到达，结果都是“缓存为空、下次读重建”；直接更新缓存则可能因到达顺序颠倒留下旧值，且缓存值是聚合计算结果时还需重算。

**为什么先 DB 后删缓存**：反过来（先删、后更新）会留下大窗口，删除已经用完，旧值却刚刚回填：

```text
T1  删缓存
T2  读请求 miss → 读到 DB 旧值 v1 → 回填 v1
T3  更新 DB 为 v2 提交        ← 缓存停留在 v1，直到 TTL 过期
```

正过来的顺序只剩一个窄窗口：

```text
T1  读请求 miss，读 DB 拿到 v1（写请求尚未提交）
T2  写请求更新 DB 为 v2 并提交，随后删缓存（缓存本就为空，删了个寂寞）
T3  T1 把 v1 回填进缓存       ← 旧值存活到 TTL 过期
```

T3 依赖“读恰好发生在写提交前、回填发生在删除后”，是毫秒级窗口；兜底手段见第 3 节。

### 2.4 删除：与更新相同

先删 DB 记录，再删缓存；如果查过“不存在”，负缓存一并清理。

```go
func (s *UserService) DeleteUser(ctx context.Context, id int64) error {
    if err := s.db.DeleteUser(ctx, id); err != nil {
        return err
    }
    // 删除后可能有人查询该 id 并回填负缓存，属正常行为，无需额外处理
    if err := s.cache.Delete(ctx, key(id)); err != nil {
        log.Warn("cache_delete_failed", "key", key(id), "err", err) // 进重试，见 3.2
    }
    return nil
}
```

删除后有人查询该 id 会回填负缓存，属正常行为，无需额外处理。

### 2.5 缓存调用的超时与错误语义

前四节的示例有两个共同写法：缓存操作都在 DB 提交之后同步执行、报错只 `log.Warn` 不返回 error。原因如下。

**同步调用的性能账**。写路径一次 `DEL` 是 O(1) 命令，同机房 0.1～1ms，前面的 `UPDATE` + commit 本身要 1～10ms，正常情况可接受，换来的是毫秒级收敛。真正的风险是 Redis 变慢（不是挂掉）时拖垮写路径——Redis 客户端默认超时常为秒级，Redis 一抖每个写请求都陪着等。因此请求路径上的缓存操作必须配短超时，宁可失效失败走重试：

```go
cache := redis.NewClient(opts)
cache.SetTimeout(100 * time.Millisecond)
```

配套纪律：缓存操作不放在 DB 事务内（会把网络往返变成持锁时间）；短超时同时防止慢调用堆积耗尽连接池。嫌慢可异步化（commit 后起 goroutine 删、或只走 3.2 的 outbox 不同步删），代价是 goroutine 随进程崩溃丢掉这次删除（旧值活到 TTL）、纯 outbox 收敛从毫秒级变成 relay 轮询间隔级——用写路径延迟换收敛速度，不是免费的。

**缓存报错不返回 error**。判断标准：调用方关心的业务结果失败了吗？`UPDATE` 已提交、事实源已成功，业务就是成功。此时返回 error 等于谎报“更新失败”，会诱导调用方重试（重复执行已成功的更新）或补偿（回滚实际成功的操作），用户也会看到“保存失败”但刷新后是新值。而失效失败的后果只是旧值多活一会儿，由 TTL 和 outbox 重试收敛。两边代价不成比例，所以降级为运维事件：`log.Warn`（带 key）+ 指标告警 + 进重试。不返回 error 不等于吞掉，它必须被看见、被重试，只是不改变这次调用的返回值。

读路径同理：`Get` 报错按 miss 处理、回源 DB；`Set` 回填失败不影响本次返回。Redis 整体不可用时全部读压到 DB，由限流熔断（学习地图 5.3）保护，不是把 error 抛给用户。

**反例：Redis 是事实源时必须返回 error**。分布式锁、分布式限流器中 Redis 就是事实源，没有 DB 兜底，`AcquireLock` 失败必须原样返回让调用方放弃操作。原则一句话：错误向上传播的边界 = 事实源的边界；派生数据的故障就地消化，事实源的故障如实上报。

### 2.6 事务中的缓存维护

前几节示例都是单条语句。多条语句放进一个事务时，原则：**事务体内只写 DB（业务数据 + 失效意图），所有缓存操作在 commit 返回之后执行**。

在事务内改缓存的问题：

- 提前删除把 2.3 的毫秒级窄窗口放大成整个事务时长：`DEL` 之后到 commit 之间还隔着剩余 SQL 的执行，并发读者在此期间 miss、读到提交前的旧值、回填旧值；commit 落地后 DB 新、缓存旧，删除已用完，旧值活到 TTL。事务越长窗口越大。
- 回滚语义变脏：commit 后才操作缓存时，回滚等于“什么都没发生”，缓存旧值与 DB 一致、零处理；事务内先删再回滚则产生无谓回源，且删除之外的操作还需要补偿逻辑。
- 缓存网络调用在事务内，把往返延迟变成持锁时间和连接占用（见 2.5）。

```go
func (s *OrderService) TransferOrder(ctx context.Context, orderID, fromUser, toUser int64) error {
    var keys []string
    err := s.db.InTx(ctx, func(tx Tx) error {
        if err := tx.UpdateOrderOwner(ctx, orderID, toUser); err != nil {
            return err
        }
        if err := tx.UpdateUserStats(ctx, fromUser, toUser); err != nil {
            return err
        }
        // 影响面在体内收集：commit 后旧值（fromUser）已查不到
        keys = append(keys, orderKey(orderID), userKey(fromUser), userKey(toUser))
        for _, k := range keys {
            if err := tx.InsertOutbox(ctx, "cache_delete", k); err != nil {
                return err
            }
        }
        return nil
    })
    if err != nil {
        return err // 回滚：缓存不动
    }
    // commit 后 best-effort 快速失效；失败由 outbox 行保证最终失效（3.2）
    if err := s.cache.DeleteBatch(ctx, keys...); err != nil {
        log.Warn("cache_delete_failed", "keys", keys, "err", err)
    }
    return nil
}
```

要点：key 在事务体内收集，尤其是失效对象由旧值决定的场景（转移订单要同时失效 A、B 两个用户）；outbox 行随事务提交，commit 与 best-effort 删除之间崩溃也不丢失效，同步 `DEL` 只是快速生效的优化；多 key 用 pipeline 一次往返，整批失败走 outbox，不逐个重试。2.2 创建与 2.4 删除的多语句事务版本同样遵循“体内收集、commit 后失效”。

### 2.7 缓存维护放在哪一层：声明与执行分离

Cache-Aside 隐含假设“repo 方法返回 = 新状态已全局可见”，事务打破了这个假设：commit 的控制权在 repo 之上，repo 写完 DB 返回时事务尚未提交。此时 repo 内立即删缓存即 2.6 的反模式（删除发生在提交前），上层回滚时缓存已被白删；事务内的读也必须走 tx 连接拿一致快照，不能走缓存。解法是把“维护缓存”拆成两件事：**repo 声明影响面（它最懂 key 规则），事务边界执行失效（它知道 commit 何时发生）**。

三种落法：

| 落法 | 适用 | 失效时机 | 事务内安全 | 收敛速度 |
| --- | --- | --- | --- | --- |
| repo 层直接维护 | 无事务的单语句读写 | 方法返回时 | 否 | 毫秒级 |
| Unit of Work 登记 | 服务层显式开事务 | commit 后 | 是 | 毫秒级 |
| outbox 驱动 | 表有多方写入、或缓存本就秒级容忍 | relay 消费时 | 是 | 秒级（轮询间隔） |

Unit of Work 登记模式：repo 只登记，事务包装器在 commit 成功后统一执行：

```go
// repo 只登记
func (r *OrderRepo) Transfer(ctx context.Context, tx *Tx, orderID, fromUser, toUser int64) error {
    if err := tx.Exec(ctx, `UPDATE orders SET user_id=$2 WHERE id=$1`, orderID, toUser); err != nil {
        return err
    }
    tx.RegisterInvalidate(orderKey(orderID), userKey(fromUser), userKey(toUser))
    return nil
}

// 事务包装器在 commit 成功后统一失效
func (s *OrderService) InTx(ctx context.Context, fn func(tx *Tx) error) error {
    tx := s.db.Begin(ctx)
    defer tx.Rollback(ctx)
    if err := fn(tx); err != nil {
        return err // 回滚：登记的 key 全部丢弃，缓存不动
    }
    if err := tx.Commit(ctx); err != nil {
        return err
    }
    if err := s.cache.Delete(ctx, tx.pending...); err != nil { // 见 2.5：不返回 error
        log.Warn("cache_delete_failed", "keys", tx.pending, "err", err) // 进 outbox
    }
    return nil
}
```

outbox 与缓存的关系要分两种形态。**混合形态（默认）**：事务内写 outbox 行 + commit 后同步 DEL（3.2）。正常路径由同步 DEL 毫秒级收敛，relay 随后消费 outbox 行再删一次——DEL 幂等，是空操作；relay 慢或短暂停机不影响正常路径的收敛速度。outbox 只在同步 DEL 失败的异常路径生效，失效时间 = relay 延迟，远好于旧值活满 TTL。**纯 outbox 驱动（特例）**：不要同步 DEL，完全由 relay 删缓存。代价是每次写入的不一致窗口都变成 relay 轮询间隔（relay 故障时更长），且要承担 outbox 表、relay、清理与监控这套基础设施，单一服务写表时不值得。它的真实收益不是消除顺序风险（UoW 登记已解决顺序，但解决不了 commit 与删除之间崩溃的丢失；同事务写 outbox 行才保证不丢），而是**覆盖绕过应用代码的写入者**——批处理作业、DBA 修数、其他服务直写同一张表都不经过 repo，应用层同步删除全部失效，CDC 读 WAL 却都能捕获。适用条件：表有多方写入，或缓存本就是秒级容忍的读模型。

两个附带结论：走缓存的读只服务“独立展示型单点读”，签名里带 `tx` 的读一律走 tx 连接绕开缓存；Spring 中 `@CacheEvict` 与 `@Transactional` 的拦截器顺序决定 evict 在 commit 前还是后，默认顺序下可能先 evict 后 commit（复现 2.6 的窗口），可用 `TransactionAwareCacheDecorator` 把缓存操作延迟到 afterCommit。

## 3. 失效失败的兜底（三层递进）

### 3.1 第一层：TTL

所有缓存值必须设置过期时间。TTL 定义了最坏情形下的最大陈旧时间，是最后一道防线；前两层怎么出错，TTL 之后数据必然正确。

### 3.2 第二层：删除重试（Outbox / 任务表）

更新事务里同时写一条“待删除 key”记录，由 relay 或任务队列消费执行 DEL，失败重试：

```go
func (s *UserService) UpdateUser(ctx context.Context, id int64, patch Patch) error {
    return s.db.InTx(ctx, func(tx Tx) error {
        if err := tx.UpdateUser(ctx, id, patch); err != nil {
            return err
        }
        // 与业务同事务落库：业务提交则“终将被删除”也被保证
        return tx.InsertOutbox(ctx, "cache_delete", key(id))
    })
}
```

正常路径上仍可同步删一次缓存作为快速生效；outbox 负责的是删除失败后的最终收敛。

### 3.3 第三层：版本号 + 墓碑删除

收窄 2.3 的窄窗口。

**为何要写墓碑**。自然的补救是“回填前比较版本：旧于缓存中的版本就拒绝”，但它没有比较对象——竞态发生时缓存刚被 `DEL`，回填面对空缓存，只能放行旧值：

```text
T1  读 miss，SELECT 得 v1（version=5）
T2  写提交 v2（version=6）→ DEL cache
T3  回填 v1：缓存为空，版本无从比较 → 旧值通过
```

`DEL` 的缺陷是抹掉了比较对象。墓碑把“删除”换成一次携带新版本的写（`{version: 6, tombstone: true}`）：旧值被清出（读者见墓碑视为 miss、照常回源），同时留下“已更新到 6”的证据，回填时的版本比较永远有对象。删除只做清除，墓碑在清除的同时留下记号。

- 缓存值统一携带版本：用显式 `version` 列（`UPDATE ... SET version = version + 1 RETURNING version`），不用 `updated_at`（应用时钟不保证单调）。
- 更新路径不 `DEL`，改为写墓碑：`SET key = {version: 新版本, tombstone: true} EX 30s`。墓碑 TTL 只需覆盖在途读请求的回填时间（SELECT→SET）；写失败同样进 outbox 重试。
- 回填用 Lua 原子判断——缓存中版本（含墓碑）大于回填版本则拒绝：

```lua
local cur = redis.call('GET', KEYS[1])
if cur and cjson.decode(cur).version > tonumber(ARGV[1]) then
    return 0 -- 缓存中有更新版本（含墓碑），拒绝旧值回填
end
redis.call('SET', KEYS[1], ARGV[2], 'EX', ARGV[3])
return 1
```

时序回放：读者读到 v1(version=5)；写者提交 v2(version=6) 并写墓碑 {version:6}；读者回填 v1 时 6 > 5 被拒绝；之后的新读者读到 v2(version=6)，6 > 6 不成立，正常落地并替换墓碑。

边界：墓碑 TTL 若短于某个读请求的在途时间（长 GC 暂停、网络抖动），窗口仍存在，只是概率从毫秒级缩到极端暂停场景；最终兜底仍是值本身的 TTL。变体：版本存独立 key `ver:{id}`（写者递增、永不删除），回填时比较——等价，代价是双份 key 且写侧也要保证原子。

### 3.4 延迟双删的定位

“提交后删一次、延迟几百毫秒再删一次”针对的正是 2.3 的窄窗口，但延迟时长只能靠猜，收益有限；能上 3.2 或 3.3 时优先这两个。

## 4. 常见坑

| 坑 | 场景 | 后果 | 解法 |
| --- | --- | --- | --- |
| 穿透 | 反复查询不存在的数据（`id=999999`、恶意乱试） | 缓存永远 miss，每个请求都打到 DB | 负缓存（空值短 TTL）；ID 空间大时用布隆过滤器 |
| 击穿 | 热点 key 恰好过期，并发请求同时 miss | 同一瞬间全部打到 DB | 进程内 singleflight；多实例用短租约或逻辑过期 |
| 雪崩 | 大量 key 同一时刻过期（统一 TTL、缓存重启） | DB 被瞬间打满 | TTL 加随机抖动；多级缓存；DB 侧限流降级 |

击穿的 singleflight 写法：

```go
v, err, _ := s.group.Do(key(id), func() (any, error) {
    return s.db.GetUser(ctx, id) // 同 key 的并发 miss 只回源一次
})
```

**逻辑过期**：缓存不设物理过期，值里带逻辑过期时间；读到已逻辑过期的值时，由一个请求去刷新，其余请求先返回旧值。

两个运维向的坑：

- 热 key：单 key QPS 极高（爆款商品），单分片成为瓶颈。解法：本地内存缓存兜一层，或把 key 复制为 `key#1..#N` 分散到多分片。
- 大 key：value 数十 KB 以上（大列表、全量数据），Redis 单线程处理命令会被它阻塞。解法：拆分成多个小 key。

不要碰：write-behind（只写缓存、异步刷 DB）。缓存已确认而 DB 未写之间崩溃会丢数据，除非业务明确能承受。

## 5. 四种缓存模式对比

| 模式 | 写路径 | 一致性 | 适用 |
| --- | --- | --- | --- |
| Cache-Aside | 写 DB + 删缓存 | 秒级收敛 | 默认选择 |
| Read-Through | 同 Cache-Aside，miss 回源由缓存库代做 | 同上 | 只是代码组织方式差异（Spring `@Cacheable`） |
| Write-Through | 同步写缓存 + 写 DB | 更强 | 写少读多且每写必读；写延迟升高 |
| Write-Behind | 只写缓存，异步刷 DB | 弱，有丢数据窗口 | 极少使用 |

## 6. 按一致性需求速查

| 需求 | 做法 | 不一致窗口 |
| --- | --- | --- |
| 能接受分钟级旧数据（商品描述、排行榜） | Cache-Aside + TTL + 抖动 | ≤ TTL |
| 更新后要尽快生效（用户名、价格） | 提交后删缓存 + outbox 重试删除 | 正常时毫秒级 |
| 写后立刻读，要看到自己写的值 | 写接口直接返回新值；或该请求短窗口读 DB | 对写者本人为 0 |
| 跨 key 强一致、对账 | 不经过缓存，直读 DB | 0 |

## 7. 命中率统计

原则：**在应用层（Cache-Aside 调用点）打点，不在 Redis 服务端统计**。命中率 = hits / (hits + misses)，按业务维度分开看。

为什么不用 Redis `INFO stats` 的 `keyspace_hits` / `keyspace_misses`（只做全局参考）：

- 无法归属：一个实例常被多服务、多类 key 共用，实例级命中率说明不了单类缓存的表现。
- 看不到语义：应用层知道 miss 之后有没有回源 DB、属于哪个业务方法；Redis 只知道 GET 返回 nil。
- error 污染统计：Redis 抖动时 GET 报错，控制流上当 miss 处理（2.5），但它是 `error` 不是 `miss`；混在一起，命中率突降时分不清是缓存故障还是 key 设计问题。

实现：cache client 外包一层装饰器统一打点，业务代码不散落计数逻辑：

```go
type MetricsCache struct {
    inner   Cache
    metrics *Metrics // cache_requests_total{cache, result}
}

func (c *MetricsCache) Get(ctx context.Context, key string) (val any, ok bool, err error) {
    val, ok, err = c.inner.Get(ctx, key)
    switch {
    case err != nil:
        c.metrics.Inc("user", "error") // 与 miss 分开，见 2.5
    case ok:
        c.metrics.Inc("user", "hit")
    default:
        c.metrics.Inc("user", "miss")
    }
    return
}
```

上报计数器、PromQL 算比率（比率 gauge 无法跨实例聚合）：

```promql
sum by (cache) (rate(cache_requests_total{result="hit"}[5m]))
/
sum by (cache) (rate(cache_requests_total{result!="error"}[5m]))
```

单看命中率不够，配三个诊断维度：

| 指标 | 回答什么问题 |
| --- | --- |
| miss 后回源 DB 的 QPS | 命中率的实际意义——DB 到底减负多少 |
| miss 分类：不存在 / 过期 / 刚被失效 | 不存在占比高 → 穿透，查负缓存是否生效（2.1）；刚失效占比高 → 写频繁导致缓存形同虚设 |
| Redis 端 `evicted_keys` / `expired_keys`（`INFO`） | miss 是内存淘汰（容量不足）还是天然 miss |

注意：负缓存命中计为 hit（没有回源 DB）——命中率的本质是“省掉了多少 DB 查询”，不是“key 是否有真值”。

告警两条：命中率突降（失效风暴、内存淘汰、爬虫扫不存在 id；error/miss 分类可立即定位）；`result="error"` 速率上升（Redis 健康，优先于命中率告警）。

## 8. 参考资料

- [Redis：Cache-Aside 与缓存击穿保护](https://redis.io/docs/latest/develop/use-cases/cache-aside/)
- [Microsoft：Cache-Aside Pattern](https://learn.microsoft.com/en-us/azure/architecture/patterns/cache-aside)
- 本仓库 [学习地图](./backend-classic-models-learning-map.md)：7.1 缓存读取与防击穿、5.4 Transactional Outbox、项目五
