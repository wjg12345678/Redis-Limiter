# 面试题库：Redis 分布式限流组件

这份文档按面试可能追问的方向整理。真实面试不会问完所有问题，但下面这些基本覆盖了项目背景、算法、Redis、C++、Python 绑定、业务落地、故障降级、测试压测、缺点和扩展方向。

## 1. 项目背景与整体设计

### 1. 这个项目一句话怎么介绍？

这是一个面向 Python 后端服务的 Redis 分布式限流组件。底层用 C++17、hiredis 和 Redis Lua 实现令牌桶与滑动窗口，通过 pybind11 暴露给 Python，并接入 FastAPI 短信验证码防刷场景，支持 Redis 故障降级、metrics、Docker 测试和压测验证。

### 2. 你为什么做这个项目？

后端服务常见接口，比如登录、短信验证码、下单，都需要限流。如果只用本地内存做限流，单机时可以工作，但多实例部署后每个实例都有自己的计数，总额度会被放大。这个项目把限流状态放到 Redis，由多个实例共享同一份配额，解决多实例下本地限流不准确的问题。

### 3. 这个项目解决的核心问题是什么？

核心问题是多实例服务下的共享配额控制。它不是只实现一个算法，而是把限流算法、Redis 原子更新、Python 接入、业务示例、故障降级和验证链路组合成一个可接入的组件。

### 4. 为什么不用单机内存限流？

单机内存只能限制当前进程。假设短信验证码限制同一手机号 60 秒 1 次，如果部署 4 个服务实例，每个实例都独立计数，那么用户可能在 60 秒内被放行 4 次。Redis 可以让所有实例共享状态，避免这种配额放大。

### 5. 为什么不用数据库做限流？

数据库可以做，但不适合高频限流路径。限流请求通常在业务逻辑之前发生，QPS 高、延迟敏感。用数据库做计数会增加写压力和锁竞争，也会把防刷流量打到核心存储。Redis 的内存存储和原子脚本更适合这类短状态、高频更新场景。

### 6. 为什么选择 Redis？

Redis 适合限流的原因有三个：第一，读写延迟低；第二，支持 TTL，适合窗口状态自动过期；第三，支持 Lua 脚本，可以把判断和扣减放在 Redis 内部原子执行。这个项目利用了这三点。

### 7. 项目整体架构是什么？

调用链是：Python/FastAPI 服务收到请求后，先调用 pybind11 暴露的 `redis_limiter` 模块；模块进入 C++ 限流器；C++ 通过 RedisPool 获取 hiredis 连接；限流逻辑在 Redis Lua 脚本中原子执行；返回 `RateLimitResult` 给 Python；业务根据 `allowed` 决定是否继续访问下游。

### 8. 项目有哪些核心模块？

主要模块包括：`RedisPool` 连接池、`TokenBucketLimiter` 令牌桶、`SlidingWindowLimiter` 滑动窗口、`LocalTokenBucketLimiter` 本地降级、`ResilientTokenBucketLimiter` 故障降级包装器、pybind11 绑定层、FastAPI demo、pytest/smoke/benchmark 验证链路。

### 9. 你在项目里做了哪些工程化工作？

除了算法本身，还做了连接池、Lua 脚本缓存、Redis `TIME` 统一时间源、故障降级、Python GIL 释放、FastAPI 业务示例、Prometheus 风格 metrics、Docker Compose、pytest、smoke test、benchmark report、面试和简历材料。

### 10. 这个项目和普通 CRUD 项目相比有什么亮点？

它更偏基础组件和中间件能力。重点不在业务表增删改查，而在并发一致性、Redis 原子操作、跨语言绑定、故障降级、性能验证和可观测性。面试时可以围绕系统设计和工程边界展开。

## 2. 限流算法

### 11. 什么是令牌桶？

令牌桶维护一个容量固定的桶，系统按固定速率往桶里补充令牌。每个请求需要消耗一定数量的令牌，令牌足够就放行，不够就拒绝。它可以控制平均速率，同时允许一定突发流量。

### 12. 令牌桶适合什么场景？

适合 API 流量治理，比如每秒最多处理 N 个请求，同时允许短时间内消耗桶内积累的令牌。短信验证码、登录接口、下单接口都可以用令牌桶做基础保护。

### 13. 项目里的令牌桶在 Redis 里怎么存？

