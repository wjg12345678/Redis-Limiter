import os
import threading
import time
from typing import Literal

import redis_limiter
from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import PlainTextResponse
from pydantic import BaseModel, Field


def build_redis_config(
    host: str | None = None,
    port: int | None = None,
    pool_size: int | None = None,
    connect_timeout_ms: int | None = None,
    socket_timeout_ms: int | None = None,
    max_retries: int | None = None,
) -> redis_limiter.RedisConfig:
    config = redis_limiter.RedisConfig()
    config.host = host or os.getenv("REDIS_HOST", "127.0.0.1")
    config.port = port or int(os.getenv("REDIS_PORT", "6379"))
    config.pool_size = pool_size if pool_size is not None else int(os.getenv("REDIS_POOL_SIZE", "8"))
    config.connect_timeout_ms = (
        connect_timeout_ms
        if connect_timeout_ms is not None
        else int(os.getenv("REDIS_CONNECT_TIMEOUT_MS", "200"))
    )
    config.socket_timeout_ms = (
        socket_timeout_ms
        if socket_timeout_ms is not None
        else int(os.getenv("REDIS_SOCKET_TIMEOUT_MS", "200"))
    )
    config.max_retries = max_retries if max_retries is not None else int(os.getenv("REDIS_MAX_RETRIES", "3"))
    return config


def parse_fallback_mode(mode: str) -> redis_limiter.FallbackMode:
    normalized = mode.strip().lower()
    if normalized == "failopen":
        return redis_limiter.FallbackMode.FailOpen
    if normalized == "failclosed":
        return redis_limiter.FallbackMode.FailClosed
    return redis_limiter.FallbackMode.LocalTokenBucket


def fallback_mode_name(mode: redis_limiter.FallbackMode) -> str:
    if mode == redis_limiter.FallbackMode.FailOpen:
        return "FailOpen"
    if mode == redis_limiter.FallbackMode.FailClosed:
        return "FailClosed"
    return "LocalTokenBucket"


def backend_status_name(status: redis_limiter.BackendStatus) -> str:
    if status == redis_limiter.BackendStatus.Unavailable:
        return "Unavailable"
    if status == redis_limiter.BackendStatus.Fallback:
        return "Fallback"
    return "Healthy"


class RateLimitRequest(BaseModel):
    key: str = Field(..., min_length=1)
    tokens_needed: int = Field(default=1, ge=1)


class RateLimitResponse(BaseModel):
    allowed: bool
    current_count: int
    remaining: int
    reset_after_ms: int
    retry_after_ms: int
    backend_status: Literal["Healthy", "Unavailable", "Fallback"]
    fallback_mode: Literal["FailOpen", "FailClosed", "LocalTokenBucket"]
    redis_error_count: int
    fallback_hit_count: int


class CreateOrderRequest(BaseModel):
    user_id: str = Field(..., min_length=1)
    sku: str = Field(..., min_length=1)
    quantity: int = Field(default=1, ge=1)


class CreateOrderResponse(BaseModel):
    order_id: str
    status: Literal["created"]
    user_id: str
    sku: str
    quantity: int
    inventory_reserved: bool
    persistence_backend: str
    rate_limit: RateLimitResponse


class SendSmsCodeRequest(BaseModel):
    phone: str = Field(..., min_length=5, max_length=32)
    user_id: str = Field(..., min_length=1)
    scene: str = Field(default="login", min_length=1, max_length=32)


class SmsRuleResult(BaseModel):
    rule: Literal["phone_per_minute", "user_per_hour", "ip_per_minute"]
    key: str
    allowed: bool
    remaining: int
    retry_after_ms: int
    backend_status: Literal["Healthy", "Unavailable", "Fallback"]
    fallback_mode: Literal["FailOpen", "FailClosed", "LocalTokenBucket"]
    redis_error_count: int
    fallback_hit_count: int


class SendSmsCodeResponse(BaseModel):
    message_id: str
    status: Literal["sent"]
    phone: str
    user_id: str
    scene: str
    gateway_backend: str
    rate_limits: list[SmsRuleResult]


