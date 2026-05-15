# Redis-Limiter 完整学习路线

这份文档回答一个问题：**怎么把 Redis-Limiter 学到能面试、能讲源码、能解释工程取舍，而不是只会说“我实现了令牌桶”。**

这个项目不要按文件顺序死读。正确方式是按 **问题、链路、验证** 学：

```text
问题：多实例本地限流为什么不准确，Redis 如何共享配额
链路：业务请求 -> limiter.allow -> RedisPool -> Lua -> RateLimitResult
验证：功能测试、fallback 测试、FastAPI smoke、pytest、benchmark、有效性断言
```

最重要的优先级：

```text
1. 项目定位：可复用限流 SDK / 基础组件，不是完整限流平台
2. Redis Lua 原子性：为什么检查和扣减必须放到一个脚本
3. 令牌桶：Redis HASH 状态、Redis TIME、补充和扣减公式
4. 滑动窗口：Redis ZSET、窗口清理、严格次数限制
5. RedisPool：hiredis 连接复用、超时、重连、健康检查、RAII
6. ResilientTokenBucketLimiter：Redis 故障时的 fallback 策略
7. pybind11：C++ core 如何暴露给 Python，为什么释放 GIL
8. FastAPI Demo：短信验证码手机号/用户/IP 多维限流
9. 测试压测：pytest、smoke、benchmark、over_issued=0 的含义
10. 生产化边界：Redis HA、规则配置、多语言接入、metrics、平台化
```

## 0. 学习目标

学完这个项目，你应该能做到：

- 一句话说清楚 Redis-Limiter 解决什么问题。
- 解释为什么单机内存限流在多实例部署下会失效。
- 画出 C++ / Python / FastAPI 接入 Redis-Limiter 的调用链路。
- 讲清楚 `RedisConfig`、`RedisPool`、`TokenBucketLimiter`、`SlidingWindowLimiter`、`ResilientTokenBucketLimiter` 的职责。
- 解释 Redis Lua 为什么能保证检查和扣减原子性。
- 解释为什么使用 Redis `TIME` 而不是业务机器本地时间。
- 解释 `SCRIPT LOAD + EVALSHA` 和 `NOSCRIPT` 处理。
- 讲清楚令牌桶补充令牌的公式。
- 讲清楚滑动窗口为什么用 ZSET，以及它的内存成本。
- 解释 Redis 不可用时三种 fallback 的区别和边界。
- 解释 `allow()` 为什么不是幂等操作。
- 解释 C++ 连接池为什么用 RAII、禁用拷贝、允许移动。
- 解释 pybind11 绑定层为什么要释放 GIL。
- 讲清楚 FastAPI 短信验证码三维限流 demo 的业务链路。
- 讲清楚 Docker、pytest、smoke、benchmark 分别验证什么。
- 主动承认这个项目不是生产级限流平台，并给出生产化演进方向。

## 1. 项目定位

### 1.1 这个项目是什么

Redis-Limiter 是一个基于 `C++17 + hiredis + Redis Lua + pybind11` 的分布式限流组件。

它的定位是：

```text
可复用限流 SDK / 基础组件
```

不是：

```text
完整限流平台
独立网关产品
带配置中心和管理后台的生产系统
```

它现在提供两种核心接入方式：

| 接入方式 | 是否需要部署 Redis-Limiter 服务 | 说明 |
| --- | --- | --- |
| C++ SDK | 不需要 | 业务进程直接链接 `redis_limiter::core` |
| Python 扩展 | 不需要 | Python 进程直接 `import redis_limiter` |
| FastAPI Demo | 可选 | 用于展示 HTTP 化和业务接入，不是唯一形态 |
| 未来 HTTP/gRPC 服务 | 需要 | 如果要给 Java/Go/Node 等语言统一接入，可以再封装服务 |

### 1.2 它解决什么问题

后端服务经常需要限流：

- 登录接口防暴力破解。
- 注册接口防刷号。
- 短信验证码防刷。
- 下单接口防刷。
- 支付、评论、上传等高风险接口限流。

如果只用本地内存：

```text
实例 A: 允许 10 次
实例 B: 允许 10 次
实例 C: 允许 10 次
实例 D: 允许 10 次
```

原本想限制 10 次，多实例后可能放行 40 次。

Redis-Limiter 把状态放到 Redis：

```text
实例 A/B/C/D
  -> 同一个 Redis key
  -> 同一份配额
  -> Lua 原子检查和扣减
```