用 Redis Hash 存两个字段：`tokens` 表示当前令牌数，`last_ms` 表示上次补充令牌的时间戳。每次请求进入 Lua 脚本后，先根据当前时间和 `last_ms` 计算应补充的令牌数，再判断是否允许扣减。

### 14. 令牌桶怎么计算补充令牌？

公式是：`tokens = min(capacity, tokens + elapsed_ms * refill_per_ms)`。其中 `capacity` 是桶容量，`elapsed_ms` 是距离上次补充经过的毫秒数，`refill_per_ms` 是每毫秒补充令牌数。

### 15. `refill_rate` 的单位是什么？

Python/C++ 接口里的 `refill_rate` 是每秒补充令牌数。传入 Lua 前会转换成 `refill_per_ms = refill_rate / 1000.0`。

### 16. 什么是滑动窗口？

滑动窗口限制“最近一段时间内最多允许多少次请求”。项目用 Redis ZSET 保存请求时间戳，每次请求先删除窗口外的旧记录，再统计窗口内数量，如果数量加上本次 cost 不超过阈值就放行。

### 17. 滑动窗口适合什么场景？

适合严格要求某个时间窗口内最多 N 次的场景，比如“1 分钟最多 5 次验证码”。它比固定窗口更平滑，不会在窗口边界出现明显突刺。

### 18. 滑动窗口有什么成本？

它需要保存窗口内每次请求的记录，QPS 高或窗口很长时，ZSET 元素会变多，内存和清理成本比令牌桶高。因此高频接口更常用令牌桶，严格次数限制才用滑动窗口。

### 19. 令牌桶和滑动窗口怎么取舍？

令牌桶适合控制平均速率并允许突发，吞吐和存储成本更可控；滑动窗口适合严格限制最近窗口内次数，但存储成本更高。项目两个都实现，业务 demo 主要用令牌桶，滑动窗口作为算法补充和对比能力。

### 20. 固定窗口有什么问题？

固定窗口按自然时间切片计数，比如 12:00:00 到 12:00:59。问题是窗口边界可能突刺：用户在前一个窗口最后一秒发 N 次，又在下一个窗口第一秒发 N 次，短时间内实际放行接近 2N。

### 21. 漏桶和令牌桶有什么区别？

漏桶强调按固定速率流出，更平滑但突发能力弱；令牌桶允许令牌积累，能容忍一定突发。接口限流常用令牌桶，因为业务流量天然有波峰。

### 22. `cost` 或 `tokens_needed` 是做什么的？

表示一次请求消耗多少配额。普通请求消耗 1 个令牌；如果某个业务操作更重，比如批量请求或下单数量，可以消耗多个令牌。

### 23. 如果 `cost` 特别大会怎样？

令牌桶会直接判断令牌是否足够。滑动窗口会在允许时插入 `cost` 条记录，因此特别大的 cost 会带来更多 ZSET 写入。生产里应该限制 cost 的最大值。

### 24. `peek` 是不是完全只读？

逻辑上 `peek` 不消耗配额，但实现上可能会刷新状态，比如令牌桶会根据时间补充令牌并更新 Redis Hash 和 TTL；滑动窗口也会清理过期记录。所以它不是纯只读操作，只是不扣减本次请求配额。

### 25. `remaining`、`reset_after_ms`、`retry_after_ms` 分别是什么？

`remaining` 是剩余配额；`reset_after_ms` 是恢复到满配额或窗口重置的大致时间；`retry_after_ms` 是被拒绝后建议等待多久再重试。业务可以把 `retry_after_ms` 返回给客户端。

## 3. Redis 与 Lua

### 26. 为什么用 Lua 脚本？

限流操作通常包括读取状态、计算是否允许、扣减配额、设置过期时间。如果拆成多条 Redis 命令，高并发下会有竞态。Lua 脚本在 Redis 内部单线程执行，可以保证这些步骤对同一个脚本调用是原子的。

### 27. Redis Lua 的原子性怎么理解？

Redis 在执行 Lua 脚本时不会插入执行其他命令。也就是说，一个脚本从开始到结束期间，其他客户端不能看到中间状态。这保证了限流判断和扣减不会被并发请求打断。

### 28. Lua 原子性是否等于事务？

它能保证脚本执行期间的原子性，但不等于复杂分布式事务。脚本执行成功后结果生效；如果脚本中出现错误，已经执行的部分命令可能不会像数据库事务那样自动全部回滚。因此脚本要写得简单、可控。

### 29. 为什么使用 Redis `TIME`？

