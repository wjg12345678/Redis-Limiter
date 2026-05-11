# 面试讲稿：Redis 分布式限流组件

## 1. 项目一句话

这是一个面向 Python 后端服务的 Redis 分布式限流组件，底层用 C++17、hiredis 和 Redis Lua 实现滑动窗口与令牌桶，通过 pybind11 暴露给 Python，并接入 FastAPI 示例，支持短信验证码防刷、Redis 故障降级、metrics、Docker 测试和压测验证。

## 2. 为什么做这个项目

单机内存限流在多实例部署时会失效。

例如验证码接口限制同一手机号 60 秒只能发 1 次，如果服务部署 4 个实例，每个实例都用本地计数，那么同一个手机号最多可能被放行 4 次。这个项目把限流状态放到 Redis，让多个实例共享同一份配额，避免多实例下的额度超发。

## 3. 项目主线怎么讲

建议按这个顺序讲：

1. 先讲问题：登录、短信验证码、下单接口容易被刷，本地限流在多实例下不准确。
2. 再讲方案：用 Redis 保存限流状态，用 Lua 把读取、补充、扣减、过期设置合成一个原子操作。
3. 再讲工程化：C++ 封装连接池和限流器，pybind11 暴露给 Python，FastAPI 可以直接调用。
4. 再讲业务落地：短信验证码接口按手机号、用户、IP 三个维度限流。
5. 最后讲可靠性：Redis 不可用时进入本地令牌桶、fail-open 或 fail-closed，并通过 `backend_status` 暴露状态。

## 4. 核心设计

### Redis Lua 原子操作

限流不是简单的 `GET` 后再 `SET`，因为高并发下会有竞态。项目把判断、扣减、设置 TTL 放进同一个 Lua 脚本，由 Redis 单线程执行，保证同一个 key 的状态更新是原子的。

### Token Bucket

令牌桶适合控制平均速率并允许一定突发。Redis 里用 Hash 保存：

- `tokens`：当前令牌数
- `last_ms`：上次补充时间

每次请求时，Lua 脚本通过 Redis `TIME` 获取当前时间，先按时间差补充令牌，再判断是否足够扣减。

### Sliding Window

滑动窗口适合严格控制某个窗口内最多请求多少次。项目用 Redis ZSET 保存请求时间戳，脚本每次先清理窗口外记录，再判断当前窗口内数量是否超过阈值。

### Redis TIME

项目没有用每个业务实例的本地时间，而是在 Lua 脚本里调用 Redis `TIME`。这样多个实例共用 Redis 的时间源，避免机器时钟漂移导致窗口判断或令牌补充不一致。

### SCRIPT LOAD + EVALSHA

项目优先用 `SCRIPT LOAD` 加载脚本，然后用 `EVALSHA` 执行。好处是避免每次请求都传完整 Lua 脚本，减少网络传输。遇到 Redis 脚本缓存丢失时，会处理 `NOSCRIPT` 并重新加载。

## 5. 业务落地：短信验证码防刷

FastAPI 示例里新增了 `POST /sms/send-code`。

默认规则：

- 同一手机号：约 60 秒 1 次
- 同一用户：约 1 小时 5 次
- 同一 IP：约 1 分钟 20 次

请求流程：

```text
Client
  -> /sms/send-code
  -> 检查 phone_per_minute
  -> 检查 user_per_hour
  -> 检查 ip_per_minute
  -> 全部通过后调用 FakeSmsGateway
  -> 返回 message_id 和每个维度的限流结果
```

如果任一维度被拒绝，接口返回 HTTP `429`，并告诉调用方：

- `blocked_rule`
- `retry_after_ms`
- `backend_status`
- `fallback_hit_count`
- `redis_error_count`

这个场景比单纯 `/rate-limit/check` 更适合面试，因为它能说明限流组件如何接入真实业务入口。

## 6. 故障降级怎么讲

Redis 不可用时，远端限流无法保证全局一致性。项目提供三种策略：

- `LocalTokenBucket`：每个实例本地限流，牺牲全局一致性，保留基础保护。
- `FailOpen`：Redis 挂了就放行，优先保证可用性，但可能放大流量风险。
- `FailClosed`：Redis 挂了就拒绝，优先保护下游，但可能误杀正常请求。

面试时要主动说明：本地 fallback 不能继续保证多实例全局配额，只能作为 Redis 故障时的降级保护。

## 7. 工程化点

可以重点讲这些：

- Redis 连接池：复用 hiredis 连接，避免每次请求重新建连。
- `max_retries`：用于连接创建和重连阶段，不盲目重放已经发出的限流写命令，避免重复扣减。
- health check：空闲连接取出后在锁外 `PING`，避免网络探测时长时间阻塞业务线程。
- pybind11 GIL：阻塞 Redis 调用释放 GIL，降低 Python 多线程场景下的互相阻塞。
- 自动化验证：Docker Compose、pytest、smoke、benchmark、CI 都能跑。
- 可观测性：FastAPI `/metrics` 暴露请求数、允许数、拒绝数、Redis 错误数和 fallback 次数。