### 1.3 和 Atlas 的关系

Atlas WebServer 是业务项目，Redis-Limiter 是通用限流组件。

```text
Redis-Limiter
  -> C++ core
  -> Redis Lua
  -> TokenBucket / SlidingWindow
  -> fallback
  -> Python binding
  -> tests / benchmark / metrics demo

Atlas
  -> 登录 IP 限流
  -> 登录用户名限流
  -> 注册 IP 限流
  -> HTTP 429 响应
  -> 通过 redis_limiter::core 接入 Redis-Limiter
```

面试时要强调：

> Atlas 不复制限流算法源码，只保留登录/注册业务适配；Redis-Limiter 是可复用组件，可以给其他 C++ 或 Python 项目接入。

## 2. 仓库地图

### 2.1 目录结构

```text
Redis-Limiter/
|-- include/
|   |-- redis_pool.hpp              # Redis 配置、连接、连接池、统计
|   `-- sliding_window_limiter.hpp  # 限流结果、算法、fallback、工厂
|-- src/
|   |-- redis_pool.cpp              # hiredis 连接、认证、DB、重连、连接池维护
|   |-- sliding_window_limiter.cpp  # Lua、令牌桶、滑动窗口、本地 fallback
|   `-- python_binding.cpp          # pybind11 绑定
|-- examples/
|   |-- python_demo.py              # 普通 Python 业务调用示例
|   `-- fastapi_demo.py             # FastAPI、短信验证码、metrics
|-- tests/
|   |-- verify_functionality.py     # 功能验证和 Redis fallback
|   |-- test_integration.py         # pytest 集成测试
|   |-- smoke_docker.py             # Docker HTTP 冒烟
|   `-- benchmark.py                # 吞吐和有效性压测
|-- reports/
|   `-- benchmark-report.md         # 压测报告
|-- prometheus/
|-- grafana/
|-- CMakeLists.txt
|-- Dockerfile
`-- docker-compose.yml
```

### 2.2 先读哪些文件

按顺序：

1. [README.md](../README.md)
2. [include/sliding_window_limiter.hpp](../include/sliding_window_limiter.hpp)
3. [include/redis_pool.hpp](../include/redis_pool.hpp)
4. [src/sliding_window_limiter.cpp](../src/sliding_window_limiter.cpp)
5. [src/redis_pool.cpp](../src/redis_pool.cpp)
6. [src/python_binding.cpp](../src/python_binding.cpp)
7. [examples/python_demo.py](../examples/python_demo.py)
8. [examples/fastapi_demo.py](../examples/fastapi_demo.py)
9. [tests/test_integration.py](../tests/test_integration.py)
10. [tests/benchmark.py](../tests/benchmark.py)

不要一开始就陷进 FastAPI 业务代码。先把核心 C++ API 和 Redis Lua 搞清楚。

## 3. 第一阶段：跑起来

### 3.1 构建 C++ core

依赖：

- CMake 3.14+
- C++17 compiler
- hiredis
- pthread

命令：

```bash
cd /home/ubuntu/Redis-Limiter
cmake -S . -B build-core -DREDIS_LIMITER_BUILD_PYTHON=OFF
cmake --build build-core --parallel
```

你要观察：

- 是否生成 `libredis_limiter_core.a`。
- 是否能找到 `hiredis`。
- `REDIS_LIMITER_BUILD_PYTHON=OFF` 时不需要 pybind11。

### 3.2 构建 Python 扩展

依赖：

- Python 3
- pybind11
- hiredis

命令：

```bash
python3 -m pip install -r requirements.txt

cmake -S . -B build \
  -DREDIS_LIMITER_BUILD_PYTHON=ON \
  -Dpybind11_DIR="$(python3 -c 'import pybind11; print(pybind11.get_cmake_dir())')"
cmake --build build --parallel
```

构建产物：

```text
build/redis_limiter*.so
```

### 3.3 Docker Compose 验证

命令：

```bash
docker compose build
docker compose up -d redis app
curl -sS http://127.0.0.1:8000/healthz
```

功能验证：

```bash
docker compose run --rm test remote
docker compose run --rm -e REDIS_HOST=redis-unavailable test fallback
docker compose run --rm pytest
docker compose run --rm smoke
```

压测：