多实例部署时，各服务机器本地时钟可能不一致。如果用本地时间计算窗口和令牌补充，可能导致配额判断偏差。项目在 Lua 中调用 Redis `TIME`，所有实例共用 Redis 时间源，减少时钟漂移问题。

### 30. 为什么不用应用服务器的 `steady_clock`？

`steady_clock` 适合单进程内部计时，但多个服务实例之间没有统一基准。分布式限流要共享状态，也需要共享时间语义，所以更适合用 Redis `TIME`。

### 31. 为什么用 `SCRIPT LOAD + EVALSHA`？

直接 `EVAL` 每次都要发送完整 Lua 脚本，脚本文本较长时会增加网络传输。`SCRIPT LOAD` 先把脚本加载到 Redis，后续用 SHA 执行，减少传输开销。

### 32. Redis 重启或脚本缓存丢失怎么办？

如果 Redis 返回 `NOSCRIPT`，项目会重新 `SCRIPT LOAD`，更新本地缓存的 SHA，然后再次执行 `EVALSHA`。

### 33. 项目如何避免脚本 SHA 缓存的并发问题？

脚本 SHA 用 `shared_ptr<const string>` 缓存，并用原子 load/store 读取更新；只有加载或重载脚本时才进入 `script_mutex`，正常 `EVALSHA` 不持有脚本锁，避免高并发下被全局串行化。

### 34. Redis key 为什么需要 prefix？

prefix 用于隔离不同业务和不同测试场景，避免 key 冲突。比如通用限流可以用 `api:tokenbucket:`，短信场景可以用 `sms:tokenbucket:phone:`。

### 35. Redis key 的 TTL 怎么设置？

令牌桶脚本会给 Hash 设置 TTL，通常按桶填满所需时间的 2 倍计算，并设置最小 TTL。滑动窗口会给 ZSET 设置窗口大小对应的过期时间。这样长时间不用的限流状态会自动清理。

### 36. 为什么令牌桶要设置 TTL？

避免每个用户、手机号、IP 的限流 key 永久留在 Redis 中。限流状态本来就是短期状态，不再访问后应该自动过期，控制内存占用。

### 37. Redis 单线程会不会成为瓶颈？

会，特别是热点 key 和高 QPS Lua 脚本场景。当前项目定位是轻量组件，单 Redis 是明确边界。后续可以考虑 Redis Cluster、按业务分片、拆分 key 或本地预限流。

### 38. 热点 key 有什么问题？

热点 key 会把大量请求集中到同一个 Redis key 和同一个脚本路径，吞吐受 Redis 单线程、网络和 Lua 执行时间限制。面试时要主动说明这是系统边界。

### 39. 为什么当前压测里热点 key QPS 比独立 key 高？

这是短时间 Docker 环境下的快照，受容器调度、Redis 状态、连接复用、worker 行为影响，不能解读为热点 key 一定更快。真正要比较性能，需要多轮压测、固定环境、统计均值和方差。

### 40. Redis Cluster 下 Lua 多 key 有什么限制？

Redis Cluster 要求 Lua 脚本访问的 key 在同一个 hash slot，否则会报错。当前项目未接 Cluster；如果做多 key 原子短信规则，需要用 hash tag 保证相关 key 落在同一 slot，或改成单 key 聚合状态。

## 4. C++ 实现与连接池

### 41. 为什么用 C++ 实现？

主要是展示底层工程能力：RAII 管理 hiredis 连接、连接池复用、线程安全、原子缓存、pybind11 跨语言绑定。对于秋招项目，它比纯 Python 封装更能体现系统编程和工程深度。

### 42. C++ 相比 Python 一定更快吗？

不一定。限流路径主要瓶颈可能在 Redis 网络 IO 和 Redis 单线程执行，不一定在本地语言。这个项目用 C++ 的重点不是简单追求极致性能，而是封装底层 Redis 客户端、连接池和可复用组件。

### 43. RedisPool 做了什么？

RedisPool 负责创建和复用 hiredis 连接，支持连接超时、socket 超时、认证、选择 DB、连接池大小、健康检查、统计信息和维护线程。

### 44. 为什么需要连接池？

每次请求都新建 Redis 连接会产生 TCP 建连开销，延迟高且浪费资源。连接池可以复用连接，降低请求路径开销。

### 45. 连接池是线程安全的吗？

连接池内部用 mutex 和 condition_variable 管理空闲连接队列，同时用 atomic 记录统计信息。获取和归还连接时会保护共享队列。

