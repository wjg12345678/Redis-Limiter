import os
import time

import redis_limiter


class FakeOrderRepository:
    def __init__(self, backend_name: str = "mock-postgresql") -> None:
        self.backend_name = backend_name
        self._sequence = 0

    def create_order(self, *, user_id: str, sku: str, quantity: int) -> dict[str, object]:
        self._sequence += 1
        return {
            "order_id": f"ord-{self._sequence:06d}",
            "user_id": user_id,
            "sku": sku,
            "quantity": quantity,
            "persistence_backend": self.backend_name,
        }


def main() -> None:
    redis_config = redis_limiter.RedisConfig()
    redis_config.host = os.getenv("REDIS_HOST", "127.0.0.1")
    redis_config.port = int(os.getenv("REDIS_PORT", "6379"))
    redis_config.pool_size = 4
    redis_config.max_retries = int(os.getenv("REDIS_MAX_RETRIES", "3"))

    pool = redis_limiter.RedisPool(redis_config)
    limiter = redis_limiter.ResilientTokenBucketLimiter(
        redis_limiter.TokenBucketLimiter(pool, max_tokens=3, refill_rate=1.0, key_prefix="orders:"),
        redis_limiter.FallbackMode.LocalTokenBucket,
        local_max_tokens=2,
        local_refill_rate=0.5,
    )
    repository = FakeOrderRepository()

    user_id = "42"
    sku = "sku-demo"

    for idx in range(5):
        rate_limit_key = f"user:{user_id}:create_order"
        result = limiter.allow(rate_limit_key)
        if not result.allowed:
            print(
                f"request={idx} rejected "
                f"backend_status={result.backend_status} "
                f"retry_after_ms={result.retry_after_ms}"
            )
        else:
            order = repository.create_order(user_id=user_id, sku=sku, quantity=1)
            print(
                f"request={idx} created "
                f"order_id={order['order_id']} "
                f"backend={order['persistence_backend']} "
                f"remaining={result.remaining}"
            )
        time.sleep(0.2)


if __name__ == "__main__":
    main()