```bash
docker compose run --rm bench --workers 4 --duration 5
docker compose run --rm bench --workers 4 --duration 5 --shared-key

docker compose run --rm bench \
  --mode effectiveness \
  --workers 4 \
  --duration 5 \
  --shared-key \
  --max-tokens 20 \
  --refill-rate 5 \
  --max-over-issue 0 \
  --max-over-issue-ratio 0
```

### 3.4 本阶段要学会什么

你要知道：

- 这个项目既能作为 C++ library，也能构建 Python extension。
- Redis 是必需后端，FastAPI 只是 demo。
- Docker Compose 打通了 Redis、app、test、pytest、bench、smoke、Prometheus、Grafana。
- benchmark 的有效性模式不是看 QPS，而是看是否超发。

## 4. 第二阶段：核心调用链

### 4.1 C++ 调用链

```text
业务服务
  -> RedisConfig
  -> RedisPool
  -> TokenBucketLimiter / SlidingWindowLimiter
  -> allow(key, cost)
  -> RedisConnectionGuard acquire
  -> SCRIPT LOAD / EVALSHA
  -> Redis Lua
  -> RateLimitResult
```

### 4.2 Python 调用链

```text
Python / FastAPI
  -> import redis_limiter
  -> RedisConfig
  -> RedisPool
  -> TokenBucketLimiter
  -> ResilientTokenBucketLimiter
  -> allow(key)
  -> pybind11 进入 C++
  -> Redis Lua
  -> Python RateLimitResult
```

### 4.3 FastAPI 业务调用链

```text
POST /sms/send-code
  -> 解析 phone/user_id/scene
  -> 取 client_ip
  -> phone_per_minute limiter.allow
  -> user_per_hour limiter.allow
  -> ip_per_minute limiter.allow
  -> 任一拒绝返回 429
  -> 全部允许调用 FakeSmsGateway
  -> metrics.observe
  -> 返回 message_id 和 rate_limits
```

## 5. 第三阶段：公共 API

### 5.1 `RedisConfig`

字段：

| 字段 | 作用 |
| --- | --- |
| `host` | Redis 地址 |
| `port` | Redis 端口 |
| `password` | Redis 密码 |
| `db` | Redis DB |
| `connect_timeout_ms` | 建连超时 |
| `socket_timeout_ms` | socket 读写超时 |
| `pool_size` | 连接池大小 |
| `max_retries` | 建连/重连阶段重试次数 |

面试重点：

> `max_retries` 只用于连接创建和重连，不盲目重放已经发出的限流写脚本，因为无法确认 Redis 是否已经扣减配额。

### 5.2 `RateLimitResult`

字段：

| 字段 | 含义 |
| --- | --- |
| `allowed` | 是否放行 |
| `current_count` | 滑动窗口当前计数，令牌桶里通常不作为核心字段 |
| `remaining` | 剩余额度 |
| `reset_after_ms` | 恢复或窗口重置时间 |
| `retry_after_ms` | 被拒绝后建议重试时间 |
| `backend_status` | Healthy / Unavailable / Fallback |

### 5.3 `BackendStatus`

| 状态 | 含义 |
| --- | --- |
| `Healthy` | Redis 正常，结果来自远端限流 |
| `Unavailable` | Redis 不可用，且没有可用 fallback 结果 |
| `Fallback` | 结果来自降级策略 |

### 5.4 `FallbackMode`

| 模式 | 行为 |
| --- | --- |
| `LocalTokenBucket` | Redis 失败时走进程内令牌桶 |
| `FailOpen` | Redis 失败时直接放行 |
| `FailClosed` | Redis 失败时直接拒绝 |

## 6. 第四阶段：RedisPool

### 6.1 为什么需要 RedisPool

每次请求都新建 Redis 连接会带来：

- TCP 建连开销。
- Redis AUTH / SELECT DB 开销。
- 高并发下大量连接抖动。
- 请求延迟上升。

连接池复用连接，可以降低请求路径开销。

### 6.2 `RedisConnection`

职责：

- 持有 `redisContext*`。
- 析构时释放连接。
- 禁用拷贝，允许移动。
- 执行 Redis 命令。
- 在连接失效时尝试重连。

为什么禁用拷贝：

> `redisContext*` 是独占资源，拷贝会导致多个对象管理同一个指针，容易 double free。

为什么允许移动：

> 连接需要从池队列移动到调用方，再移动回池里。移动语义能表达所有权转移。

### 6.3 `RedisReplyPtr`

Redis reply 需要 `freeReplyObject` 手动释放。项目用：