### 46. `RedisConnectionGuard` 的作用是什么？

它是 RAII guard。构造时从连接池获取连接，析构时自动归还，避免业务代码忘记 release 导致连接泄漏。

### 47. `max_retries` 具体重试什么？

它用于连接创建和重连阶段。如果连接还没建立，或者已有连接失效，需要重新打开 Redis 连接时，会按 `max_retries` 尝试。它不会盲目重放已经发出的限流写命令。

### 48. 为什么不重试已经发出的限流命令？

如果限流写命令已经发给 Redis，但客户端丢失响应，就无法确定 Redis 是否已经扣减了令牌。直接重放可能导致重复扣减，所以项目只对连接创建/重连做重试，避免破坏限流语义。

### 49. health_check 做了什么优化？

健康检查会把空闲连接从池里取出，然后在锁外执行 `PING`，最后把健康连接放回池里。这样避免持有连接池锁进行网络 IO，降低对业务线程获取连接的影响。

### 50. health_check 会检查正在使用的连接吗？

不会直接检查 active 连接，只检查空闲连接并补足连接池。正在使用的连接如果后续出错，会在归还或下次使用时被识别并丢弃。

### 51. 维护线程为什么要用 condition_variable？

维护线程定期做健康检查。如果用 `sleep_for(30s)`，析构时可能最多等 30 秒。改成 `condition_variable::wait_for` 后，析构设置 shutdown 并 notify，可以及时退出。

### 52. 连接失效后怎么处理？

如果连接无效，连接池会丢弃它并减少连接计数；之后如果总连接数低于池大小，会尝试创建新连接补足。

### 53. 统计信息有哪些？

包括 total_connections、active_connections、wait_count、total_requests、failed_requests。它们可以用来观察连接池是否不足、是否频繁等待或失败。

### 54. C++ 里为什么用 RAII？

Redis 连接和 reply 都是资源，需要明确释放。RAII 可以把资源生命周期绑定到对象生命周期，减少泄漏风险。项目里 Redis reply 用 `unique_ptr` 包装，连接用 `RedisConnection` 析构释放。

### 55. 代码里为什么要禁用 RedisConnection 拷贝、允许移动？

Redis 连接是独占资源，拷贝会导致两个对象管理同一个 `redisContext*`，容易 double free。允许移动可以把连接从队列移动到 guard，再移动回连接池。

### 56. C++ 代码有哪些还可以优化的地方？

可以补更完整的单元测试、连接池压力测试、连接失效后的细粒度恢复策略、Redis Cluster 支持、pipeline 批量执行、CMake 安装和 Python wheel 构建。

## 5. pybind11 与 Python 接入

### 57. pybind11 在项目里做什么？

pybind11 把 C++ 类和枚举暴露成 Python 模块 `redis_limiter`。Python 代码可以创建 `RedisConfig`、`RedisPool`、`TokenBucketLimiter`、`ResilientTokenBucketLimiter` 等对象。

### 58. 为什么要释放 GIL？

Python 的 GIL 会限制同一进程内 Python 字节码并行执行。Redis 调用是阻塞 IO，如果持有 GIL，会影响其他 Python 线程执行。项目在阻塞 Redis 调用上加 `py::gil_scoped_release`，让等待 Redis 时不占用 GIL。

### 59. 释放 GIL 是否一定提升性能？

不一定，取决于运行模型。如果是多进程 worker，影响较小；如果是多线程 Python Web 服务，释放 GIL 可以减少阻塞。它是更合理的绑定层设计。

### 60. FastAPI 是异步框架，为什么这里还是同步调用？

示例接口是同步函数，由 FastAPI 在线程池中执行。底层 Redis 调用也是同步 hiredis。生产中如果要完全 async，可以考虑异步 Redis 客户端或把限流调用放在线程池。

### 61. Python 层如何使用这个模块？

先创建 `RedisConfig`，再创建 `RedisPool`，然后创建具体 limiter。调用 `allow(key)` 获取 `RateLimitResult`，根据 `allowed` 决定是否继续业务逻辑。

### 62. pybind11 绑定有什么不足？

目前还不是标准 Python 包发布形态，缺少 `pyproject.toml`、wheel 构建、版本管理和跨平台发布。作为秋招项目可以说明这是后续工程包装方向。

## 6. FastAPI 与短信验证码业务

### 63. 为什么加短信验证码场景？