## 8. 压测结果怎么讲

仓库里的 `reports/benchmark-report.md` 是一页版报告。

最新短压测结果：

- 独立 key：约 `7055.60 QPS`
- 热点 key：约 `19475.20 QPS`
- 严格有效性：理论放行 `45`，实际放行 `45`，`over_issued=0`
- pytest：`9 passed in 0.53s`

注意不要把短压测说成生产容量评估。更稳妥的表达是：这说明在当前 Docker 环境和参数下，Redis Lua 原子扣减没有出现超发，组件功能正确性和基础吞吐达到了 demo 目标。

## 9. 高频追问

### 为什么不用本地内存限流？

本地内存只能限制单实例。多实例部署时，每个实例都有自己的计数，会导致总放行量超过预期。Redis 可以让多个实例共享同一份配额。

### 为什么用 Lua？

限流涉及读取状态、计算、扣减、设置过期时间。拆成多条 Redis 命令会有竞态，Lua 可以让这几步在 Redis 内部一次原子完成。

### 为什么用令牌桶，不只用滑动窗口？

令牌桶适合控制平均速率并允许突发，适合接口流量治理。滑动窗口更严格，适合明确要求某个时间窗口内最多 N 次的场景。项目两个都保留，业务示例主要用令牌桶。

### Redis 挂了怎么办？

走 `ResilientTokenBucketLimiter`，根据配置进入本地令牌桶、fail-open 或 fail-closed。默认本地令牌桶保留基础保护，同时通过 `backend_status=Fallback` 暴露状态。

### 本地 fallback 会不会超发？

会。在多实例场景下，本地 fallback 不能共享配额，只能单实例保护。所以它是可用性和一致性的取舍，不是全局限流的等价替代。

### 热点 key 会有什么问题？

热点 key 会集中到 Redis 单线程和同一个 Lua 脚本执行路径，吞吐上限受 Redis 单点影响。后续可以考虑 Redis Cluster、分片 key 或更细粒度规则拆分。

### 滑动窗口有什么成本？

滑动窗口用 ZSET 保存窗口内请求记录，高 QPS 和长窗口下会产生较多元素，内存和清理成本比令牌桶高。

### 为什么不对失败命令重试？

连接创建和重连可以重试。但限流写命令如果已经发给 Redis 后客户端丢失响应，无法知道 Redis 是否已经扣减。直接重放可能重复扣减，所以项目避免盲目重放写命令。

### 多维短信规则是不是原子的？

当前 demo 是顺序检查手机号、用户、IP 三个维度，适合展示业务接入。严格 all-or-nothing 的多维配额需要进一步写成一个 Redis Lua 多 key 原子脚本。

## 10. 项目缺点要主动说

建议主动承认这些边界：

- 当前不是完整限流平台，而是一个可接入的轻量组件。
- 单 Redis 是瓶颈和单点，未接 Sentinel / Cluster。
- 本地 fallback 不能保证多实例全局一致性。
- 短信多维规则是顺序检查，不是 all-or-nothing 原子事务。
- 压测是短时间 Docker 环境结果，不代表生产容量。
- Python 包发布还可以继续补 `pyproject.toml` 和 wheel 构建。

主动讲清楚这些，反而说明你知道系统边界。

## 11. 简历写法

可以放 3 到 4 条：

- 基于 `C++17 + hiredis + Redis Lua + pybind11` 实现分布式限流组件，支持令牌桶、滑动窗口和 Python 服务接入。
- 使用 Redis Lua、`SCRIPT LOAD + EVALSHA` 和 Redis `TIME` 保证多实例共享配额下的原子扣减与统一时间源，热点 key 严格压测下 `over_issued=0`。
- 设计 `ResilientTokenBucketLimiter`，支持 Redis 故障时本地令牌桶、fail-open、fail-closed 降级，并通过 `backend_status` 暴露后端状态。
- 接入 FastAPI 短信验证码防刷场景，实现手机号、用户、IP 多维限流，并补充 Docker smoke、pytest、benchmark、Prometheus metrics 和 CI 验证链路。

## 12. 面试开场版本

可以这样讲：

> 我这个项目是一个 Redis 分布式限流组件，主要解决多实例服务里本地限流额度不共享的问题。核心实现是 C++ 封装 Redis 连接池和两种限流算法，限流状态放在 Redis，状态更新通过 Lua 脚本原子完成，并通过 pybind11 暴露给 Python 服务。业务上我接了一个短信验证码接口，按手机号、用户和 IP 三个维度限流。Redis 不可用时组件会进入本地令牌桶 fallback，并把后端状态暴露给调用方。项目里还补了 Docker、pytest、smoke、benchmark 和 metrics，用来验证功能、限流有效性和可观测性。
