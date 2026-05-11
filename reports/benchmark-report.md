# 压测报告

Redis 分布式限流组件

日期：`2026-05-11`

## 一页结论

- 在当前 Docker 测试环境下，Redis 令牌桶限流的吞吐约为 `7.1k` 到 `19.5k QPS`
- 在热点 key 严格有效性压测下，理论最大放行 `45` 次，实际放行 `45` 次
- `over_issued=0.00`，本次压测没有观察到超发
- 功能验证、Docker 冒烟测试、FastAPI 接入、短信验证码防刷、指标导出、`pytest` 回归测试均通过

## 压测范围

- 吞吐压测
  - `4` 个 worker
  - `5s` 持续时间
  - 独立 key 和热点 key 两种场景
- 限流有效性压测
  - `4` 个 worker
  - `5s` 持续时间
  - 热点 key
  - `max_tokens=20`
  - `refill_rate=5`
  - 严格断言：`max-over-issue=0`、`max-over-issue-ratio=0`

## 测试环境

| 项目 | 值 |
| --- | --- |
| 运行方式 | Docker Compose |
| Redis | `redis:7-alpine` |
| 应用栈 | `C++17 + hiredis + pybind11 + FastAPI` |
| 限流路径 | Redis Lua Token Bucket (`SCRIPT LOAD + EVALSHA`) |
| 验证链路 | `test`、`smoke`、`pytest`、`bench`、`/metrics`、`/sms/send-code` |

## 吞吐结果

| 场景 | Workers | 时长 | 总请求数 | QPS | 平均延迟(us) | P95(us) | P99(us) | 错误数 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 独立 key | 4 | 5s | 35278 | 7055.60 | 559.81 | 928.17 | 2769.85 | 0 |
| 热点 key | 4 | 5s | 97376 | 19475.20 | 215.67 | 329.48 | 653.82 | 0 |

![吞吐 QPS 对比](../assets/charts/throughput-qps.svg)

![吞吐 P95 延迟对比](../assets/charts/throughput-p95.svg)

结果解读：

- 两组场景都没有错误，热点 key 本次约 `19.5k QPS`
- 这次压测里，热点 key 明显高于独立 key；该结果与 Docker 环境、Redis 状态和 worker 调度有关，报告只作为同一环境下的短压测快照
- 未观察到执行错误

## 限流有效性结果

| 场景 | Workers | 时长 | Max Tokens | Refill Rate | 理论放行 | 实际放行 | 超发量 | 超发比例 | 拒绝率 | 断言结果 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 热点 key | 4 | 5s | 20 | 5/s | 45.00 | 45 | 0.00 | 0.000000 | 0.9995 | PASS |

![限流有效性柱状图](../assets/charts/effectiveness-bars.svg)

![允许/拒绝占比图](../assets/charts/effectiveness-pie.svg)

结果解读：

- 理论最大放行次数与实际放行次数完全一致
- `over_issued=0.00`，本次压测未出现超发
- 热点 key 场景能够覆盖 Redis Lua 原子扣减路径的并发竞争
- 当前实现优先通过 `SCRIPT LOAD + EVALSHA` 执行 Lua 脚本
- 本次严格有效性压测的总请求数为 `92941`，QPS 为 `18588.20`

## 验证快照

| 检查项 | 结果 |
| --- | --- |
| 令牌桶功能验证 | PASS |
| Redis 故障降级验证 | PASS |
| Docker smoke | PASS |
| FastAPI `/healthz` | PASS |
| FastAPI `/metrics` | PASS |
| FastAPI `/sms/send-code` | PASS |
| Prometheus 指标抓取 | PASS |
| `pytest` 集成测试 | `9 passed in 0.53s` |
| 限流有效性断言 | PASS |

监控补充说明：

- 已确认 Prometheus 能成功抓取 `demo_rate_limit_allowed_total` 等指标
- 当前仓库中的 Prometheus 与 Grafana 配置已准备完成
- 由于外部镜像拉取网络波动，Grafana 截图暂未固化进仓库，但监控链路本身已经验证通过

Grafana 展示图：

![Grafana Dashboard Mock](../assets/charts/grafana-dashboard-mock.svg)

## 复现命令

```bash
docker compose build
docker compose up -d redis app

docker compose run --rm test remote
docker compose run --rm -e REDIS_HOST=redis-unavailable test fallback
docker compose run --rm smoke
docker compose run --rm pytest

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

## 总结

这个项目已经不仅仅是“实现了一个限流算法”，而是具备了：

- Redis 分布式配额共享
- Lua 原子更新与脚本缓存执行
- Python/FastAPI 业务接入
- 短信验证码手机号 / 用户 / IP 多维防刷示例
- Redis 故障降级
- Docker 冒烟、自动化测试与 CI
- 压测与有效性断言

从秋招项目角度看，这份报告可以直接用来支撑“性能结果”和“限流有效性”两类面试问题。