短信验证码是典型防刷场景，业务语义清楚：同一手机号、同一用户、同一 IP 都要限制频率。它能说明组件如何接入真实业务，而不是只提供一个抽象 `/rate-limit/check`。

### 64. `/sms/send-code` 的流程是什么？

接口收到手机号、用户 ID 和 scene 后，依次检查手机号维度、用户维度、IP 维度。三个维度全部允许后，才调用 `FakeSmsGateway` 模拟发送短信；如果任一维度拒绝，返回 HTTP 429。

### 65. 三个短信限流规则是什么？

默认是：同一手机号约 60 秒 1 次；同一用户约 1 小时 5 次；同一 IP 约 1 分钟 20 次。测试里可以通过参数把阈值调小，验证拒绝逻辑。

### 66. 为什么要同时按手机号、用户和 IP 限流？

只按手机号限制，攻击者可以换手机号；只按用户限制，未登录场景不好处理；只按 IP 限制，会误伤 NAT 后的正常用户。多维限流能覆盖更多滥用方式。

### 67. 短信多维规则是原子的吗？

当前 demo 是顺序检查，不是 all-or-nothing 原子事务。这适合展示业务接入，但严格生产场景下，如果要求三个维度同时扣减或同时不扣减，需要写成 Redis Lua 多 key 原子脚本。

### 68. 顺序检查有什么问题？

如果手机号维度放行并扣减后，用户维度或 IP 维度拒绝，前一个维度的配额已经被消耗。这在防刷场景通常可以接受，因为失败请求也消耗风险预算；但如果业务要求完全不消耗，就要做多 key 原子脚本。

### 69. 为什么使用 FakeSmsGateway？

项目重点是限流组件，不是接真实短信服务。FakeSmsGateway 模拟下游调用，让 demo 有完整业务链路：先限流，再调用下游，最后返回 message_id。

### 70. `/orders` 接口还有价值吗？

有。`/orders` 展示通用业务动作限流，`/sms/send-code` 展示更贴近防刷的多维规则。两个接口说明组件可用于不同业务入口。

### 71. `client_ip` 从哪里来？

示例中通过 FastAPI `Request.client.host` 获取。真实线上如果经过 Nginx 或网关，还需要从可信的 `X-Forwarded-For` 或网关传递字段中取真实 IP，并防止伪造。

### 72. 这个短信限流能直接上生产吗？

不能直接说生产级。它是业务接入 demo。生产还要补真实网关 IP 解析、手机号规范化、黑白名单、风控规则、配置中心、审计日志、Redis 高可用和多维原子脚本。

## 7. 故障降级与可靠性

### 73. Redis 不可用时怎么办？

`ResilientTokenBucketLimiter` 会调用远端 Redis limiter。如果返回 unavailable 或抛异常，就根据 fallback mode 进入本地令牌桶、fail-open 或 fail-closed。

### 74. 三种 fallback 模式分别是什么？

`LocalTokenBucket` 是本地令牌桶，保留单实例保护；`FailOpen` 是 Redis 故障时直接放行，优先可用性；`FailClosed` 是 Redis 故障时直接拒绝，优先保护下游。

### 75. 默认为什么选 LocalTokenBucket？

它在可用性和保护能力之间折中。Redis 挂了以后，服务还能继续处理部分流量，同时每个实例本地仍有基础限流，不会完全裸奔。

### 76. Local fallback 有什么缺点？

它不能共享配额。多实例部署时，每个实例都有自己的本地桶，所以 Redis 故障期间可能超发。面试时要主动说明这是可用性和一致性的取舍。

### 77. FailOpen 适合什么场景？

适合业务可用性优先、限流失败不应该阻断用户的场景，比如低风险查询接口。但对于短信、登录、支付等高风险接口，FailOpen 可能放大攻击流量。

### 78. FailClosed 适合什么场景？

适合保护下游优先的场景，比如支付、库存、核心写接口。缺点是 Redis 故障时会误杀正常请求。

### 79. 如何让业务感知 Redis 是否健康？

`RateLimitResult` 里有 `backend_status`，FastAPI 响应里也暴露了 `redis_error_count` 和 `fallback_hit_count`。metrics 也会暴露 Redis 错误和 fallback 命中次数。

### 80. Redis 恢复后如何回切？

当前逻辑是每次请求优先尝试远端 Redis limiter，如果 Redis 恢复，远端调用成功后自然返回 Healthy，不再走 fallback。没有复杂状态机。

### 81. Redis 抖动会有什么问题？