class DemoMetrics:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self.requests_total = 0
        self.allowed_total = 0
        self.denied_total = 0
        self.redis_error_total = 0
        self.fallback_total = 0
        self.downstream_calls_total = 0
        self.request_duration_seconds_sum = 0.0

    def observe(
        self,
        *,
        allowed: bool,
        redis_error_delta: int,
        fallback_delta: int,
        downstream_called: bool,
        duration_seconds: float,
    ) -> None:
        with self._lock:
            self.requests_total += 1
            if allowed:
                self.allowed_total += 1
            else:
                self.denied_total += 1
            self.redis_error_total += max(0, redis_error_delta)
            self.fallback_total += max(0, fallback_delta)
            if downstream_called:
                self.downstream_calls_total += 1
            self.request_duration_seconds_sum += duration_seconds

    def render_prometheus(self, *, redis_healthy: bool, fallback_mode: str) -> str:
        fallback_value = {
            "FailOpen": 0,
            "FailClosed": 1,
            "LocalTokenBucket": 2,
        }[fallback_mode]
        with self._lock:
            lines = [
                "# HELP demo_rate_limit_requests_total Total number of rate-limit API requests.",
                "# TYPE demo_rate_limit_requests_total counter",
                f"demo_rate_limit_requests_total {self.requests_total}",
                "# HELP demo_rate_limit_allowed_total Total number of allowed rate-limit API requests.",
                "# TYPE demo_rate_limit_allowed_total counter",
                f"demo_rate_limit_allowed_total {self.allowed_total}",
                "# HELP demo_rate_limit_denied_total Total number of denied rate-limit API requests.",
                "# TYPE demo_rate_limit_denied_total counter",
                f"demo_rate_limit_denied_total {self.denied_total}",
                "# HELP demo_rate_limit_redis_error_total Total number of Redis errors observed by the API.",
                "# TYPE demo_rate_limit_redis_error_total counter",
                f"demo_rate_limit_redis_error_total {self.redis_error_total}",
                "# HELP demo_rate_limit_fallback_total Total number of fallback executions observed by the API.",
                "# TYPE demo_rate_limit_fallback_total counter",
                f"demo_rate_limit_fallback_total {self.fallback_total}",
                "# HELP demo_downstream_calls_total Total number of downstream persistence calls.",
                "# TYPE demo_downstream_calls_total counter",
                f"demo_downstream_calls_total {self.downstream_calls_total}",
                "# HELP demo_rate_limit_request_duration_seconds_sum Sum of request durations for the rate-limit API.",
                "# TYPE demo_rate_limit_request_duration_seconds_sum counter",
                f"demo_rate_limit_request_duration_seconds_sum {self.request_duration_seconds_sum:.6f}",
                "# HELP demo_redis_health Redis health status reported by the demo app. 1 means healthy.",
                "# TYPE demo_redis_health gauge",
                f"demo_redis_health {1 if redis_healthy else 0}",
                "# HELP demo_fallback_mode Current fallback mode. FailOpen=0, FailClosed=1, LocalTokenBucket=2.",
                "# TYPE demo_fallback_mode gauge",
                f"demo_fallback_mode {fallback_value}",
            ]
        return "\n".join(lines) + "\n"


class FakeOrderRepository:
    def __init__(self, backend_name: str = "mock-postgresql") -> None:
        self.backend_name = backend_name
        self._lock = threading.Lock()
        self._sequence = 0

    def create_order(self, *, user_id: str, sku: str, quantity: int) -> dict[str, object]:
        with self._lock:
            self._sequence += 1
            order_id = f"ord-{self._sequence:06d}"
        return {
            "order_id": order_id,
            "status": "created",
            "user_id": user_id,
            "sku": sku,
            "quantity": quantity,
            "inventory_reserved": True,
            "persistence_backend": self.backend_name,
        }