```cpp
using RedisReplyPtr = std::unique_ptr<redisReply, void (*)(redisReply*)>;
```

这就是 RAII。好处：

- 正常路径自动释放。
- 异常路径自动释放。
- 不需要每个 return 分支手写 free。

### 6.4 `RedisConnectionGuard`

构造时：

```text
pool.acquire()
```

析构时：

```text
pool.release()
```

作用：

- 避免忘记归还连接。
- 异常路径也能归还。
- 业务代码更简单。

### 6.5 连接池线程安全

共享状态：

- 空闲连接队列。
- 连接计数。
- active 连接数。
- wait 计数。
- shutdown 标志。

保护方式：

- `mutex` 保护队列。
- `condition_variable` 等待可用连接。
- `atomic` 维护统计和关闭状态。

### 6.6 health_check

健康检查会：

- 取出空闲连接。
- 在锁外执行 `PING`。
- 健康连接放回池。
- 不健康连接丢弃并补充新连接。

关键点：

> 不要持有连接池锁执行网络 IO，否则业务线程 acquire 连接会被健康检查阻塞。

### 6.7 维护线程

连接池有后台维护线程，定期检查连接健康和补足连接数。

析构时：

- 设置 shutdown。
- notify 维护线程。
- join 线程。

面试可说：

> 如果维护线程只是 sleep，析构可能等待很久；使用 condition_variable 可以更快响应 shutdown。

## 7. 第五阶段：Redis Lua 原子性

### 7.1 为什么不能用多条命令

错误方式：

```text
GET tokens
业务端判断
SET tokens
```

并发问题：

```text
请求 A 看到 tokens=1
请求 B 看到 tokens=1
A 放行并扣减
B 也放行并扣减
实际超发
```

正确方式：

```text
Redis Lua:
  读取状态
  计算补充/窗口
  判断是否允许
  扣减或插入
  设置 TTL
  返回结果
```

Redis 执行 Lua 时是原子的，不会被其他命令插入。

### 7.2 Lua 原子性是不是数据库事务

不是完整事务。Redis Lua 的含义是：

- 脚本执行期间不会被其他命令打断。
- 其他客户端看不到中间状态。
- 但如果脚本内部发生运行错误，已经执行的写命令不一定像数据库事务那样全部回滚。

所以脚本要保持：

- 短。
- 简单。
- 可控。
- 不做复杂长循环。

### 7.3 Redis TIME

为什么不用业务机器时间：

- 多实例机器时钟可能不一致。
- 容器、虚拟机、NTP 抖动会影响限流。
- 令牌补充和窗口判断依赖时间。

使用 Redis `TIME`：

```text
所有实例基于同一个 Redis 时间源
```

### 7.4 SCRIPT LOAD + EVALSHA

流程：

```text
首次执行
  -> SCRIPT LOAD
  -> 缓存 SHA
  -> EVALSHA

后续执行
  -> EVALSHA

Redis 重启 / 脚本缓存丢失
  -> NOSCRIPT
  -> SCRIPT LOAD
  -> EVALSHA
```

好处：

- 减少每次请求传完整脚本的网络开销。
- 脚本内容由 Redis 缓存。

### 7.5 SHA 缓存并发设计

项目里脚本 SHA 使用 `shared_ptr<const string>` 缓存，并通过 atomic load/store 获取。

正常路径：

```text
atomic_load sha
EVALSHA
```

只有加载或重载脚本时：

```text
script_mutex
SCRIPT LOAD
atomic_store sha
```

这样避免所有请求都被脚本锁串行化。

## 8. 第六阶段：令牌桶

### 8.1 令牌桶要解决什么

令牌桶控制平均速率，同时允许短时间突发。

例子：

```text
max_tokens = 100
refill_rate = 20/s
```

含义：

- 桶最多存 100 个令牌。
- 每秒补 20 个。
- 请求来时扣令牌。
- 令牌够就放行，不够就拒绝。

### 8.2 Redis 存储结构

使用 Redis HASH：

```text
key = tokenbucket:<business-key>
fields:
  tokens  = 当前令牌数
  last_ms = 上次补充时间
```

### 8.3 核心公式

```text
elapsed = now_ms - last_ms
tokens = min(capacity, tokens + elapsed * refill_per_ms)

if tokens >= requested:
  allowed = 1
  if consume:
    tokens -= requested
else:
  allowed = 0
```

其中：

```text
refill_per_ms = refill_rate / 1000.0
```