可能出现一段请求走 Redis、一段请求走 fallback，导致限流状态不完全连续。生产可进一步加熔断、半开探测、退避和更明确的降级窗口。

### 82. 项目有没有熔断？

目前没有完整熔断器。它有错误计数和 fallback，但没有“连续失败后短时间不再访问 Redis”的状态机。后续可以加 circuit breaker。

### 83. 降级期间如何避免本地内存无限增长？

当前本地令牌桶用 unordered_map 保存 key，没有实现淘汰。生产里应该加 TTL、LRU 或定期清理，避免被大量不同 key 打爆内存。

## 8. 测试、压测与可观测性

### 84. 项目有哪些测试？

有功能验证脚本、pytest 集成测试、Docker smoke test 和 benchmark。pytest 覆盖令牌桶容量、滑动窗口并发、Redis fallback、FastAPI `/rate-limit/check`、metrics、短信验证码手机号/IP/fallback 场景。

### 85. smoke test 测什么？

smoke 通过 Docker 网络访问 app，验证 `/healthz`、`/rate-limit/check`、`/sms/send-code`、`/metrics` 的端到端链路，确认容器环境能跑。

### 86. benchmark 测什么？

benchmark 有吞吐模式和有效性模式。吞吐模式用高容量和高补充速率测 Redis 调用 QPS；有效性模式设置小桶和共享热点 key，验证并发下不会超发。

### 87. 什么是 `over_issued=0`？

理论上最多应该放行 `max_tokens + refill_rate * duration` 次。严格有效性压测中理论放行 45，实际也放行 45，所以超发量为 0，说明 Redis Lua 原子扣减有效。

### 88. 为什么严格有效性压测比单纯 QPS 更重要？

限流器首先要正确。如果高并发下超发，即使 QPS 高也没意义。有效性压测验证的是并发竞争下是否严格执行配额。

### 89. 当前压测结果是什么？

最新短压测里，独立 key 约 7.1k QPS，热点 key 约 19.5k QPS；严格有效性压测理论放行 45，实际放行 45，`over_issued=0`。这些结果写在 benchmark report 里。

### 90. 这些压测结果能代表生产容量吗？

不能。它们是 Docker 本地短压测快照，只能证明当前环境下功能和基础吞吐。生产容量评估需要固定机器规格、长时间压测、多轮统计、Redis 监控、网络延迟和故障注入。

### 91. 为什么 pytest 变快了？

之前 Redis-unavailable 测试会按默认连接池大小和重试参数反复尝试，耗时较长。后来测试场景显式设置小连接池、短超时和零重试，并修复维护线程退出等待，所以 pytest 能很快结束。

### 92. metrics 暴露了什么？

FastAPI demo 暴露 Prometheus 风格指标，包括请求总数、允许数、拒绝数、Redis 错误数、fallback 次数、下游调用次数、请求耗时累计、Redis health 和 fallback mode。

### 93. 当前 metrics 有什么不足？

它是 demo 级 in-memory metrics，不是多进程聚合，也没有 histogram bucket。生产里应该使用成熟 Prometheus client，按标签区分接口、规则和状态。

### 94. Grafana 做了什么？

仓库提供 Prometheus 和 Grafana 配置，用于本地查看 Redis 健康、请求数、允许/拒绝、平均耗时、Redis 错误和 fallback 命中情况。

### 95. CI 做了什么？

GitHub Actions 里会构建 Docker 镜像、启动 Redis/app，运行功能验证、smoke、pytest 和严格有效性 benchmark。这保证提交后能自动验证关键路径。

## 9. 安全、边界与生产化

### 96. 项目当前最大的缺点是什么？

最大边界是它还是轻量组件，不是完整生产级限流平台。单 Redis 是瓶颈和单点，未接 Sentinel/Cluster；短信多维规则不是原子事务；本地 fallback 不保证全局一致。

### 97. 如果要生产化，第一步补什么？

优先补 Redis 高可用、配置中心、规则动态更新、熔断器、限流 key 规范、真实 metrics、日志审计和多维原子扣减脚本。

### 98. 如何支持 Redis Sentinel？

需要让 RedisPool 支持从 Sentinel 获取 master 地址，并在主从切换后重新建连。还要处理连接池里旧 master 连接失效和重连。

### 99. 如何支持 Redis Cluster？

需要使用支持 Cluster 的客户端或自己维护 slot 映射，处理 MOVED/ASK 重定向。Lua 多 key 脚本还要保证 key 在同一个 slot。

