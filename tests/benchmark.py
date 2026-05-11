import argparse
import multiprocessing as mp
import os
import statistics
import sys
import time

import redis_limiter


def build_redis_config() -> redis_limiter.RedisConfig:
    config = redis_limiter.RedisConfig()
    config.host = os.getenv("REDIS_HOST", "127.0.0.1")
    config.port = int(os.getenv("REDIS_PORT", "6379"))
    config.pool_size = int(os.getenv("REDIS_POOL_SIZE", "16"))
    config.connect_timeout_ms = int(os.getenv("REDIS_CONNECT_TIMEOUT_MS", "200"))
    config.socket_timeout_ms = int(os.getenv("REDIS_SOCKET_TIMEOUT_MS", "200"))
    config.max_retries = int(os.getenv("REDIS_MAX_RETRIES", "3"))
    return config


def percentile(sorted_values: list[float], ratio: float) -> float:
    if not sorted_values:
        return 0.0
    index = min(len(sorted_values) - 1, max(0, int(round((len(sorted_values) - 1) * ratio))))
    return sorted_values[index]


def run_worker(
    worker_id: int,
    duration_s: float,
    sample_rate: int,
    max_tokens: int,
    refill_rate: float,
    shared_key: bool,
    key_prefix: str,
    result_queue: mp.Queue,
) -> None:
    config = build_redis_config()
    pool = redis_limiter.RedisPool(config)
    limiter = redis_limiter.TokenBucketLimiter(
        pool,
        max_tokens=max_tokens,
        refill_rate=refill_rate,
        key_prefix=key_prefix,
    )

    deadline = time.perf_counter() + duration_s
    request_count = 0
    allowed_count = 0
    denied_count = 0
    error_count = 0
    samples_us: list[float] = []

    if shared_key:
        key = "bench:shared"
    else:
        key = f"bench:worker:{worker_id}"

    while time.perf_counter() < deadline:
        started_ns = time.perf_counter_ns()
        try:
            result = limiter.allow(key)
            if result.allowed:
                allowed_count += 1
            else:
                denied_count += 1
        except Exception:
            error_count += 1
        finally:
            request_count += 1

        if request_count % sample_rate == 0:
            elapsed_us = (time.perf_counter_ns() - started_ns) / 1000.0
            samples_us.append(elapsed_us)

    result_queue.put(
        {
            "requests": request_count,
            "allowed": allowed_count,
            "denied": denied_count,
            "errors": error_count,
            "samples_us": samples_us,
            "elapsed_s": duration_s,
        }
    )


def main() -> int:
    parser = argparse.ArgumentParser(description="Benchmark Redis token bucket limiter.")
    parser.add_argument("--workers", type=int, default=4, help="Number of worker processes.")
    parser.add_argument("--duration", type=float, default=10.0, help="Benchmark duration in seconds.")
    parser.add_argument("--sample-rate", type=int, default=100, help="Record 1 latency sample every N requests.")
    parser.add_argument(
        "--shared-key",
        action="store_true",
        help="All workers hit the same key to measure contention on a shared bucket.",
    )
    parser.add_argument(
        "--max-tokens",
        type=int,
        default=100000000,
        help="Bucket capacity. Keep this high if you want to benchmark throughput instead of throttling.",
    )
    parser.add_argument(
        "--refill-rate",
        type=float,
        default=100000000.0,
        help="Tokens refilled per second. Keep this high if you want to benchmark throughput instead of throttling.",
    )
    parser.add_argument(
        "--key-prefix",
        default="bench:tokenbucket:",
        help="Redis key prefix used by the benchmark.",
    )
    parser.add_argument(
        "--mode",
        choices=("throughput", "effectiveness"),
        default="throughput",
        help="throughput: measure raw throughput; effectiveness: measure whether the limiter enforces the configured quota.",
    )
    parser.add_argument(
        "--max-over-issue",
        type=float,
        default=0.0,
        help="Maximum allowed absolute over-issue in effectiveness mode.",
    )
    parser.add_argument(
        "--max-over-issue-ratio",
        type=float,
        default=0.0,
        help="Maximum allowed over-issue ratio relative to theoretical_allowed in effectiveness mode.",
    )
    args = parser.parse_args()

    if args.workers <= 0:
        raise SystemExit("--workers must be positive")
    if args.duration <= 0:
        raise SystemExit("--duration must be positive")
    if args.sample_rate <= 0:
        raise SystemExit("--sample-rate must be positive")

    run_key_prefix = f"{args.key_prefix}{int(time.time() * 1000)}:"
    result_queue: mp.Queue = mp.Queue()
    processes = [
        mp.Process(
            target=run_worker,
            args=(
                worker_id,
                args.duration,
                args.sample_rate,
                args.max_tokens,
                args.refill_rate,
                args.shared_key,
                run_key_prefix,
                result_queue,
            ),
        )
        for worker_id in range(args.workers)
    ]

    for process in processes:
        process.start()

    results = [result_queue.get() for _ in processes]

    for process in processes:
        process.join()

    elapsed_s = max(item["elapsed_s"] for item in results)
    total_requests = sum(item["requests"] for item in results)
    total_allowed = sum(item["allowed"] for item in results)
    total_denied = sum(item["denied"] for item in results)
    total_errors = sum(item["errors"] for item in results)
    all_samples_us = sorted(sample for item in results for sample in item["samples_us"])

    print(f"workers={args.workers} duration_s={elapsed_s:.2f} shared_key={args.shared_key}")
    print(
        f"requests={total_requests} allowed={total_allowed} denied={total_denied} "
        f"errors={total_errors} qps={total_requests / elapsed_s:.2f}"
    )

    if args.mode == "effectiveness":
        bucket_count = 1 if args.shared_key else args.workers
        theoretical_allowed = bucket_count * (args.max_tokens + (args.refill_rate * elapsed_s))
        over_issued = total_allowed - theoretical_allowed
        over_issued_ratio = (over_issued / theoretical_allowed) if theoretical_allowed > 0 else 0.0
        allowed_ratio = (total_allowed / total_requests) if total_requests else 0.0
        denied_ratio = (total_denied / total_requests) if total_requests else 0.0
        print(
            f"effectiveness theoretical_allowed={theoretical_allowed:.2f} "
            f"actual_allowed={total_allowed} over_issued={over_issued:.2f}"
        )
        print(
            f"effectiveness over_issued_ratio={over_issued_ratio:.6f} "
            f"max_over_issue={args.max_over_issue:.2f} "
            f"max_over_issue_ratio={args.max_over_issue_ratio:.6f}"
        )
        print(
            f"effectiveness allowed_ratio={allowed_ratio:.4f} "
            f"denied_ratio={denied_ratio:.4f}"
        )

        if over_issued > args.max_over_issue or over_issued_ratio > args.max_over_issue_ratio:
            print("FAIL effectiveness assertion exceeded", file=sys.stderr)
            return 1

        print("PASS effectiveness assertion")

    if all_samples_us:
        print(
            "latency_us "
            f"avg={statistics.fmean(all_samples_us):.2f} "
            f"p50={percentile(all_samples_us, 0.50):.2f} "
            f"p95={percentile(all_samples_us, 0.95):.2f} "
            f"p99={percentile(all_samples_us, 0.99):.2f}"
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
