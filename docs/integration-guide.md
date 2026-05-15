# Redis-Limiter 组件化接入指南

这份文档回答一个核心问题：Redis-Limiter 不是 Atlas 里的内嵌代码，那么其他项目到底应该怎么接入它？接入时 key 怎么设计？限流结果怎么映射到业务响应？Redis 故障时怎么处理？未来如何支持更多语言？

项目当前定位是“可复用限流 SDK / 基础组件”，不是必须独立部署的限流平台。C++ 服务可以直接链接核心库，Python 服务可以通过 pybind11 扩展模块调用，FastAPI demo 只是示例，不是唯一形态。

## 1. 接入形态总览

| 接入形态 | 当前状态 | 适用项目 | 是否需要单独部署 Redis-Limiter |
| --- | --- | --- | --- |
| C++ SDK | 已支持 | C++ WebServer、网关、内部服务 | 不需要 |
| Python 扩展 | 已支持 | FastAPI、Flask、脚本任务 | 不需要 |
| FastAPI Demo | 已支持 | 演示、HTTP 化验证、业务样例 | 可选 |
| HTTP Sidecar | 可扩展 | 任意语言服务 | 需要 |
| gRPC 服务 | 可扩展 | 微服务、多语言、强 schema | 需要 |
| Java/Go/Node SDK | 可扩展 | 常见业务服务 | 不一定 |

当前最推荐的接入方式：

```text
C++ 项目 -> 直接链接 redis_limiter::core
Python 项目 -> import redis_limiter
其他语言 -> 先用 HTTP/gRPC 包一层，再逐步做原生 SDK
```

## 2. 接入链路

通用请求链路：

```text
业务请求进入服务
  -> 从请求中提取 user_id / ip / api / tenant
  -> 按规则构造 rate-limit key
  -> 调用 limiter.allow(key, cost)
  -> allowed=true：继续业务逻辑
  -> allowed=false：返回 429 或业务拒绝
  -> 记录 metrics 和日志
```

关键点：

- 限流应该尽量靠近业务入口。
- 不要在核心业务已经产生副作用后才限流。
- key 设计比算法本身更影响真实效果。
- `allow()` 不是幂等调用，失败和重试策略必须明确。

## 3. C++ 项目接入

### 3.1 CMake 方式

项目导出的核心 target 是 `redis_limiter::core`。业务项目可以把 Redis-Limiter 作为子模块、源码依赖或安装后的包来使用。

示意：

```cmake
target_link_libraries(my_service PRIVATE redis_limiter::core)
```

业务代码只需要依赖公开头文件：

```cpp
#include "redis_pool.hpp"
#include "sliding_window_limiter.hpp"
```

### 3.2 初始化 Redis 连接池

示意代码：

```cpp
rrl::RedisConfig redis;
redis.host = "127.0.0.1";
redis.port = 6379;
redis.db = 0;
redis.pool_size = 8;
redis.connect_timeout_ms = 50;
redis.command_timeout_ms = 50;

auto pool = std::make_shared<rrl::RedisPool>(redis);
```

连接池配置建议：

| 配置 | 建议 |
| --- | --- |
| `pool_size` | 起步按服务工作线程数或 Redis 并发需求设置 |
| `connect_timeout_ms` | 不要太长，限流不能拖垮主业务 |
| `command_timeout_ms` | 登录、验证码等入口建议几十毫秒级 |
| `max_retries` | 只重试连接获取或脚本加载这类安全操作，不盲目重试已发出的扣减 |

### 3.3 创建令牌桶

令牌桶适合登录、注册、短信验证码、API 平均 QPS 控制。

```cpp
rrl::TokenBucketConfig config;
config.capacity = 10;
config.refill_rate = 1.0;
config.key_prefix = "login";

rrl::TokenBucketLimiter limiter(pool, config);

auto result = limiter.allow("ip:127.0.0.1", 1);
if (!result.allowed) {
    // return 429
}
```

业务返回建议：

```http
HTTP/1.1 429 Too Many Requests
Retry-After: 3
Content-Type: application/json

{
  "error": "rate_limited",
  "retry_after_ms": 3000
}
```

### 3.4 创建滑动窗口

滑动窗口适合严格控制“任意 N 秒内最多 M 次”的规则。