### 100. 如何做动态限流规则？

可以把规则放到配置中心，比如接口维度、key 维度、容量、补充速率、fallback 策略。服务监听配置变更后调用 `update_limits` 或重建 limiter。

### 101. 如何避免恶意用户制造大量 Redis key？

需要规范 key、限制 key 长度、对输入做归一化和哈希、设置 TTL、增加业务层校验，并监控 key 数量和内存。短信手机号和 IP 这类 key 也应该经过规范化。

### 102. 如何处理用户隐私？

手机号不建议明文直接作为 Redis key。生产里可以加盐哈希或脱敏，日志中也不要打印完整手机号。

### 103. 如何处理网关后的真实 IP？

必须只信任来自可信网关的 `X-Forwarded-For` 或专门头部，不能直接信任用户传入的头。否则攻击者可以伪造 IP 绕过 IP 限流。

### 104. 如何做多租户隔离？

key prefix 应加入租户、业务、环境信息，比如 `prod:sms:tenant_a:phone:`。同时 metrics 也要按业务维度打标签，避免不同租户互相影响。

### 105. 如何处理跨机房？

跨机房 Redis 延迟会影响限流性能。可以按地域使用本地 Redis，或设计分层限流：本地预限流 + 中心全局限流。严格全局一致会牺牲延迟和可用性。

### 106. Redis 内存满了怎么办？

限流 key 应有 TTL，但高基数攻击仍可能撑爆内存。生产要配置 maxmemory、淘汰策略、key 监控、输入限长和异常流量防护。

### 107. Lua 脚本执行太慢怎么办？

要保持脚本简单，避免大循环和大 key 操作。滑动窗口的 ZSET 清理和 cost 批量插入都可能变慢。可以限制 cost、缩短窗口、改用令牌桶或分片。

### 108. 这个项目有没有数据一致性风险？

有。Redis 故障 fallback 期间无法保证全局一致；短信多维顺序扣减不是 all-or-nothing；客户端丢失响应时无法知道写命令是否已生效。这些都需要在面试里说明。

## 10. 代码级追问

### 109. `RateLimitResult` 里有哪些字段？

包括 `allowed`、`current_count`、`remaining`、`reset_after_ms`、`retry_after_ms` 和 `backend_status`。它既告诉业务是否放行，也提供重试建议和后端状态。

### 110. `BackendStatus` 有哪些状态？

`Healthy` 表示 Redis 远端限流正常；`Unavailable` 表示后端不可用且未降级处理；`Fallback` 表示结果来自本地降级或 fail-open/fail-closed。

### 111. `allow_batch` 是真正批量吗？

不是。当前 `allow_batch` 是顺序调用 `allow`，不是 Redis pipeline，也不是批量 Lua。它主要是接口便利，后续可以优化。

### 112. 滑动窗口 member id 为什么要加随机 nonce、线程 id 和 sequence？

ZSET member 必须唯一，否则同一毫秒内重复请求可能覆盖。随机进程 nonce、线程 id 和自增序列可以降低冲突概率，保证同一时间戳下多次请求都能记录。

### 113. TokenBucket 的 `current_count` 怎么计算？

它用 `max_tokens - remaining` 表示桶中已消耗的大致数量。由于令牌会随时间补充，这不是窗口计数，而是当前桶状态的近似展示。

### 114. 为什么 Redis reply 要检查类型？

Redis 命令可能返回错误、nil 或非预期结构。C++ 层需要检查 reply 类型，避免错误解释数据导致未定义行为或错误放行。

### 115. Redis 命令参数为什么用 argv 形式？

`redisCommandArgv` 可以按参数数组传递，避免字符串拼接和转义问题。对于 Lua 脚本、key 和业务输入，更安全也更结构化。

### 116. `health_check()` 会不会影响业务？

影响较小。它只短暂持锁交换空闲队列，网络 `PING` 在锁外执行。但它仍然会占用空闲连接做检查，生产里可以进一步控制频率和超时。

### 117. 连接池大小怎么设置？

要结合应用并发、Redis 延迟和 worker 数量设置。太小会等待连接，太大可能给 Redis 带来过多连接。可以通过 wait_count、active_connections 和 Redis 监控调优。

### 118. 为什么测试里 Redis 不可用场景设置 `pool_size=1`、短超时和 `max_retries=0`？