### 8.4 retry_after 怎么算

如果拒绝：

```text
缺少令牌 = requested - tokens
retry_after_ms = 缺少令牌 / refill_per_ms
```

这可以作为 HTTP `Retry-After` 或 JSON 字段返回给客户端。

### 8.5 TTL 怎么算

令牌桶状态是临时状态。脚本设置 TTL，通常按桶填满时间的 2 倍计算，并设置最小值。

目的：

- 冷 key 自动清理。
- 控制 Redis 内存。
- 不影响活跃 key。

### 8.6 令牌桶适合哪些业务

适合：

- 登录。
- 注册。
- 短信验证码。
- 下单。
- API 平均 QPS。
- 允许突发但限制长期速率的接口。

## 9. 第七阶段：滑动窗口

### 9.1 滑动窗口要解决什么

滑动窗口限制：

```text
最近 N 毫秒内最多 M 次
```

比如：

```text
60 秒最多 5 次短信验证码
```

### 9.2 Redis 存储结构

使用 Redis ZSET：

```text
key = ratelimit:<business-key>
score = 请求时间 now_ms
member = 唯一请求 ID
```

### 9.3 Lua 流程

```text
now_ms = Redis TIME
min_score = now_ms - window_ms
ZREMRANGEBYSCORE key 0 min_score
current = ZCARD key
if current + cost <= limit:
  ZADD key now_ms member
  PEXPIRE key window_ms
  allowed = 1
else:
  allowed = 0
```

### 9.4 为什么 member 要唯一

ZSET 的 member 唯一。如果多个请求 member 相同，会覆盖，导致计数不准。

项目使用：

- 进程随机 nonce。
- cost。
- thread id hash。
- 原子递增 sequence。

组合成唯一 member。

### 9.5 滑动窗口成本

每次允许请求会写入一条 ZSET member。

高 QPS + 长窗口时：

- ZSET 元素多。
- 内存高。
- 清理成本高。

因此滑动窗口适合严格窗口场景，令牌桶更适合高频平均速率控制。

## 10. 第八阶段：fallback

### 10.1 为什么需要 fallback

Redis 是限流状态中心。如果 Redis 不可用，服务有三种选择：

- 全部放行。
- 全部拒绝。
- 单机本地限流。

不同业务风险不同，所以不能写死一种策略。

### 10.2 `ResilientTokenBucketLimiter`

它包装远端 Redis `TokenBucketLimiter`。

正常：

```text
remote_limiter.allow
  -> Redis
  -> Healthy result
```

异常：

```text
remote failed
  -> redis_error_count++
  -> fallback_hit_count++
  -> fallback_result
```

### 10.3 三种策略

| 策略 | 行为 | 优点 | 缺点 |
| --- | --- | --- | --- |
| LocalTokenBucket | Redis 挂了走本地令牌桶 | 可用且有单机保护 | 多实例全局配额不准确 |
| FailOpen | Redis 挂了直接放行 | 业务可用性最高 | 失去限流保护 |
| FailClosed | Redis 挂了直接拒绝 | 保护下游和风控 | 误伤正常请求 |

### 10.4 默认为什么推荐 LocalTokenBucket

它是折中：

- 比 FailOpen 更安全。
- 比 FailClosed 更可用。
- Redis 故障时至少单机不裸奔。

但必须主动承认：

> Local fallback 不能继续保证多实例全局配额。

### 10.5 `backend_status`

业务方可以通过 `backend_status` 判断结果来源：

```text
Healthy   -> Redis 正常
Fallback  -> 降级结果
Unavailable -> Redis 不可用且没有可用结果
```

这适合写日志、metrics、告警和响应字段。

## 11. 第九阶段：pybind11

### 11.1 pybind11 做什么

把 C++ 类暴露成 Python 模块：

```python
import redis_limiter

cfg = redis_limiter.RedisConfig()
pool = redis_limiter.RedisPool(cfg)
limiter = redis_limiter.TokenBucketLimiter(pool, 100, 20.0)
result = limiter.allow("user:42")
```

绑定文件：

- [src/python_binding.cpp](../src/python_binding.cpp)

### 11.2 暴露了哪些类

- `RedisConfig`
- `PoolStats`
- `RedisPool`
- `RateLimitConfig`
- `RateLimitResult`
- `BackendStatus`
- `FallbackMode`
- `SlidingWindowLimiter`
- `TokenBucketLimiter`
- `LocalTokenBucketLimiter`
- `ResilientTokenBucketLimiter`
- `RateLimiterFactory`