```cpp
rrl::RateLimitConfig config;
config.max_requests = 100;
config.window_size_ms = 60000;
config.key_prefix = "api";

rrl::SlidingWindowLimiter limiter(pool, config);
auto result = limiter.allow("user:42:orders", 1);
```

滑动窗口的成本高于令牌桶，因为它需要维护 ZSET 中的请求记录。高 QPS 热点 key 不建议无脑使用滑动窗口。

## 4. Python 项目接入

Python 扩展通过 pybind11 暴露核心能力。适合 FastAPI、Flask、Django、脚本任务等 Python 服务。

示意：

```python
import redis_limiter

redis = redis_limiter.RedisConfig()
redis.host = "127.0.0.1"
redis.port = 6379
redis.pool_size = 8

limiter = redis_limiter.TokenBucketLimiter(
    redis,
    capacity=10,
    refill_rate=1.0,
    key_prefix="login"
)

result = limiter.allow("ip:127.0.0.1", 1)
if not result.allowed:
    raise TooManyRequests(result.retry_after_ms)
```

注意：

- pybind11 层会释放 GIL，让阻塞 Redis 调用不长期占住 Python GIL。
- 释放 GIL 不代表整个服务自动异步化。
- FastAPI 中如果调用是同步函数，仍要注意线程池和阻塞时间。

## 5. FastAPI 接入模式

FastAPI demo 的价值不是“限流服务只能这么部署”，而是演示业务如何使用限流组件。

典型短信验证码链路：

```text
POST /sms/send-code
  -> 按手机号限流
  -> 按用户限流
  -> 按 IP 限流
  -> 全部通过后调用 SmsGateway
  -> 返回发送成功
```

多维规则建议：

| 维度 | 作用 |
| --- | --- |
| 手机号 | 防止同一手机号被刷验证码 |
| 用户 id | 防止登录用户滥用 |
| IP | 防止匿名来源高频请求 |
| tenant | 多租户场景隔离配额 |
| API name | 不同接口使用不同规则 |

多维限流的边界：

```text
当前 demo 是顺序检查多个规则，不是跨多个 key 的全局原子事务。
如果第一个维度扣减成功，第二个维度失败，可能出现局部额度消耗。
对登录、短信这类防刷场景通常可接受；如果业务要求强原子，需要设计单 Lua 多 key，并考虑 Redis Cluster hash slot 限制。
```

## 6. key 设计指南

限流 key 的设计决定效果。

### 6.1 推荐格式

```text
<tenant>:<service>:<api>:<dimension>:<value>
```

示例：

```text
t1:atlas:login:ip:203.0.113.8
t1:atlas:login:user:alice
t1:sms:send:phone:hash_abc123
t1:order:create:user:42
```

### 6.2 不要直接暴露敏感信息

手机号、邮箱、身份证、token 不建议明文进入 Redis key。原因：

- Redis 管理员和日志可能看到 key。
- 监控系统可能采集 key。
- 出问题时截图可能泄露隐私。

建议：

```text
phone -> HMAC-SHA256(phone, secret)
email -> HMAC-SHA256(lowercase(email), secret)
token -> 不要直接作为限流 key，优先用 user_id 或 token hash 前缀
```

### 6.3 避免无限 key

恶意用户可以构造大量不同 key，撑爆 Redis 内存。应对方法：

- key prefix 白名单。
- 对未登录用户优先按 IP / subnet 限流。
- 对高基数字段做规范化和长度限制。
- 设置 TTL，避免冷 key 永久保留。
- 监控 key 数量和内存。

## 7. 限流结果映射

`RateLimitResult` 不只是 `true/false`，还应该用于业务响应和监控。

| 字段 | 用途 |
| --- | --- |
| `allowed` | 是否放行 |
| `remaining` | 剩余额度，用于响应头或日志 |
| `retry_after_ms` | 建议多久后重试 |
| `reset_after_ms` | 窗口或桶恢复时间 |
| `backend_status` | Redis、本地 fallback、fail-open、fail-closed 等状态 |

HTTP 服务建议响应头：

```http
X-RateLimit-Remaining: 3
X-RateLimit-Reset: 1700000000
Retry-After: 2
```

注意不要把内部 Redis key 原样返回给客户端。

## 8. fallback 接入策略

Redis 不可用时，组件支持不同 fallback 思路。