class FakeSmsGateway:
    def __init__(self, backend_name: str = "mock-sms-gateway") -> None:
        self.backend_name = backend_name
        self._lock = threading.Lock()
        self._sequence = 0

    def send_code(self, *, phone: str, user_id: str, scene: str) -> dict[str, object]:
        with self._lock:
            self._sequence += 1
            message_id = f"sms-{self._sequence:06d}"
        return {
            "message_id": message_id,
            "status": "sent",
            "phone": phone,
            "user_id": user_id,
            "scene": scene,
            "gateway_backend": self.backend_name,
        }


def build_rate_limit_response(
    *,
    limiter: redis_limiter.ResilientTokenBucketLimiter,
    result: redis_limiter.RateLimitResult,
    redis_error_count: int,
    fallback_hit_count: int,
) -> RateLimitResponse:
    return RateLimitResponse(
        allowed=result.allowed,
        current_count=result.current_count,
        remaining=result.remaining,
        reset_after_ms=result.reset_after_ms,
        retry_after_ms=result.retry_after_ms,
        backend_status=backend_status_name(result.backend_status),
        fallback_mode=fallback_mode_name(limiter.fallback_mode()),
        redis_error_count=redis_error_count,
        fallback_hit_count=fallback_hit_count,
    )


def build_sms_rule_result(
    *,
    rule: Literal["phone_per_minute", "user_per_hour", "ip_per_minute"],
    key: str,
    limiter: redis_limiter.ResilientTokenBucketLimiter,
    result: redis_limiter.RateLimitResult,
) -> SmsRuleResult:
    return SmsRuleResult(
        rule=rule,
        key=key,
        allowed=result.allowed,
        remaining=result.remaining,
        retry_after_ms=result.retry_after_ms,
        backend_status=backend_status_name(result.backend_status),
        fallback_mode=fallback_mode_name(limiter.fallback_mode()),
        redis_error_count=limiter.redis_error_count(),
        fallback_hit_count=limiter.fallback_hit_count(),
    )