### 11.3 为什么释放 GIL

Redis 调用是阻塞 IO。Python 多线程服务中，如果 C++ 扩展持有 GIL 等 Redis，其他 Python 线程会受影响。

项目在阻塞调用上使用：

```cpp
py::call_guard<py::gil_scoped_release>()
```

这样等待 Redis 响应时释放 GIL。

### 11.4 释放 GIL 是否一定提升性能

不一定。

取决于：

- Python 服务是多线程还是多进程。
- Redis 延迟。
- FastAPI 是否在线程池里执行同步函数。
- 业务是否有其他 Python 线程需要运行。

但这是正确的绑定层设计。

### 11.5 Python 包装的边界

当前还不是完整 Python 包发布形态。后续可以补：

- `pyproject.toml`
- wheel 构建
- manylinux
- 版本号
- pip install
- 类型提示 `.pyi`

## 12. 第十阶段：FastAPI Demo

### 12.1 为什么要做 FastAPI Demo

因为单纯写一个限流库，面试官可能会问：

```text
这个组件怎么接业务？
怎么返回 429？
怎么记录 metrics？
Redis 挂了接口怎么表现？
```

FastAPI Demo 证明它能接入真实 HTTP 业务链路。

### 12.2 API

| API | 作用 |
| --- | --- |
| `GET /healthz` | 健康检查 |
| `POST /rate-limit/check` | 通用限流检查 |
| `POST /orders` | 下单前限流 |
| `POST /sms/send-code` | 短信验证码多维限流 |
| `GET /metrics` | Prometheus 指标 |

### 12.3 短信验证码链路

```text
POST /sms/send-code
  -> phone_per_minute
  -> user_per_hour
  -> ip_per_minute
  -> 任一拒绝返回 429
  -> 全部通过调用 FakeSmsGateway
  -> 返回 message_id
```

默认规则：

| 维度 | 默认 |
| --- | --- |
| 手机号 | 约 60 秒 1 次 |
| 用户 | 约 1 小时 5 次 |
| IP | 约 1 分钟 20 次 |

### 12.4 多维规则是不是原子

不是。当前 demo 是顺序检查。

问题：

```text
phone 扣减成功
user 拒绝
phone 配额已经消耗
```

防刷场景通常可以接受，因为失败请求也消耗风险预算。

如果要严格 all-or-nothing：

- 写一个多 key Lua 脚本。
- Redis Cluster 下用 hash tag 保证 key 同 slot。
- 或把多维状态聚合到同一个 key。

### 12.5 metrics

`/metrics` 暴露：

- 请求数。
- 允许数。
- 拒绝数。
- Redis 错误数。
- fallback 次数。
- 下游调用次数。
- 请求耗时累计。
- Redis 健康状态。
- fallback mode。

这说明项目不只做功能，还考虑可观测性。

## 13. 第十一阶段：测试体系

### 13.1 功能验证

文件：

- [tests/verify_functionality.py](../tests/verify_functionality.py)

验证：

- Redis 正常时令牌桶限流。
- Redis 不可用时 fallback。
- `backend_status` 是否正确。

命令：

```bash
docker compose run --rm test remote
docker compose run --rm -e REDIS_HOST=redis-unavailable test fallback
```

### 13.2 pytest

文件：

- [tests/test_integration.py](../tests/test_integration.py)

覆盖：

- Python binding。
- Redis token bucket。
- fallback。
- FastAPI `/rate-limit/check`。
- `/metrics`。
- `/sms/send-code` 手机号限流。
- IP 限流。
- Redis unavailable fallback。

命令：

```bash
docker compose run --rm pytest
```

### 13.3 smoke

文件：

- [tests/smoke_docker.py](../tests/smoke_docker.py)

覆盖 Docker app：

- `/healthz`
- `/rate-limit/check`
- `/sms/send-code`
- `/metrics`

命令：

```bash
docker compose run --rm smoke
```

### 13.4 benchmark

文件：

- [tests/benchmark.py](../tests/benchmark.py)

两种模式：

| 模式 | 作用 |
| --- | --- |
| throughput | 测吞吐和延迟 |
| effectiveness | 测是否超发 |

有效性模式重点：

```text
theoretical_allowed = max_tokens + duration * refill_rate
over_issued = actual_allowed - theoretical_allowed
```

报告里热点 key 严格有效性压测：