| 模式 | 行为 | 适用场景 | 风险 |
| --- | --- | --- | --- |
| LocalTokenBucket | 使用本地令牌桶继续限流 | 登录、普通 API | 多实例总额度可能放大 |
| FailOpen | Redis 故障时直接放行 | 可用性优先接口 | 防刷能力暂时失效 |
| FailClosed | Redis 故障时拒绝 | 安全优先接口 | Redis 抖动会影响正常用户 |

推荐：

- 登录、注册：LocalTokenBucket 或 FailClosed，取决于风险。
- 短信验证码：FailClosed 更稳，避免 Redis 故障时短信被刷。
- 低风险浏览接口：FailOpen。
- 支付、库存扣减：限流不应替代业务事务，fallback 要非常谨慎。

## 9. 幂等和重试

`allow()` 不是幂等的。原因：

```text
第一次 allow 成功：扣 1 个 token
客户端没有收到响应，以为失败
再次调用 allow：又扣 1 个 token
```

因此：

- 不要对已经发出的 Lua 扣减命令盲目重试。
- 可以重试连接建立、脚本加载、NOSCRIPT 后重新 LOAD 这类安全动作。
- 业务请求如果需要幂等，应该在业务层使用 request_id。
- 如果限流结果丢失，通常宁可按失败或 fallback 处理，也不要无脑再次扣减。

## 10. 和 Atlas 的推荐接入方式

Atlas 中限流只负责登录/注册入口：

```text
POST /api/login
  -> login_ip:<ip>
  -> login_user:<username>

POST /api/register
  -> register_ip:<ip>
```

这样设计的理由：

- IP 维度防单源爆破。
- username 维度防多源撞同一账号。
- 注册 IP 维度防批量注册。
- 上传下载后续也可以加限流，但不要和认证限流混用同一个 key 空间。

Atlas 不应该复制 Redis-Limiter 源码。Atlas 只保留适配层，Redis-Limiter 作为独立组件演进。

## 11. 未来 HTTP/gRPC 服务化

如果要让任意语言接入，可以把 Redis-Limiter 包成独立服务：

```text
Business Service
  -> HTTP/gRPC RateLimitService
  -> Redis-Limiter core
  -> Redis
```

HTTP API 示例：

```http
POST /v1/ratelimit/check
Content-Type: application/json

{
  "tenant": "t1",
  "rule": "login_ip",
  "key": "203.0.113.8",
  "cost": 1
}
```

响应：

```json
{
  "allowed": false,
  "remaining": 0,
  "retry_after_ms": 2400,
  "backend_status": "redis"
}
```

gRPC 更适合强类型、多语言和内部服务治理。HTTP 更容易演示和接入。

## 12. 接入检查清单

上线前检查：

```text
1. 明确限流目标：防刷、控成本、保护下游还是公平性
2. 选择算法：令牌桶或滑动窗口
3. 设计 key：包含租户、服务、接口、维度
4. 设置 TTL：防止冷 key 长期占内存
5. 确定 fallback：Local / FailOpen / FailClosed
6. 确定 429 响应格式
7. 接入 metrics：allowed、denied、fallback、redis_error
8. 压测热点 key 和独立 key
9. 故障演练 Redis 不可用
10. 文档说明 allow 非幂等
```

## 13. 常见错误接入

| 错误 | 后果 |
| --- | --- |
| 所有接口共用一个 key | 一个接口流量影响全站 |
| 只按 IP 限流登录 | 分布式来源可以绕过 |
| 只按用户名限流 | 单 IP 可以撞很多账号 |
| 手机号明文放 key | 隐私泄露 |
| Redis 故障时无限等待 | 主业务被限流组件拖垮 |
| 对 allow 盲目重试 | 额度被重复消耗 |
| 滑动窗口用于所有高 QPS key | Redis ZSET 成本过高 |

## 14. 最终接入建议

如果是面试项目或内部服务，优先这样落地：

```text
第一步：C++ / Python SDK 直接接入
第二步：把 key 规范、fallback 策略和 429 响应写清楚
第三步：接入 Prometheus 指标
第四步：补 Redis 故障演练
第五步：再考虑 HTTP/gRPC 服务化和多语言 SDK
```

不要一开始就把它做成复杂平台。先让一个业务稳定接入，再抽象规则中心、管理面和多语言协议。
