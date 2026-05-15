# Redis-Limiter 生产化加固与平台化路线

Redis-Limiter 当前已经具备核心限流组件能力：C++17 core、hiredis 连接池、Redis Lua 原子脚本、Redis TIME、令牌桶、滑动窗口、pybind11 Python 接入、FastAPI demo、Prometheus/Grafana 示例、Docker 验证和压测报告。

但它还不是完整商业化限流平台。生产化不是把 README 写得更夸张，而是补齐高可用、动态规则、多语言接入、多租户、可观测、熔断和运维能力。

## 1. 当前定位

最准确的定位：

```text
Redis-Limiter 是一个可复用的分布式限流 SDK / 基础组件。它把限流状态放在 Redis，通过 Lua 脚本保证检查和扣减原子性，并提供 C++ 与 Python 接入方式。它可以被 Atlas 或其他业务服务直接集成，也可以继续封装成 HTTP/gRPC 限流服务。
```

不建议说：

```text
这是完整生产级限流平台。
```

原因：

- 当前没有动态规则中心。
- 没有管理后台。
- 没有多语言原生 SDK。
- 没有完整 Redis Sentinel/Cluster 适配。
- 没有多租户配额治理。
- fallback 和熔断还可以更系统化。

## 2. 生产化能力分层

| 层级 | 当前状态 | 生产化目标 |
| --- | --- | --- |
| 算法层 | 令牌桶、滑动窗口 | 增加固定窗口、漏桶、并发数限制、配额型限制 |
| 原子层 | Redis Lua | 支持 Cluster hash tag、多 key 原子规则设计 |
| 连接层 | hiredis 连接池 | Sentinel/Cluster、连接熔断、慢请求隔离 |
| SDK 层 | C++、Python | Java、Go、Node、Rust SDK 或 HTTP/gRPC |
| 服务层 | FastAPI demo | 独立 RateLimitService、统一 API、鉴权 |
| 规则层 | 代码配置 | 动态规则中心、热更新、版本管理 |
| 观测层 | 基础 metrics | 规则维度指标、SLO、告警、trace |
| 运维层 | Docker demo | 灰度、回滚、压测、容量评估、故障演练 |

## 3. Redis 高可用

当前核心依赖 Redis。单 Redis 实例是最大风险之一。

### 3.1 Sentinel

Redis Sentinel 适合主从高可用：

```text
client
  -> Sentinel 查询当前 master
  -> Redis master
  -> master 故障后 Sentinel 切换
  -> client 重连新 master
```

需要补：

- Sentinel 地址配置。
- master name 配置。
- 连接池发现新 master。
- 故障切换期间命令失败处理。
- 脚本 SHA 缓存失效后重新 `SCRIPT LOAD`。

面试说法：

```text
当前版本按单 Redis 地址连接。生产化第一步会加 Sentinel 支持，让连接池不直接绑定固定 master，而是通过 Sentinel 发现当前 master，并在故障切换后重建连接和脚本缓存。
```

### 3.2 Redis Cluster

Redis Cluster 支持分片扩展，但 Lua 多 key 有限制：同一个 Lua 脚本访问的 key 必须在同一个 hash slot。

应对：

```text
rate:{tenant:user}:login_ip:1.2.3.4
rate:{tenant:user}:login_user:alice
```

使用 `{...}` hash tag 让相关 key 落到同一个 slot。

但要谨慎：把太多 key 放到同一个 slot 会制造热点。多维限流是否必须跨 key 原子，要按业务选择。

## 4. 热点 key 治理

限流组件最容易遇到热点 key：

- 全站公共 API：`global:api`
- 秒杀活动：`activity:123`
- 登录接口被攻击：`login:ip:x`
- 单个大客户：`tenant:big`

治理思路：

| 方案 | 说明 | 风险 |
| --- | --- | --- |
| 本地预限流 | 先在进程内粗限流，再打 Redis | 多实例精确度下降 |
| key 分片 | 把一个热点拆成多个子 key | 统计和严格性变复杂 |
| 分层限流 | 全局、租户、用户多层组合 | 多 key 原子性问题 |
| 读写隔离 | metrics 和管理查询不打主 Redis | 系统复杂度增加 |
| 降级策略 | Redis 慢时本地 fallback | 可能超发 |

推荐优先做：

```text
1. 监控每个规则的 denied/allowed 和 Redis latency
2. 对高风险接口加本地预限流
3. 对全局热点 key 做分片或分层规则
4. 明确热点 key 下的精确性边界
```

## 5. 动态规则中心

当前规则多在代码中配置。生产上需要动态规则：

```text
rule_id: login_ip
algorithm: token_bucket
capacity: 10
refill_rate: 1/s
dimension: ip
fallback: local
enabled: true
version: 12
```

### 5.1 规则存储

可以先用 MySQL：

```text
rate_limit_rules
  id
  tenant
  service
  api
  dimension
  algorithm
  capacity
  refill_rate
  window_ms
  fallback_mode
  enabled
  version
  updated_at
```

### 5.2 热更新

SDK 可定期拉取规则：

```text
每 5 秒查询规则版本
版本变化 -> 拉取新规则
校验合法 -> 原子替换本地配置
失败 -> 保留旧规则
```

注意：

- 新规则要有版本号。
- 配置加载失败不能清空旧规则。
- 规则变更要有审计日志。
- 高风险规则要支持灰度。

## 6. 服务化形态

SDK 方式优点是低延迟、少一跳；服务化优点是多语言统一接入。

### 6.1 HTTP 服务

优点：

- 任意语言都能接。
- curl 可以直接调试。
- 和 FastAPI demo 衔接自然。

缺点：

- 每次限流多一次网络调用。
- 限流服务本身需要高可用。
- 超时策略更复杂。