```text
理论放行 45
实际放行 45
over_issued = 0
```

### 13.5 为什么不能只看 QPS

QPS 高不一定代表限流正确。

还要看：

- 是否超发。
- 是否有错误。
- 拒绝比例是否符合预期。
- Redis 是否健康。
- P95/P99 延迟。
- 是否是短压测快照。

## 14. 第十二阶段：Prometheus / Grafana

### 14.1 Prometheus

配置：

- [prometheus/prometheus.yml](../prometheus/prometheus.yml)

作用：

- 抓取 FastAPI `/metrics`。
- 观察 allowed / denied / fallback / Redis health。

### 14.2 Grafana

配置：

- [grafana/provisioning/datasources/prometheus.yml](../grafana/provisioning/datasources/prometheus.yml)
- [grafana/provisioning/dashboards/dashboard.yml](../grafana/provisioning/dashboards/dashboard.yml)
- [grafana/dashboards/redis-rate-limiter-dashboard.json](../grafana/dashboards/redis-rate-limiter-dashboard.json)

作用：

- 自动配置数据源。
- 自动加载 dashboard。
- 展示限流请求、拒绝、fallback、Redis health。

### 14.3 面试怎么讲

> 当前 metrics 是 Demo 层指标，不是完整中间件级观测体系。它证明了可观测性链路能打通，后续生产化要把 C++ core 的连接池状态、Redis 错误、Lua 延迟等也纳入统一 metrics。

## 15. 生产化边界

### 15.1 当前不是完整限流平台

缺少：

- Redis Sentinel / Cluster。
- 配置中心。
- 动态规则下发。
- 多租户隔离。
- 管理后台。
- 灰度发布。
- 多语言 SDK。
- HTTP/gRPC 限流服务。
- 完整中间件 metrics。
- 长时间压测和故障演练。

### 15.2 单 Redis 边界

单 Redis 问题：

- 单点故障。
- 热点 key 瓶颈。
- 单线程执行 Lua。
- 网络延迟影响请求。

演进：

- Sentinel。
- Cluster。
- 云 Redis。
- key 分片。
- 本地预限流。

### 15.3 本地 fallback 边界

本地 fallback 只能保证：

```text
单实例限流
```

不能保证：

```text
多实例全局配额
```

所以它是 Redis 故障时的降级保护，不是分布式限流的等价替代。

### 15.4 多语言接入

当前：

- C++ core。
- Python binding。

未来：

- HTTP/gRPC service。
- Go SDK。
- Java SDK。
- Node SDK。
- sidecar 模式。

### 15.5 动态规则

当前规则通过代码或环境变量配置。

生产化可以做：

- 规则表。
- 配置中心。
- 热更新。
- 灰度规则。
- 租户级限流。
- 用户级限流。
- 黑白名单。

## 16. 学习顺序安排

### 第 1 天：跑项目

目标：

- 能构建 C++ core。
- 能构建 Python 扩展。
- 能跑 Docker Compose。

命令：

```bash
cmake -S . -B build-core -DREDIS_LIMITER_BUILD_PYTHON=OFF
cmake --build build-core --parallel
docker compose config --quiet
```

### 第 2 天：读公共 API

看：

- [include/redis_pool.hpp](../include/redis_pool.hpp)
- [include/sliding_window_limiter.hpp](../include/sliding_window_limiter.hpp)

目标：

- 知道每个类负责什么。
- 能画出类之间关系。

### 第 3 天：读 RedisPool

看：

- [src/redis_pool.cpp](../src/redis_pool.cpp)

目标：

- 理解 RAII。
- 理解连接复用。
- 理解超时、认证、DB、重连、健康检查。

### 第 4 天：读令牌桶和滑动窗口

看：

- [src/sliding_window_limiter.cpp](../src/sliding_window_limiter.cpp)

目标：

- 逐行理解两个 Lua 脚本。
- 能写出令牌桶公式。
- 能解释 ZSET 滑动窗口成本。

### 第 5 天：读 fallback

看：

- `LocalTokenBucketLimiter`
- `ResilientTokenBucketLimiter`

目标：

- 能解释三种 fallback。
- 能解释 `backend_status`。
- 能解释本地 fallback 边界。

### 第 6 天：读 pybind11 和 FastAPI

看：

- [src/python_binding.cpp](../src/python_binding.cpp)
- [examples/fastapi_demo.py](../examples/fastapi_demo.py)

目标：

