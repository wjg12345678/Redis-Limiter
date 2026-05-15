# Redis-Limiter 文档导航

这份文档是 Redis-Limiter 的阅读入口。项目资料已经覆盖学习路线、面试问答、接入指南、生产化和答辩攻防，不需要从头到尾机械阅读。先明确目标，再按路径看。

## 1. 项目一句话

```text
Redis-Limiter 是一个基于 C++17、hiredis、Redis Lua 和 pybind11 的分布式限流组件，支持令牌桶、滑动窗口、Redis TIME 统一时间源、连接池、fallback、Python 接入、FastAPI demo、metrics 和压测验证。
```

学习时要抓住三条主线：

```text
算法：令牌桶、滑动窗口
原子性：Redis Lua、TIME、SCRIPT LOAD/EVALSHA
组件化：C++ SDK、Python binding、FastAPI demo、未来 HTTP/gRPC
```

## 2. 文档分类

| 目标 | 推荐文档 |
| --- | --- |
| 系统学习 | [project-study-guide-complete.md](project-study-guide-complete.md) |
| 面试完整准备 | [interview-qna-complete.md](interview-qna-complete.md) |
| 快速面试讲稿 | [interview-guide.md](interview-guide.md) |
| 题库速查 | [interview-qa.md](interview-qa.md) |
| 简历写法 | [resume-bullets.md](resume-bullets.md) |
| 组件接入 | [integration-guide.md](integration-guide.md) |
| 生产化路线 | [production-hardening-roadmap.md](production-hardening-roadmap.md) |
| 答辩攻防 | [defense-playbook.md](defense-playbook.md) |

## 3. 新手阅读顺序

第一次看项目：

```text
1. README.md
2. docs/project-study-guide-complete.md 的 1-5 章
3. docs/integration-guide.md
4. docs/interview-guide.md
5. docs/defense-playbook.md
```

目标是先搞懂：

- 它是 SDK 还是服务。
- 为什么多实例限流不能只用本地内存。
- Redis Lua 为什么必要。
- 令牌桶和滑动窗口怎么取舍。
- Redis 挂了怎么 fallback。
- Atlas 为什么只是一个接入方。

## 4. 面试准备路径

### 4.1 只有 30 分钟

只看：

```text
1. README.md 的项目定位和核心能力
2. docs/resume-bullets.md
3. docs/interview-guide.md
4. docs/defense-playbook.md 的最终 1 分钟答辩稿
```

必须会讲：

- 这个项目不是 Atlas 内嵌代码。
- 当前是 SDK / 组件，不是完整限流平台。
- Redis Lua 保证检查和扣减原子性。
- `allow()` 不是幂等。
- Redis 故障时 fallback 有边界。

### 4.2 有 2 小时

按这个顺序：

```text
1. docs/project-study-guide-complete.md
2. docs/integration-guide.md
3. docs/interview-qna-complete.md
4. docs/production-hardening-roadmap.md
5. docs/defense-playbook.md
```

重点看：RedisPool、TokenBucket Lua、SlidingWindow Lua、pybind11 为什么释放 GIL、FastAPI 短信验证码 demo、Sentinel/Cluster 后续怎么补。

## 5. 源码阅读路径

不要先看 demo，先看公开 API，再看实现。

### 5.1 公共接口

```text
include/redis_pool.hpp
include/sliding_window_limiter.hpp
```

要回答：`RedisConfig` 有哪些配置、`RateLimitResult` 为什么不只是 bool、`BackendStatus` 有什么意义、fallback 模式怎么表达。

### 5.2 Redis 连接池

```text
src/redis_pool.cpp
```

要回答：连接如何创建和释放、RAII guard 有什么价值、health_check 怎么做、连接失败和命令失败怎么区分。

### 5.3 限流算法

```text
src/sliding_window_limiter.cpp
```

要回答：令牌桶 Redis 里存什么、令牌补充公式是什么、滑动窗口为什么用 ZSET、Lua 脚本为什么要设置 TTL、NOSCRIPT 怎么恢复。

### 5.4 Python 接入

```text
src/python_binding.cpp
examples/python_demo.py
examples/fastapi_demo.py
```

要回答：pybind11 暴露了哪些类、为什么阻塞 Redis 调用要释放 GIL、FastAPI demo 如何构造多维限流 key。

### 5.5 测试和压测

```text
tests/verify_functionality.py
tests/test_integration.py
tests/smoke_docker.py
tests/benchmark.py
reports/benchmark-report.md
```

要回答：怎么证明没有超发、怎么验证 Redis 故障降级、为什么 benchmark 不能只看 QPS。

## 6. 最重要的 10 个问题

1. 为什么不用本地内存限流？
2. 为什么不用 Nginx 限流就够了？
3. 为什么不用 Redis `INCR + EXPIRE`？
4. 为什么要用 Lua？
5. Redis Lua 原子性是不是数据库事务？
6. 为什么用 Redis TIME？
7. 令牌桶和滑动窗口怎么选？
8. Redis 挂了怎么办？
9. `allow()` 是否幂等？
10. 这个项目后续如何支持 Java/Go/Node？

## 7. 生产化边界

优先看：

```text
docs/production-hardening-roadmap.md
docs/integration-guide.md
docs/defense-playbook.md
```

必须主动承认：当前不是完整限流平台，动态规则中心还没有，多语言 SDK 还不完整，Sentinel/Cluster 支持需要补，本地 fallback 在多实例下可能放大额度，多维限流顺序检查不是全局原子。

更好的说法：

```text
当前是可复用限流组件，核心算法和接入能力已经完成；平台化还要补规则中心、多语言协议、高可用 Redis、熔断、租户隔离和管理面。
```

## 8. 和 Atlas 的关系

面试时要讲清：

```text
Atlas 是业务项目，Redis-Limiter 是基础组件。
Atlas 只构造登录/注册限流 key，不复制限流源码。
Redis-Limiter 可以被 Atlas、FastAPI demo、未来其他语言服务接入。
```

这正是两个项目能分开写简历的原因。

## 9. 最终建议

Redis-Limiter 的学习重点不是背 Lua 代码，而是理解：

```text
多实例为什么需要共享配额
检查和扣减为什么必须原子
故障降级为什么必须让业务感知
组件化为什么比复制代码更合理
生产化为什么要从 SDK 走向服务化和规则中心
```

能把这五点讲清楚，项目就能支撑大多数面试追问。