def create_app(
    *,
    redis_host: str | None = None,
    redis_port: int | None = None,
    max_tokens: int | None = None,
    refill_rate: float | None = None,
    local_max_tokens: int | None = None,
    local_refill_rate: float | None = None,
    key_prefix: str | None = None,
    fallback_mode: str | None = None,
    redis_pool_size: int | None = None,
    redis_connect_timeout_ms: int | None = None,
    redis_socket_timeout_ms: int | None = None,
    redis_max_retries: int | None = None,
    sms_key_prefix: str | None = None,
    sms_phone_max_tokens: int | None = None,
    sms_phone_refill_rate: float | None = None,
    sms_user_max_tokens: int | None = None,
    sms_user_refill_rate: float | None = None,
    sms_ip_max_tokens: int | None = None,
    sms_ip_refill_rate: float | None = None,
) -> FastAPI:
    app = FastAPI(title="Redis Rate Limiter Demo", version="1.0.0")

    redis_config = build_redis_config(
        redis_host,
        redis_port,
        pool_size=redis_pool_size,
        connect_timeout_ms=redis_connect_timeout_ms,
        socket_timeout_ms=redis_socket_timeout_ms,
        max_retries=redis_max_retries,
    )
    pool = redis_limiter.RedisPool(redis_config)
    remote = redis_limiter.TokenBucketLimiter(
        pool,
        max_tokens=max_tokens or int(os.getenv("RATE_LIMIT_MAX_TOKENS", "20")),
        refill_rate=refill_rate or float(os.getenv("RATE_LIMIT_REFILL_RATE", "5")),
        key_prefix=key_prefix or os.getenv("RATE_LIMIT_KEY_PREFIX", "api:tokenbucket:"),
    )
    limiter = redis_limiter.ResilientTokenBucketLimiter(
        remote,
        parse_fallback_mode(fallback_mode or os.getenv("RATE_LIMIT_FALLBACK_MODE", "LocalTokenBucket")),
        local_max_tokens=local_max_tokens or int(os.getenv("LOCAL_MAX_TOKENS", "10")),
        local_refill_rate=local_refill_rate or float(os.getenv("LOCAL_REFILL_RATE", "2")),
    )
    sms_fallback_mode = parse_fallback_mode(fallback_mode or os.getenv("RATE_LIMIT_FALLBACK_MODE", "LocalTokenBucket"))
    sms_prefix = sms_key_prefix or os.getenv("SMS_RATE_LIMIT_KEY_PREFIX", "sms:tokenbucket:")

    def create_sms_limiter(rule_prefix: str, tokens: int, refill: float) -> redis_limiter.ResilientTokenBucketLimiter:
        remote_limiter = redis_limiter.TokenBucketLimiter(
            pool,
            max_tokens=tokens,
            refill_rate=refill,
            key_prefix=f"{sms_prefix}{rule_prefix}:",
        )
        return redis_limiter.ResilientTokenBucketLimiter(
            remote_limiter,
            sms_fallback_mode,
            local_max_tokens=tokens,
            local_refill_rate=refill,
        )

    sms_limiters: dict[str, redis_limiter.ResilientTokenBucketLimiter] = {
        "phone_per_minute": create_sms_limiter(
            "phone",
            sms_phone_max_tokens or int(os.getenv("SMS_PHONE_MAX_TOKENS", "1")),
            sms_phone_refill_rate or float(os.getenv("SMS_PHONE_REFILL_RATE", str(1 / 60))),
        ),
        "user_per_hour": create_sms_limiter(
            "user",
            sms_user_max_tokens or int(os.getenv("SMS_USER_MAX_TOKENS", "5")),
            sms_user_refill_rate or float(os.getenv("SMS_USER_REFILL_RATE", str(5 / 3600))),
        ),
        "ip_per_minute": create_sms_limiter(
            "ip",
            sms_ip_max_tokens or int(os.getenv("SMS_IP_MAX_TOKENS", "20")),
            sms_ip_refill_rate or float(os.getenv("SMS_IP_REFILL_RATE", str(20 / 60))),
        ),
    }

    metrics = DemoMetrics()
    repository = FakeOrderRepository()
    sms_gateway = FakeSmsGateway()
    app.state.pool = pool
    app.state.limiter = limiter
    app.state.sms_limiters = sms_limiters
    app.state.metrics = metrics
    app.state.repository = repository
    app.state.sms_gateway = sms_gateway

    @app.get("/healthz")
    def healthz() -> dict[str, object]:
        redis_healthy = pool.health_check()
        return {
            "ok": True,
            "redis_healthy": redis_healthy,
            "fallback_mode": fallback_mode_name(limiter.fallback_mode()),
            "persistence_backend": repository.backend_name,
        }

    @app.post("/rate-limit/check", response_model=RateLimitResponse)
    def check_rate_limit(request: RateLimitRequest) -> RateLimitResponse:
        before_redis_errors = limiter.redis_error_count()
        before_fallback_hits = limiter.fallback_hit_count()
        started_at = time.perf_counter()
        result = limiter.allow(request.key, request.tokens_needed)
        after_redis_errors = limiter.redis_error_count()
        after_fallback_hits = limiter.fallback_hit_count()
        metrics.observe(
            allowed=result.allowed,
            redis_error_delta=after_redis_errors - before_redis_errors,
            fallback_delta=after_fallback_hits - before_fallback_hits,
            downstream_called=False,
            duration_seconds=time.perf_counter() - started_at,
        )
        return build_rate_limit_response(
            limiter=limiter,
            result=result,
            redis_error_count=after_redis_errors,
            fallback_hit_count=after_fallback_hits,
        )

    @app.post("/orders", response_model=CreateOrderResponse)
    def create_order(request: CreateOrderRequest) -> CreateOrderResponse:
        rate_limit_key = f"user:{request.user_id}:create_order"
        before_redis_errors = limiter.redis_error_count()
        before_fallback_hits = limiter.fallback_hit_count()
        started_at = time.perf_counter()

        result = limiter.allow(rate_limit_key, request.quantity)
        after_redis_errors = limiter.redis_error_count()
        after_fallback_hits = limiter.fallback_hit_count()
        rate_limit = build_rate_limit_response(
            limiter=limiter,
            result=result,
            redis_error_count=after_redis_errors,
            fallback_hit_count=after_fallback_hits,
        )

        if not result.allowed:
            metrics.observe(
                allowed=False,
                redis_error_delta=after_redis_errors - before_redis_errors,
                fallback_delta=after_fallback_hits - before_fallback_hits,
                downstream_called=False,
                duration_seconds=time.perf_counter() - started_at,
            )
            raise HTTPException(
                status_code=429,
                detail={
                    "message": "rate limit exceeded",
                    "rate_limit": rate_limit.model_dump(),
                },
            )

        persisted = repository.create_order(
            user_id=request.user_id,
            sku=request.sku,
            quantity=request.quantity,
        )
        metrics.observe(
            allowed=True,
            redis_error_delta=after_redis_errors - before_redis_errors,
            fallback_delta=after_fallback_hits - before_fallback_hits,
            downstream_called=True,
            duration_seconds=time.perf_counter() - started_at,
        )
        return CreateOrderResponse(rate_limit=rate_limit, **persisted)

    @app.post("/sms/send-code", response_model=SendSmsCodeResponse)
    def send_sms_code(payload: SendSmsCodeRequest, request: Request) -> SendSmsCodeResponse:
        client_ip = request.client.host if request.client else "unknown"
        started_at = time.perf_counter()
        before_redis_errors = sum(item.redis_error_count() for item in sms_limiters.values())
        before_fallback_hits = sum(item.fallback_hit_count() for item in sms_limiters.values())

        rule_keys: list[tuple[Literal["phone_per_minute", "user_per_hour", "ip_per_minute"], str]] = [
            ("phone_per_minute", f"{payload.scene}:phone:{payload.phone}"),
            ("user_per_hour", f"{payload.scene}:user:{payload.user_id}"),
            ("ip_per_minute", f"{payload.scene}:ip:{client_ip}"),
        ]
        rule_results: list[SmsRuleResult] = []

        for rule, key in rule_keys:
            rule_limiter = sms_limiters[rule]
            result = rule_limiter.allow(key)
            rule_result = build_sms_rule_result(
                rule=rule,
                key=key,
                limiter=rule_limiter,
                result=result,
            )
            rule_results.append(rule_result)
            if not result.allowed:
                after_redis_errors = sum(item.redis_error_count() for item in sms_limiters.values())
                after_fallback_hits = sum(item.fallback_hit_count() for item in sms_limiters.values())
                metrics.observe(
                    allowed=False,
                    redis_error_delta=after_redis_errors - before_redis_errors,
                    fallback_delta=after_fallback_hits - before_fallback_hits,
                    downstream_called=False,
                    duration_seconds=time.perf_counter() - started_at,
                )
                raise HTTPException(
                    status_code=429,
                    detail={
                        "message": "sms rate limit exceeded",
                        "blocked_rule": rule,
                        "rate_limits": [item.model_dump() for item in rule_results],
                    },
                )

        sent = sms_gateway.send_code(
            phone=payload.phone,
            user_id=payload.user_id,
            scene=payload.scene,
        )
        after_redis_errors = sum(item.redis_error_count() for item in sms_limiters.values())
        after_fallback_hits = sum(item.fallback_hit_count() for item in sms_limiters.values())
        metrics.observe(
            allowed=True,
            redis_error_delta=after_redis_errors - before_redis_errors,
            fallback_delta=after_fallback_hits - before_fallback_hits,
            downstream_called=True,
            duration_seconds=time.perf_counter() - started_at,
        )
        return SendSmsCodeResponse(rate_limits=rule_results, **sent)

    @app.get("/metrics", response_class=PlainTextResponse)
    def metrics_endpoint() -> PlainTextResponse:
        redis_healthy = pool.health_check()
        fallback_mode = fallback_mode_name(limiter.fallback_mode())
        return PlainTextResponse(
            metrics.render_prometheus(redis_healthy=redis_healthy, fallback_mode=fallback_mode),
            media_type="text/plain; version=0.0.4; charset=utf-8",
        )

    return app


app = create_app()
