# 简历材料：Redis 分布式限流组件

## 项目名称

Redis 分布式限流组件

可选写法：

- Redis 分布式限流组件
- Redis Rate Limiter for Python Services
- 面向 Python 服务的 Redis 分布式限流组件

## 技术栈

`C++17` / `hiredis` / `Redis Lua` / `pybind11` / `Python` / `FastAPI` / `Docker Compose` / `pytest` / `Prometheus` / `Grafana`

## 简历项目描述

面向 Python 后端服务实现 Redis 分布式限流组件，支持令牌桶、滑动窗口、Redis 故障降级和 FastAPI 业务接入，可用于短信验证码、登录、下单等接口防刷场景。

## 推荐简历 Bullet

版本一，偏后端工程：

- 基于 `C++17 + hiredis + Redis Lua + pybind11` 实现分布式限流组件，支持令牌桶、滑动窗口和 Python 服务接入，解决多实例部署下本地限流配额不共享的问题。
- 使用 Redis Lua 将读取状态、补充令牌、扣减配额和设置 TTL 合并为原子操作，并通过 `SCRIPT LOAD + EVALSHA` 缓存脚本；热点 key 严格有效性压测下理论放行 `45` 次、实际放行 `45` 次，`over_issued=0`。
- 设计 `ResilientTokenBucketLimiter`，支持 Redis 不可用时切换本地令牌桶、fail-open 或 fail-closed，并通过 `backend_status`、`redis_error_count`、`fallback_hit_count` 暴露降级状态。
- 接入 FastAPI 短信验证码防刷场景，实现手机号、用户、IP 多维限流，并补充 Docker smoke、`pytest`、benchmark、Prometheus metrics 和 CI 验证链路。

版本二，偏中间件能力：

- 封装 Redis 连接池和限流器工厂，支持连接复用、连接超时、失败重试、健康检查和连接池统计，降低 Python 服务接入成本。
- 在 Lua 脚本中使用 Redis `TIME` 作为统一时间源，避免多实例本地时钟漂移影响滑动窗口判断和令牌桶补充。
- 通过 pybind11 暴露 C++ 限流能力，并在阻塞 Redis 调用期间释放 GIL，降低 Python 多线程 Web 场景下的互相阻塞。
- 使用 Docker Compose 打通 Redis、FastAPI、测试、压测和监控链路，当前 `pytest` 集成测试 `9 passed in 0.53s`，短压测热点 key 约 `19.5k QPS`。

## 简历短版

如果简历空间紧张，可以只放 3 条：

- 基于 `C++17 + hiredis + Redis Lua + pybind11` 实现 Redis 分布式限流组件，支持令牌桶、滑动窗口和 Python 服务接入。
- 使用 Lua 原子扣减、Redis `TIME` 统一时间源和 `SCRIPT LOAD + EVALSHA` 脚本缓存，严格有效性压测下 `over_issued=0`。
- 接入 FastAPI 短信验证码防刷场景，实现手机号、用户、IP 多维限流，并支持 Redis 故障本地降级、metrics、Docker smoke、pytest 和 benchmark 验证。

## 项目介绍 30 秒版

我做了一个面向 Python 服务的 Redis 分布式限流组件，主要解决多实例部署下本地限流配额不共享的问题。底层用 C++17 和 hiredis 封装 Redis 连接池和限流算法，通过 Lua 保证状态更新原子性，再用 pybind11 暴露给 Python。业务上接了 FastAPI 短信验证码接口，按手机号、用户和 IP 多维限流，同时支持 Redis 故障时本地令牌桶降级，并补了测试、压测和 metrics。

## 项目介绍 1 分钟版

这个项目的背景是：登录、短信验证码、下单这类接口都需要限流，但如果只在每个服务实例里做本地计数，多实例部署时总额度会被放大。所以我把限流状态放到 Redis，由多个实例共享配额。

核心实现上，我用 C++17 和 hiredis 实现了 Redis 连接池、令牌桶和滑动窗口；限流状态更新通过 Redis Lua 完成，把读取、计算、扣减和设置过期时间放在同一个脚本里，保证原子性。Lua 脚本里使用 Redis `TIME` 作为统一时间源，避免不同机器本地时钟不一致。

工程接入上，我用 pybind11 暴露 Python 模块，并在阻塞 Redis 调用时释放 GIL。FastAPI 示例里接了短信验证码防刷，按手机号、用户和 IP 三个维度限流。Redis 不可用时支持本地令牌桶、fail-open、fail-closed 三种降级策略，并通过 `backend_status` 暴露状态。最后用 Docker Compose、pytest、smoke、benchmark 和 metrics 做了验证闭环。

## 面试可强调的数据

- `pytest` 集成测试：`9 passed in 0.53s`
- 热点 key 严格有效性压测：理论放行 `45`，实际放行 `45`
- 超发量：`over_issued=0.00`
- 本次短压测吞吐：独立 key 约 `7.1k QPS`，热点 key 约 `19.5k QPS`
- 验证链路：`docker compose build`、`test`、`smoke`、`pytest`、`bench`

## 面试主动说明的边界

- 当前是轻量组件，不是完整限流平台。
- 单 Redis 是瓶颈和单点，后续可接 Sentinel / Cluster。
- 本地 fallback 只能保证单实例限流，Redis 故障时不能继续保证多实例全局配额。
- 短信验证码 demo 的手机号、用户、IP 三个维度是顺序检查，不是 all-or-nothing 原子事务。
- 当前压测是 Docker 短压测快照，不能直接等同生产容量评估。

## 不建议写法

避免这些表达：

- “实现高并发限流系统”但没有说明并发规模和验证方式。
- “支持分布式限流”但没有解释 Redis Lua 原子性。
- “生产级限流平台”但没有 Sentinel / Cluster / 配置中心 / 规则灰度。
- 只写算法名，不写业务落地和测试验证。

## 推荐投递标题

如果简历项目区需要一句标题，可以写：

> Redis 分布式限流组件：基于 C++/Redis Lua/pybind11 实现多实例共享配额，接入 FastAPI 短信验证码防刷并支持故障降级与压测验证。
