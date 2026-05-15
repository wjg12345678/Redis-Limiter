# Redis-Limiter

![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)
![Redis](https://img.shields.io/badge/Redis-Lua%20%2B%20TIME-dc2626)
![Python](https://img.shields.io/badge/Python-pybind11-2563eb)
![FastAPI](https://img.shields.io/badge/Demo-FastAPI-059669)
![Docker](https://img.shields.io/badge/Verify-Docker%20Compose-7c3aed)

Redis-Limiter 是一个基于 `C++17 + hiredis + Redis Lua + pybind11` 的分布式限流组件。它的核心定位是 **可复用限流 SDK / 基础组件**，不是某个业务项目里的内嵌代码，也不是必须独立部署的限流平台。

项目提供两种接入方式：

- C++ 服务直接链接 `redis_limiter::core`
- Python 服务通过 `redis_limiter` 扩展模块调用

业务项目只需要在请求进入核心逻辑前构造限流 key，调用 `allow()` 判断是否放行。限流状态存储在 Redis 中，多实例共享同一份配额；Redis 异常时可以按配置切换本地令牌桶、fail-open 或 fail-closed。

## 目录

- [项目定位](#项目定位)
- [核心能力](#核心能力)
- [整体架构](#整体架构)
- [模块分层](#模块分层)
- [限流算法](#限流算法)
- [Redis 原子性设计](#redis-原子性设计)
- [故障降级](#故障降级)
- [C++ 接入](#c-接入)
- [Python 接入](#python-接入)
- [FastAPI Demo](#fastapi-demo)
- [Docker 验证](#docker-验证)
- [测试与压测](#测试与压测)
- [监控与指标](#监控与指标)
- [与 Atlas 的关系](#与-atlas-的关系)
- [生产化边界](#生产化边界)
- [简历与面试讲法](#简历与面试讲法)
- [文档索引](#文档索引)

## 项目定位

很多后端接口都需要限流，例如登录、注册、短信验证码、下单、支付、评论、上传等。单机内存限流只能保护当前进程，一旦服务多实例部署，每个实例都会维护自己的计数，最终总流量可能被实例数放大。

Redis-Limiter 解决的是这个问题：

```text
多实例服务
  -> 共享 Redis 中的限流状态
  -> 通过 Lua 脚本原子检查和扣减额度
  -> 返回 allowed / remaining / retry_after / backend_status
```

它适合放在业务服务内部作为 SDK 使用：

```text
HTTP / RPC 请求
  -> 业务服务计算限流 key
  -> Redis-Limiter allow()
  -> allowed: 执行业务逻辑
  -> denied: 返回 429 / 风控失败 / 稍后重试
```

是否需要单独部署取决于接入形态：

| 接入形态 | 是否单独部署 Redis-Limiter | 说明 |
| --- | --- | --- |
| C++ SDK | 不需要 | 业务进程直接链接 `redis_limiter::core` |
| Python 扩展 | 不需要 | 业务进程直接 `import redis_limiter` |
| FastAPI Demo | 可选 | Demo 可以作为 HTTP 化示例，但不是组件唯一形态 |
| 未来 HTTP/gRPC 网关 | 需要 | 如果要给任意语言调用，可以再封装成独立限流服务 |

推荐在简历中把它写成：

```text
Redis-Limiter｜分布式限流组件

基于 C++17、hiredis、Redis Lua 和 pybind11 实现的可复用分布式限流组件，支持令牌桶、滑动窗口、Redis TIME 统一时间源、SCRIPT LOAD/EVALSHA 脚本缓存、连接池、故障降级、Python/FastAPI 接入、Docker Compose 验证、Prometheus 指标和压测报告。
```

## 核心能力

| 能力 | 说明 |
| --- | --- |
| 多实例共享配额 | 限流状态放在 Redis，同一 key 在多个服务实例间共享 |
| 令牌桶 | 适合平均速率控制，允许短时突发 |
| 滑动窗口 | 适合严格控制一段时间窗口内的请求数 |
| Redis Lua 原子性 | 读取状态、清理窗口、补充令牌、扣减额度、设置 TTL 在同一个脚本内完成 |
| Redis TIME | 使用 Redis 时间作为统一时间源，降低多机器时钟漂移影响 |
| SCRIPT LOAD/EVALSHA | 脚本加载后优先通过 SHA 执行，减少重复传输 Lua 脚本 |
| Redis 连接池 | 复用 hiredis 连接，支持超时、重连、健康检查、统计信息 |
| 故障降级 | Redis 不可用时支持本地令牌桶、fail-open、fail-closed |
| C++ core | 可直接链接到 C++ 服务，target 名为 `redis_limiter::core` |
| Python binding | 通过 pybind11 暴露 Python 模块，阻塞 Redis 调用释放 GIL |
| Demo 与验证 | FastAPI 示例、Docker Compose、pytest、smoke、benchmark、Prometheus/Grafana |

## 整体架构

```text
C++ service / Python service / FastAPI demo
        |
        v
Redis-Limiter public API
  RedisConfig
  RedisPool
  SlidingWindowLimiter
  TokenBucketLimiter
  ResilientTokenBucketLimiter
        |
        v
hiredis connection pool
        |
        v
Redis Lua script
  TIME
  ZSET / HASH
  PEXPIRE
  atomic allow / deny
        |
        v
Redis
```

正常请求链路：

```text
request
  -> build rate-limit key
  -> limiter.allow(key, cost)
  -> acquire Redis connection
  -> SCRIPT LOAD if needed
  -> EVALSHA Lua
  -> RateLimitResult
  -> business allow / reject
```

Redis 异常链路：

```text
request
  -> ResilientTokenBucketLimiter
  -> remote TokenBucketLimiter throws or returns unavailable
  -> fallback strategy
       |-- LocalTokenBucket
       |-- FailOpen
       `-- FailClosed
  -> RateLimitResult.backend_status = Fallback
```

## 模块分层

```text
.
|-- include/
|   |-- redis_pool.hpp              # Redis 配置、连接、连接池、统计
|   `-- sliding_window_limiter.hpp  # 限流结果、算法、fallback、工厂
|-- src/
|   |-- redis_pool.cpp              # hiredis 连接、认证、DB 选择、健康检查
|   |-- sliding_window_limiter.cpp  # Lua 脚本、令牌桶、滑动窗口、降级逻辑
|   `-- python_binding.cpp          # pybind11 Python 扩展
|-- examples/
|   |-- python_demo.py              # 普通 Python 业务调用示例
|   `-- fastapi_demo.py             # HTTP API、短信验证码、多维限流、metrics
|-- tests/
|   |-- verify_functionality.py     # 功能验证和 Redis 故障降级验证
|   |-- test_integration.py         # pytest 集成测试
|   |-- smoke_docker.py             # Docker HTTP 链路冒烟
|   `-- benchmark.py                # 吞吐和有效性压测
|-- prometheus/                     # Prometheus 抓取配置
|-- grafana/                        # Grafana datasource / dashboard provisioning
|-- reports/                        # 压测报告
|-- docs/                           # 简历材料、面试讲稿和面试题库
|-- CMakeLists.txt
|-- Dockerfile
`-- docker-compose.yml
```

主要类：

| 类 / 结构 | 作用 |
| --- | --- |
| `rrl::RedisConfig` | Redis host、port、password、db、超时、连接池大小、重试参数 |
| `rrl::RedisConnection` | hiredis 连接 RAII 封装 |
| `rrl::RedisPool` | Redis 连接池，支持 acquire/release、health_check、resize、stats |
| `rrl::RateLimitConfig` | 滑动窗口配置：最大请求数、窗口长度、key 前缀 |
| `rrl::RateLimitResult` | 限流结果：是否放行、剩余额度、重试时间、后端状态 |
| `rrl::SlidingWindowLimiter` | Redis ZSET 滑动窗口限流 |
| `rrl::TokenBucketLimiter` | Redis HASH 令牌桶限流 |
| `rrl::LocalTokenBucketLimiter` | 进程内本地令牌桶 |
| `rrl::ResilientTokenBucketLimiter` | 带 Redis 故障降级的令牌桶包装器 |
| `rrl::RateLimiterFactory` | 创建限流器的工厂方法 |

## 限流算法

### 令牌桶

令牌桶适合控制平均速率，同时允许短时间突发。

示例：

```text
capacity = 100
refill_rate = 20 tokens/s
request cost = 1
```

实现方式：

- Redis `HASH` 保存当前令牌数和上次补充时间。
- 每次请求使用 Redis `TIME` 计算距离上次补充经过了多久。
- 按 `refill_rate` 补充令牌，最多补到 `capacity`。
- 如果令牌足够，扣减并放行；否则拒绝并返回建议重试时间。
- 更新后的状态和 TTL 在 Lua 脚本里一次提交。

适用场景：

- 登录 / 注册接口防刷
- 短信验证码发送频率限制
- API 平均 QPS 控制
- 允许一定突发但要长期平滑的接口

### 滑动窗口

滑动窗口适合严格控制一段时间内最多允许多少次请求。

示例：

```text
window_size_ms = 1000
max_requests = 100
含义：任意 1 秒内最多 100 次
```

实现方式：

- Redis `ZSET` 保存请求时间戳。
- 每次请求先删除窗口外的旧记录。
- 使用 `ZCARD` 统计窗口内请求数。
- 如果 `current + cost <= limit`，插入本次请求记录并放行。
- 使用 `PEXPIRE` 设置窗口 TTL，避免冷 key 长期保留。

适用场景：

- 严格窗口限额
- 审计、风控、接口配额
- 需要解释“最近 N 秒内最多 M 次”的规则

## Redis 原子性设计

限流最容易出错的是并发读写。例如两个请求同时看到剩余 1 个额度，如果检查和扣减不是原子的，就可能同时放行，导致超发。

本项目使用 Redis Lua 解决这个问题：

```text
Lua script
  -> read current state
  -> compute refill / cleanup
  -> check limit
  -> consume quota
  -> set TTL
  -> return result
```

Redis 单线程执行单个 Lua 脚本，因此同一个 key 的检查和扣减不会被其他请求插入打断。

脚本执行策略：

- 首次执行前通过 `SCRIPT LOAD` 加载 Lua 脚本。
- 后续优先使用 `EVALSHA` 执行缓存脚本。
- 如果 Redis 返回 `NOSCRIPT`，重新加载脚本并重试。
- 时间源使用 Redis `TIME`，而不是调用方机器本地时间。

关于幂等性：

- `peek()` 是只查看配额，不消耗额度，可以视为读语义。
- `reset()` 是显式重置指定 key，调用方需要保证业务上允许。
- `allow()` 是检查并扣减额度的写操作，本身不是幂等接口；同一个业务请求重复调用会重复消耗额度。
- 如果业务层存在重试，需要在业务层引入 request id / 幂等表 / 去重 key，避免同一业务动作重复扣减。

关于重试：

- Redis 连接建立和重连可以按 `RedisConfig.max_retries` 重试。
- 限流写脚本发出后，如果网络异常导致客户端拿不到响应，无法确定 Redis 是否已经扣减额度，因此实现不会盲目重放已经发出的写操作。
- 这个取舍是为了避免“客户端重试导致额外扣额度”的隐性错误。

## 故障降级

`ResilientTokenBucketLimiter` 是推荐的工程接入形态。它包装远端 Redis 令牌桶，在 Redis 异常时按策略降级。

| 策略 | 行为 | 适用场景 | 代价 |
| --- | --- | --- | --- |
| `LocalTokenBucket` | Redis 失败时切到进程内令牌桶 | 默认推荐，兼顾可用性和保护能力 | 多实例全局配额不再严格一致 |
| `FailOpen` | Redis 失败时直接放行 | 核心链路可用性优先 | Redis 故障期间基本失去限流 |
| `FailClosed` | Redis 失败时直接拒绝 | 风控、安全、成本保护优先 | Redis 故障会影响业务可用性 |

返回结果中的 `backend_status` 用于区分当前状态：

| 状态 | 含义 |
| --- | --- |
| `Healthy` | Redis 正常，结果来自分布式限流 |
| `Unavailable` | Redis 不可用，且没有可用降级结果 |
| `Fallback` | 结果来自降级策略 |

包装器还提供：

- `redis_error_count()`
- `fallback_hit_count()`
- `fallback_mode()`
- `update_fallback_mode()`

这些字段适合接入日志、指标和告警。

## C++ 接入

### 构建 C++ core

依赖：

- CMake 3.14+
- C++17 compiler
- hiredis
- pthread

Ubuntu 示例：

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake libhiredis-dev
```

只构建 C++ core：

```bash
cmake -S . -B build -DREDIS_LIMITER_BUILD_PYTHON=OFF
cmake --build build --parallel
```

### 作为子项目接入

推荐目录：

```text
workspace/
|-- YourServer/
`-- Redis-Limiter/
```

业务项目的 `CMakeLists.txt`：

```cmake
add_subdirectory(/path/to/Redis-Limiter redis_limiter)

add_executable(my_server main.cpp)
target_link_libraries(my_server PRIVATE redis_limiter::core)
```

如果使用相对路径：

```cmake
add_subdirectory(../Redis-Limiter redis_limiter)
target_link_libraries(my_server PRIVATE redis_limiter::core)
```

### C++ 示例

```cpp
#include "redis_pool.hpp"
#include "sliding_window_limiter.hpp"

#include <iostream>
#include <memory>

int main() {
    rrl::RedisConfig redis;
    redis.host = "127.0.0.1";
    redis.port = 6379;
    redis.pool_size = 8;
    redis.connect_timeout_ms = 200;
    redis.socket_timeout_ms = 200;
    redis.max_retries = 1;

    auto pool = std::make_shared<rrl::RedisPool>(redis);
    auto remote = std::make_shared<rrl::TokenBucketLimiter>(
        pool,
        100,       // max_tokens
        20.0,      // refill_rate tokens/s
        "login:"   // key_prefix
    );

    rrl::ResilientTokenBucketLimiter limiter(
        remote,
        rrl::FallbackMode::LocalTokenBucket,
        50,
        5.0
    );

    auto result = limiter.allow("user:42");
    if (!result.allowed) {
        std::cout << "rate limited, retry_after_ms="
                  << result.retry_after_ms << "\n";
        return 1;
    }

    std::cout << "allowed, remaining=" << result.remaining << "\n";
    return 0;
}
```

## Python 接入

### 构建 Python 扩展

依赖：

- Python 3
- pybind11
- hiredis

```bash
python3 -m pip install -r requirements.txt

cmake -S . -B build \
  -DREDIS_LIMITER_BUILD_PYTHON=ON \
  -Dpybind11_DIR="$(python3 -c 'import pybind11; print(pybind11.get_cmake_dir())')"
cmake --build build --parallel
```

构建完成后会得到 Python 可导入模块：

```text
build/redis_limiter*.so
```

本地运行示例时可以设置：

```bash
PYTHONPATH=build python3 examples/python_demo.py
```

### Python 令牌桶示例

```python
import redis_limiter

cfg = redis_limiter.RedisConfig()
cfg.host = "127.0.0.1"
cfg.port = 6379
cfg.pool_size = 8
cfg.connect_timeout_ms = 200
cfg.socket_timeout_ms = 200
cfg.max_retries = 1

pool = redis_limiter.RedisPool(cfg)
remote = redis_limiter.TokenBucketLimiter(
    pool,
    max_tokens=100,
    refill_rate=20.0,
    key_prefix="login:",
)

limiter = redis_limiter.ResilientTokenBucketLimiter(
    remote,
    redis_limiter.FallbackMode.LocalTokenBucket,
    50,
    5.0,
)

result = limiter.allow("user:42")
if not result.allowed:
    print("deny", result.retry_after_ms, result.backend_status)
else:
    print("allow", result.remaining, result.backend_status)
```

### Python 滑动窗口示例

```python
import redis_limiter

cfg = redis_limiter.RedisConfig()
cfg.host = "127.0.0.1"
cfg.port = 6379

pool = redis_limiter.RedisPool(cfg)

rate_cfg = redis_limiter.RateLimitConfig()
rate_cfg.max_requests = 10
rate_cfg.window_size_ms = 1000
rate_cfg.key_prefix = "api:"

limiter = redis_limiter.SlidingWindowLimiter(pool, rate_cfg)
result = limiter.allow("ip:127.0.0.1")

print(result.allowed, result.current_count, result.remaining)
```

## FastAPI Demo

`examples/fastapi_demo.py` 演示了把组件接入 HTTP 服务的方式。它不是项目唯一部署方式，但适合展示完整业务链路。

Demo 提供：

| API | 说明 |
| --- | --- |
| `GET /healthz` | 健康检查 |
| `POST /rate-limit/check` | 通用限流检查接口 |
| `POST /orders` | 下单前先限流，再模拟访问下游持久化 |
| `POST /sms/send-code` | 短信验证码手机号 / 用户 / IP 多维限流 |
| `GET /metrics` | Prometheus 风格指标 |

短信验证码示例链路：

```text
POST /sms/send-code
  -> phone_per_minute
  -> user_per_hour
  -> ip_per_minute
  -> 全部通过后调用 FakeSmsGateway
  -> 返回 message_id
```

这里的多维限流是顺序检查，不是 all-or-nothing 事务。如果第一条规则已经扣减成功、第二条规则拒绝，本次请求会被拒绝，但第一条规则的额度不会自动回滚。真实生产中如果需要多维规则强一致，可以把多维规则合并进一个 Lua 脚本，或在业务上接受这种保守扣减。

本地启动：

```bash
PYTHONPATH=build uvicorn examples.fastapi_demo:app --host 0.0.0.0 --port 8000
```

调用：

```bash
curl -sS http://127.0.0.1:8000/healthz

curl -sS -X POST http://127.0.0.1:8000/rate-limit/check \
  -H 'Content-Type: application/json' \
  -d '{"key":"demo:user:1","tokens_needed":1}'

curl -sS -X POST http://127.0.0.1:8000/sms/send-code \
  -H 'Content-Type: application/json' \
  -d '{"phone":"+8613800000000","user_id":"42","scene":"login"}'
```

常用环境变量：

| 环境变量 | 默认值 | 说明 |
| --- | --- | --- |
| `REDIS_HOST` | `127.0.0.1` | Redis host |
| `REDIS_PORT` | `6379` | Redis port |
| `REDIS_POOL_SIZE` | `8` | 连接池大小 |
| `REDIS_CONNECT_TIMEOUT_MS` | `200` | 连接超时 |
| `REDIS_SOCKET_TIMEOUT_MS` | `200` | socket 超时 |
| `REDIS_MAX_RETRIES` | `3` | 连接阶段重试次数 |
| `RATE_LIMIT_MAX_TOKENS` | `20` | Demo 令牌桶容量 |
| `RATE_LIMIT_REFILL_RATE` | `5` | Demo 每秒补充令牌数 |
| `RATE_LIMIT_FALLBACK_MODE` | `LocalTokenBucket` | 降级模式 |
| `LOCAL_MAX_TOKENS` | `10` | 本地降级令牌桶容量 |
| `LOCAL_REFILL_RATE` | `2` | 本地降级每秒补充令牌数 |

## Docker 验证

Docker Compose 会构建 Python 扩展，并启动 Redis、FastAPI Demo、测试、压测、Prometheus 和 Grafana。

构建镜像：

```bash
docker compose build
```

启动 Redis 和 Demo：

```bash
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

监控：

```bash
docker compose up -d prometheus grafana
```

访问：

| 服务 | 地址 |
| --- | --- |
| FastAPI Demo | `http://127.0.0.1:8000` |
| Prometheus | `http://127.0.0.1:9090` |
| Grafana | `http://127.0.0.1:3000`，默认账号 `admin/admin` |

## 测试与压测

当前仓库包含四类验证：

| 文件 / 命令 | 覆盖内容 |
| --- | --- |
| `tests/verify_functionality.py` | Redis 正常限流、Redis 故障降级 |
| `tests/test_integration.py` | Python 绑定、FastAPI API、短信验证码多维限流 |
| `tests/smoke_docker.py` | Docker 中的 `/healthz`、`/rate-limit/check`、`/metrics` |
| `tests/benchmark.py` | 吞吐压测和严格有效性压测 |

压测报告见 [reports/benchmark-report.md](reports/benchmark-report.md)。

当前报告快照：

| 场景 | 结果 |
| --- | --- |
| 独立 key 短压测 | 约 `7.1k QPS` |
| 热点 key 短压测 | 约 `19.5k QPS` |
| 严格有效性压测 | 理论放行 `45`，实际放行 `45` |
| 超发量 | `over_issued=0.00` |
| pytest | `9 passed in 0.53s` |

这些数字只代表当前 Docker 测试环境下的短压测快照，不能直接等同生产容量。它们更适合用于证明：

- Lua 原子扣减没有在热点 key 竞争下出现超发。
- Redis 路径、Python 绑定、FastAPI 示例和 Docker 验证链路是打通的。
- 组件具备基础性能测量和回归验证能力。

## 监控与指标

FastAPI Demo 的 `/metrics` 暴露 Prometheus 风格指标，包括：

| 指标 | 含义 |
| --- | --- |
| `demo_rate_limit_requests_total` | 限流检查请求总数 |
| `demo_rate_limit_allowed_total` | 放行总数 |
| `demo_rate_limit_denied_total` | 拒绝总数 |
| `demo_rate_limit_redis_error_total` | Redis 错误次数 |
| `demo_rate_limit_fallback_total` | 降级次数 |
| `demo_downstream_calls_total` | 下游业务调用次数 |
| `demo_rate_limit_request_duration_seconds_sum` | 限流请求耗时累计 |
| `demo_redis_health` | Redis 健康状态 |
| `demo_fallback_mode` | 当前 fallback 模式 |

配置文件：

- [prometheus/prometheus.yml](prometheus/prometheus.yml)
- [grafana/provisioning/datasources/prometheus.yml](grafana/provisioning/datasources/prometheus.yml)
- [grafana/provisioning/dashboards/dashboard.yml](grafana/provisioning/dashboards/dashboard.yml)
- [grafana/dashboards/redis-rate-limiter-dashboard.json](grafana/dashboards/redis-rate-limiter-dashboard.json)

## 与 Atlas 的关系

Atlas WebServer 和 Redis-Limiter 是两个可以分开写在简历上的项目。

```text
Redis-Limiter
  -> 通用分布式限流组件
  -> C++ core
  -> Python binding
  -> Redis Lua / fallback / metrics / tests / benchmark

Atlas WebServer
  -> C++ Linux 网盘后端
  -> epoll + Reactor + HTTP + MySQL + 文件业务
  -> 登录 / 注册限流业务适配
  -> 通过 CMake 链接外部 Redis-Limiter
```

Atlas 不再复制 `Redis-Limiter` 的源码。Atlas 只保留自己的业务适配层，例如：

- 登录 IP 限流 key 怎么生成
- 登录用户名限流 key 怎么生成
- 注册 IP 限流 key 怎么生成
- 被限流后如何返回 HTTP 429
- Redis 异常时业务采用哪种 fallback 策略

这样两个项目的边界更清楚：

| 项目 | 面试重点 |
| --- | --- |
| Redis-Limiter | Redis Lua 原子性、令牌桶/滑动窗口、连接池、fallback、pybind11、测试压测 |
| Atlas WebServer | epoll/Reactor、HTTP parser、multipart、MySQL 事务、文件一致性、网盘业务 |

## 生产化边界

这个项目可以作为面试项目和轻量基础组件展示，但不要包装成完整生产级限流平台。需要主动承认的边界：

- 默认接入单 Redis，生产需要 Sentinel / Cluster / 云 Redis 高可用。
- 当前规则主要由代码配置，缺少配置中心、动态规则下发、灰度、租户隔离和后台管理台。
- Python 扩展适合 Python 服务接入，但 Java、Go、Node 等语言还需要 HTTP/gRPC 网关或对应语言 SDK。
- 本地 fallback 只能保证单实例保护，Redis 故障期间无法继续保证多实例全局配额。
- 多维限流 Demo 是顺序扣减，不是单脚本 all-or-nothing。
- 当前 metrics 是 Demo 层指标，不是完整中间件级观测体系。
- 压测是短时间 Docker 环境结果，生产容量还需要结合 Redis 拓扑、网络、key 分布和业务延迟重新评估。
- Redis Lua 脚本需要控制复杂度，避免长脚本阻塞 Redis。
- 如果要求跨机房、跨地域限流，还需要考虑一致性、延迟和降级策略。

可以主动说：

```text
这个项目不是完整限流平台，我把重点放在 Redis 原子限流、SDK 接入、故障降级和验证闭环上。生产化继续演进会做 Redis 高可用、动态规则配置、HTTP/gRPC 网关、多语言 SDK、统一指标和管理后台。
```

## 简历与面试讲法

### 30 秒介绍

我做了一个可复用的 Redis 分布式限流组件，主要解决多实例服务里本地限流额度不共享的问题。核心用 C++17 和 hiredis 封装 Redis 连接池、令牌桶和滑动窗口，状态更新通过 Redis Lua 原子完成，并使用 Redis TIME 作为统一时间源。组件既可以被 C++ 服务链接，也可以通过 pybind11 给 Python 服务使用，另外还做了 FastAPI 短信验证码防刷 Demo、Redis 故障降级、Prometheus 指标、Docker 测试和压测验证。

### 推荐简历 bullet

- 基于 `C++17 + hiredis + Redis Lua` 实现可复用分布式限流组件，支持令牌桶、滑动窗口和多实例共享配额，C++ 服务可直接链接 `redis_limiter::core`。
- 使用 Redis Lua 将状态读取、令牌补充、额度扣减和 TTL 设置封装为原子操作，并通过 Redis `TIME` 统一时间源和 `SCRIPT LOAD + EVALSHA` 优化脚本执行。
- 设计 `ResilientTokenBucketLimiter`，支持 Redis 不可用时切换本地令牌桶、fail-open 或 fail-closed，并通过 `backend_status`、`redis_error_count`、`fallback_hit_count` 暴露降级状态。
- 通过 pybind11 提供 Python 扩展模块，接入 FastAPI 短信验证码防刷场景，实现手机号、用户、IP 多维限流，并补充 Docker smoke、pytest、benchmark、Prometheus/Grafana 验证链路。
- 热点 key 严格有效性压测下理论放行 `45` 次、实际放行 `45` 次，`over_issued=0.00`，短压测热点 key 约 `19.5k QPS`。

### 高频问题

| 问题 | 回答要点 |
| --- | --- |
| 为什么不用本地内存限流 | 多实例部署时每个实例各算各的，总额度会被放大 |
| 为什么用 Redis | Redis 可作为共享状态中心，Lua 脚本能保证检查和扣减原子性 |
| 为什么用 Lua | 避免 `GET -> 计算 -> SET` 中间被并发请求插入导致超发 |
| 为什么用 Redis TIME | 避免不同服务机器时钟不一致影响窗口和补充令牌 |
| 令牌桶和滑动窗口区别 | 令牌桶控制平均速率并允许突发，滑动窗口严格限制窗口内次数 |
| Redis 挂了怎么办 | 按业务选择本地令牌桶、fail-open 或 fail-closed |
| allow 是否幂等 | 不是，allow 会消耗额度；业务重试需要上层幂等设计 |
| 为什么不是生产级平台 | 缺少 Redis 高可用、动态规则、管理台、多语言网关、完整观测和容量治理 |

## 文档索引

- [docs/README.md](docs/README.md)：文档导航和阅读顺序
- [docs/project-study-guide-complete.md](docs/project-study-guide-complete.md)：完整学习路线
- [docs/interview-qna-complete.md](docs/interview-qna-complete.md)：面试完整问答
- [docs/interview-guide.md](docs/interview-guide.md)：面试讲稿和高频问答
- [docs/interview-qa.md](docs/interview-qa.md)：完整面试题库和参考答案
- [docs/resume-bullets.md](docs/resume-bullets.md)：简历 bullet 和项目描述
- [docs/integration-guide.md](docs/integration-guide.md)：组件化接入指南
- [docs/production-hardening-roadmap.md](docs/production-hardening-roadmap.md)：生产化加固与平台化路线
- [docs/defense-playbook.md](docs/defense-playbook.md)：面试答辩攻防手册
- [reports/benchmark-report.md](reports/benchmark-report.md)：压测报告
- [reports/benchmark-report.html](reports/benchmark-report.html)：HTML 压测报告
- [examples/python_demo.py](examples/python_demo.py)：普通 Python 调用示例
- [examples/fastapi_demo.py](examples/fastapi_demo.py)：FastAPI 接入示例
- [tests/verify_functionality.py](tests/verify_functionality.py)：功能验证脚本
- [tests/test_integration.py](tests/test_integration.py)：集成测试
- [tests/benchmark.py](tests/benchmark.py)：压测脚本

## License

See [LICENSE](LICENSE).