为了让故障路径测试快速失败并进入 fallback，避免每个测试都等待多次连接超时。生产默认可以更保守，测试要强调确定性和速度。

### 119. Docker Compose 里为什么显式设置 image？

之前 app build 的镜像名和 test/pytest 使用的镜像名可能不一致，干净环境会失败。显式设置 `image: redis-rate-limiter-app` 可以保证 app 和测试服务使用同一镜像。

### 120. 为什么 PYTHONPATH 要 `/work:/app`？

测试容器挂载了当前工作区到 `/work`，镜像内也有一份 `/app` 代码。把 `/work` 放前面，可以保证 pytest 验证的是当前工作区最新代码。

## 11. HR 和项目表达

### 121. 你在项目里最大的收获是什么？

最大的收获是把“算法实现”扩展成了“可接入组件”：不仅要写令牌桶和滑动窗口，还要考虑 Redis 原子性、连接池、故障降级、Python 接入、业务场景、测试、压测和文档表达。

### 122. 项目里最难的点是什么？

最难的是边界处理：并发下不能超发，Redis 故障时要有降级策略，Python 调用 C++ 阻塞 IO 时要处理 GIL，连接池健康检查不能长时间阻塞业务线程。

### 123. 如果重做这个项目，你会怎么改？

我会先定义更明确的生产目标，然后补 Redis Sentinel/Cluster、配置中心、多维原子脚本、成熟 metrics、Python wheel 构建和更系统的长时间压测。

### 124. 这个项目最适合写在简历上的点是什么？

最适合写 Redis Lua 原子限流、C++/pybind11 工程实现、短信验证码多维限流业务落地、Redis 故障 fallback、pytest/smoke/benchmark/metrics 验证闭环。

### 125. 面试官问“这是不是包装轮子”怎么答？

限流算法本身是经典方案，项目价值不在发明算法，而在把算法做成可接入组件：Redis Lua 原子更新、统一时间源、连接池、跨语言绑定、故障降级、业务接入和验证链路。这些是工程实现能力。

### 126. 面试官问“为什么不直接用现成限流库”怎么答？

真实生产可以优先选成熟组件。但作为项目，我希望完整理解分布式限流的关键问题，包括原子扣减、时钟一致、Redis 故障、Python 接入和测试验证。这个项目展示的是我对底层原理和工程边界的掌握。

### 127. 面试官问“你这个项目生产能用吗”怎么答？

我会说它是可接入的轻量组件 demo，还不是完整生产级平台。核心限流链路、故障降级、metrics 和测试已经具备，但生产还需要 Redis 高可用、配置中心、规则灰度、多维原子脚本、日志审计和长时间压测。

### 128. 面试官问“你怎么证明没超发”怎么答？

项目有严格有效性 benchmark：4 个 worker 并发打同一个热点 key，设置 `max_tokens=20`、`refill_rate=5/s`、持续 5 秒，理论最大放行 45 次，实际放行 45 次，`over_issued=0`。这验证了 Redis Lua 原子扣减路径。

### 129. 面试官问“你怎么证明故障降级生效”怎么答？

测试里把 Redis host 指向不可用地址，远端 limiter 失败后进入 `ResilientTokenBucketLimiter` 的本地令牌桶。测试断言前两次允许、第三次拒绝，并检查 `backend_status=Fallback`、`redis_error_count>0`、`fallback_hit_count` 增加。

### 130. 面试官问“项目还有什么缺点”怎么答？

我会主动说：单 Redis 是瓶颈和单点；本地 fallback 不保证全局一致；短信多维规则不是 all-or-nothing；滑动窗口高 QPS 下内存成本高；压测只是短时间 Docker 快照；还缺 Python 包发布和 Redis 高可用。

## 12. 最短背诵版

### 131. 如果只能背一段，怎么说？

我这个项目是一个面向 Python 服务的 Redis 分布式限流组件，解决多实例部署下本地限流配额不共享的问题。底层用 C++17 和 hiredis 封装 Redis 连接池、令牌桶和滑动窗口，通过 Redis Lua 保证判断和扣减原子性，并用 Redis `TIME` 作为统一时间源。通过 pybind11 暴露给 Python，阻塞 Redis 调用释放 GIL。业务上接入 FastAPI 短信验证码防刷，按手机号、用户、IP 多维限流。Redis 不可用时支持本地令牌桶、fail-open、fail-closed 降级，并通过 metrics、pytest、smoke 和 benchmark 验证，严格有效性压测下 `over_issued=0`。