### 6.2 gRPC 服务

优点：

- schema 清晰。
- 多语言生成客户端。
- 内部服务治理更成熟。

缺点：

- 引入 protobuf 和 gRPC 运维成本。
- 浏览器和简单脚本调试不如 HTTP 方便。

### 6.3 推荐路线

```text
阶段 1：保持 C++ / Python SDK
阶段 2：提供 HTTP RateLimitService
阶段 3：补 gRPC IDL
阶段 4：常用语言 SDK 调 HTTP/gRPC
阶段 5：核心语言再做原生 SDK
```

## 7. 多租户治理

如果多个业务共用同一个限流组件，必须做租户隔离。

需要隔离：

- Redis key prefix
- 规则配置
- 指标标签
- 管理权限
- 默认配额
- 降级策略

key 示例：

```text
rl:{tenant_a}:atlas:login:ip:<hash>
rl:{tenant_b}:payment:send_sms:phone:<hash>
```

不要让业务随意传 key。更稳的是业务传结构化字段，由 SDK 或服务端统一生成 key。

## 8. 安全和隐私

生产要补：

| 项 | 建议 |
| --- | --- |
| Redis TLS | 跨机器访问 Redis 时启用 |
| Redis AUTH | 不允许无密码 Redis |
| key 脱敏 | 手机号、邮箱用 HMAC |
| 管理 API 鉴权 | 动态规则和指标不能裸露 |
| 审计 | 规则新增、修改、删除都记录 |
| 租户隔离 | 不同业务不能互相改规则 |
| 请求签名 | 服务化模式下防伪造调用 |

## 9. 可观测性升级

当前已有 metrics 示例。生产需要更细：

### 9.1 核心指标

- `rate_limiter_requests_total`
- `rate_limiter_allowed_total`
- `rate_limiter_denied_total`
- `rate_limiter_fallback_total`
- `rate_limiter_redis_errors_total`
- `rate_limiter_redis_latency_ms`
- `rate_limiter_lua_noscript_total`
- `rate_limiter_pool_in_use`
- `rate_limiter_pool_wait_ms`

### 9.2 标签

建议标签：

```text
tenant
service
rule
algorithm
backend_status
fallback_mode
```

谨慎标签：

```text
raw_user_id
raw_ip
raw_phone
```

高基数标签会撑爆 Prometheus，不要把用户级 key 直接作为指标标签。

## 10. 熔断和超时

限流组件不能拖垮主业务。

建议策略：

```text
Redis latency 连续超过阈值
  -> 短时间熔断远端 Redis
  -> 进入本地 fallback
  -> 后台半开探测
  -> Redis 恢复后逐步回切
```

需要指标：

- 熔断打开次数
- 熔断持续时间
- 半开探测成功率
- fallback 放行/拒绝数量

## 11. 压测体系升级

不要只压 QPS。限流组件必须证明：

| 测试 | 目标 |
| --- | --- |
| 热点 key 压测 | 单 key 并发下不超发 |
| 独立 key 压测 | 多 key 分散时吞吐 |
| Redis 故障压测 | fallback 是否生效 |
| Redis 慢请求 | 主业务是否被拖慢 |
| NOSCRIPT | Redis 重启后脚本恢复 |
| Cluster slot | 多 key 规则是否可用 |
| 长稳测试 | key TTL 和内存是否稳定 |

报告要同时给：

- allowed / denied
- over_issued
- p95 / p99
- Redis CPU
- Redis memory
- 连接池等待时间
- fallback 次数

## 12. 平台化路线

### 12.1 第一阶段：组件稳定

目标：

- C++ / Python 接入稳定。
- 明确 key 规范。
- fallback 行为可配置。
- metrics 可用。
- Redis 故障演练通过。

### 12.2 第二阶段：服务化

目标：

- HTTP `/v1/ratelimit/check`
- gRPC `CheckRateLimit`
- 服务端鉴权
- 请求超时和限流服务自身限流
- 客户端 SDK 封装

### 12.3 第三阶段：规则中心

目标：

- MySQL 或配置中心保存规则。
- 规则热更新。
- 规则版本和审计。
- 灰度发布。
- 回滚。

### 12.4 第四阶段：高可用

目标：

- Sentinel / Cluster。
- 熔断和半开恢复。
- 多实例 RateLimitService。
- 容量评估。
- 告警。

## 13. 面试边界表达

被问“这是不是生产级”，建议答：

```text
它目前是一个可复用限流组件，不是完整限流平台。核心算法和工程链路已经具备，包括 Redis Lua 原子扣减、Redis TIME、连接池、令牌桶、滑动窗口、fallback、Python binding、metrics 和压测。生产化下一步要补 Sentinel/Cluster、动态规则中心、多语言 SDK、服务化 API、熔断、租户隔离和更完善的监控告警。
```

被问“Redis 挂了怎么办”，建议答：

```text
组件提供本地令牌桶、fail-open 和 fail-closed 三类 fallback。具体选哪种由业务风险决定。登录可以选本地 fallback，短信验证码可能更适合 fail-closed，低风险浏览接口可以 fail-open。但本地 fallback 在多实例下会放大总额度，所以必须通过 backend_status 和 metrics 让业务知道当前处于降级状态。
```

## 14. 最终优先级

如果现在继续做，推荐顺序：

```text
1. 写清楚接入规范和 key 规范
2. 增加 Sentinel 支持
3. 增加熔断和半开恢复
4. 增加 HTTP RateLimitService
5. 增加动态规则中心
6. 增加 Redis Cluster hash tag 支持
7. 增加 Java/Go SDK
8. 增加管理后台
```

这条路线能让 Redis-Limiter 从“算法组件”逐步演进成“基础设施组件”，同时不会一开始就陷入平台化过度设计。
