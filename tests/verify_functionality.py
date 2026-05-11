import argparse
import os
import sys
import time

import redis_limiter


def build_redis_config() -> redis_limiter.RedisConfig:
    config = redis_limiter.RedisConfig()
    config.host = os.getenv("REDIS_HOST", "127.0.0.1")
    config.port = int(os.getenv("REDIS_PORT", "6379"))
    config.pool_size = int(os.getenv("REDIS_POOL_SIZE", "4"))
    config.connect_timeout_ms = int(os.getenv("REDIS_CONNECT_TIMEOUT_MS", "200"))
    config.socket_timeout_ms = int(os.getenv("REDIS_SOCKET_TIMEOUT_MS", "200"))
    config.max_retries = int(os.getenv("REDIS_MAX_RETRIES", "3"))
    return config


def verify_remote_token_bucket() -> None:
    config = build_redis_config()
    pool = redis_limiter.RedisPool(config)

    if not pool.health_check():
        raise AssertionError(
            f"Redis unavailable at {config.host}:{config.port}. Start Redis before running remote verification."
        )

    limiter = redis_limiter.TokenBucketLimiter(pool, max_tokens=3, refill_rate=0.5, key_prefix="verify:remote:")
    test_key = f"token-bucket:{int(time.time() * 1000)}"

    # Ensure this run starts from a clean bucket state.
    limiter.reset(test_key)

    results = [limiter.allow(test_key) for _ in range(4)]
    allowed_flags = [result.allowed for result in results]

    if allowed_flags[:3] != [True, True, True]:
        raise AssertionError(f"Expected first 3 requests allowed, got {allowed_flags}")
    if allowed_flags[3] is not False:
        raise AssertionError(f"Expected 4th request denied, got {allowed_flags}")
    if results[3].retry_after_ms <= 0:
        raise AssertionError(f"Expected denied request to include retry_after_ms, got {results[3].retry_after_ms}")
    if any(result.backend_status != redis_limiter.BackendStatus.Healthy for result in results):
        raise AssertionError("Expected remote token bucket requests to report Healthy backend status")

    print("PASS remote token bucket")
    for idx, result in enumerate(results, start=1):
        print(
            f"  req={idx} allowed={result.allowed} remaining={result.remaining} "
            f"retry_after_ms={result.retry_after_ms}"
        )


def verify_resilient_fallback() -> None:
    config = build_redis_config()
    pool = redis_limiter.RedisPool(config)
    remote = redis_limiter.TokenBucketLimiter(pool, max_tokens=3, refill_rate=1.0, key_prefix="verify:fallback:")
    limiter = redis_limiter.ResilientTokenBucketLimiter(
        remote,
        redis_limiter.FallbackMode.LocalTokenBucket,
        local_max_tokens=2,
        local_refill_rate=0.01,
    )

    test_key = f"resilient:{int(time.time() * 1000)}"
    results = [limiter.allow(test_key) for _ in range(3)]
    allowed_flags = [result.allowed for result in results]

    if allowed_flags != [True, True, False]:
        raise AssertionError(f"Expected local fallback to allow 2 then deny 1, got {allowed_flags}")
    if limiter.redis_error_count() == 0:
        raise AssertionError("Expected redis_error_count > 0 when Redis is unavailable")
    if limiter.fallback_hit_count() != 3:
        raise AssertionError(f"Expected fallback_hit_count == 3, got {limiter.fallback_hit_count()}")
    if any(result.backend_status != redis_limiter.BackendStatus.Fallback for result in results):
        raise AssertionError("Expected fallback requests to report Fallback backend status")

    print("PASS resilient fallback")
    print(
        f"  redis_error_count={limiter.redis_error_count()} "
        f"fallback_hit_count={limiter.fallback_hit_count()}"
    )
    for idx, result in enumerate(results, start=1):
        print(
            f"  req={idx} allowed={result.allowed} remaining={result.remaining} "
            f"retry_after_ms={result.retry_after_ms}"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify redis-rate-limiter behavior.")
    parser.add_argument(
        "mode",
        choices=("remote", "fallback"),
        help="remote: verify normal Redis token bucket; fallback: verify local fallback when Redis is unavailable",
    )
    args = parser.parse_args()

    if args.mode == "remote":
        verify_remote_token_bucket()
    else:
        verify_resilient_fallback()

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL {error}", file=sys.stderr)
        raise SystemExit(1)