- 能解释 C++ 怎么暴露给 Python。
- 能解释短信验证码多维限流。
- 能解释 metrics。

### 第 7 天：跑测试和压测

看：

- [tests/test_integration.py](../tests/test_integration.py)
- [tests/benchmark.py](../tests/benchmark.py)
- [reports/benchmark-report.md](../reports/benchmark-report.md)

目标：

- 能解释每个测试验证什么。
- 能解释 `over_issued=0` 的意义。
- 能主动说明压测边界。

## 17. 读代码方法

### 17.1 不要这样读

不要：

- 从第一个源文件读到最后一个。
- 只背令牌桶概念，不看 Redis Lua。
- 只看 FastAPI，不看 C++ core。
- 只看成功路径，不看 fallback。
- 只背 QPS，不看有效性断言。

### 17.2 应该这样读

按问题读：

```text
问题：为什么不会超发？
  -> Lua 原子性
  -> TokenBucket Lua
  -> effectiveness benchmark
```

按链路读：

```text
Python allow()
  -> pybind11
  -> C++ TokenBucketLimiter
  -> RedisPool
  -> EVALSHA
  -> Lua
  -> RateLimitResult
```

按故障读：

```text
Redis unavailable
  -> remote limiter fails
  -> ResilientTokenBucketLimiter
  -> LocalTokenBucket / FailOpen / FailClosed
  -> backend_status=Fallback
```

### 17.3 每个模块问 6 个问题

```text
1. 这个模块负责什么？
2. 输入是什么？
3. 输出是什么？
4. 依赖什么下游？
5. 并发或故障风险是什么？
6. 用什么测试证明它有效？
```

## 18. 面试主线

### 18.1 30 秒介绍

```text
Redis-Limiter 是我实现的可复用分布式限流组件，主要解决多实例服务中本地限流配额不共享的问题。核心用 C++17 和 hiredis 封装 Redis 连接池、令牌桶和滑动窗口，限流状态更新通过 Redis Lua 原子完成，并使用 Redis TIME 做统一时间源。组件可以被 C++ 服务直接链接，也可以通过 pybind11 给 Python 服务使用。项目还提供 FastAPI 短信验证码多维限流 demo、Redis 故障降级、Prometheus 指标、Docker 测试和 benchmark 有效性验证。
```

### 18.2 如果只能讲一个点

优先讲 Redis Lua 原子限流：

```text
本地限流多实例会放大配额
Redis 做共享状态
GET/SET 多命令会竞态
Lua 把读取、补充、扣减、TTL 合成原子脚本
Redis TIME 统一时间源
SCRIPT LOAD/EVALSHA 降低传输
effectiveness benchmark 验证 over_issued=0
```

### 18.3 如果面试官偏 C++

讲：

```text
hiredis 封装
RedisConnection RAII
RedisReplyPtr RAII
禁用拷贝允许移动
RedisPool mutex/condition_variable
连接健康检查
脚本 SHA 原子缓存
pybind11 GIL release
```

### 18.4 如果面试官偏后端

讲：

```text
短信验证码手机号/用户/IP 多维限流
Redis 故障 fallback
429 响应 retry_after
metrics
Docker Compose
pytest/smoke/benchmark
生产化边界
```

## 19. 最后检查清单

面试前确认你能回答：

- [ ] 为什么本地内存限流多实例会失效？
- [ ] Redis 为什么适合做限流状态中心？
- [ ] 为什么必须用 Lua？
- [ ] Lua 原子性和数据库事务有什么区别？
- [ ] 为什么用 Redis TIME？
- [ ] 令牌桶公式是什么？
- [ ] 滑动窗口为什么用 ZSET？
- [ ] 令牌桶和滑动窗口怎么选？
- [ ] `SCRIPT LOAD + EVALSHA` 怎么工作？
- [ ] `NOSCRIPT` 怎么处理？
- [ ] RedisPool 为什么需要 RAII？
- [ ] `allow()` 是否幂等？
- [ ] Redis 挂了怎么办？
- [ ] Local fallback 的边界是什么？
- [ ] pybind11 为什么释放 GIL？
- [ ] FastAPI 短信验证码三维限流怎么做？
- [ ] 多维规则是不是原子？
- [ ] benchmark 的 `over_issued=0` 说明什么？
- [ ] 这个项目为什么不是完整生产级限流平台？
- [ ] 后续怎么做 Redis HA、动态规则、多语言接入？

