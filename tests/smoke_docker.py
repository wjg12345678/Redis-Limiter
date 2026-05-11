import sys
import time

import httpx


def wait_for_app(base_url: str, timeout_s: float = 20.0) -> None:
    deadline = time.monotonic() + timeout_s
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        try:
            response = httpx.get(f"{base_url}/healthz", timeout=1.0)
            if response.status_code == 200:
                return
        except Exception as error:  # pragma: no cover - smoke bootstrap
            last_error = error
        time.sleep(0.5)
    raise RuntimeError(f"app did not become healthy within {timeout_s}s: {last_error}")


def main() -> int:
    base_url = "http://app:8000"
    wait_for_app(base_url)

    health = httpx.get(f"{base_url}/healthz", timeout=2.0)
    if health.status_code != 200 or health.json().get("redis_healthy") is not True:
        raise AssertionError(f"unexpected healthz response: status={health.status_code} body={health.text}")

    payloads = []
    for _ in range(3):
        response = httpx.post(
            f"{base_url}/rate-limit/check",
            json={"key": "smoke:user:1"},
            timeout=2.0,
        )
        if response.status_code != 200:
            raise AssertionError(f"unexpected rate-limit response: status={response.status_code} body={response.text}")
        payloads.append(response.json())

    allowed = [payload["allowed"] for payload in payloads]
    statuses = [payload["backend_status"] for payload in payloads]
    if allowed != [True, True, True]:
        raise AssertionError(f"unexpected allowed sequence: {allowed}")
    if any(status != "Healthy" for status in statuses):
        raise AssertionError(f"unexpected backend statuses: {statuses}")

    sms_response = httpx.post(
        f"{base_url}/sms/send-code",
        json={"phone": "13800000000", "user_id": "smoke-user", "scene": "login"},
        timeout=2.0,
    )
    if sms_response.status_code != 200:
        raise AssertionError(f"unexpected sms response: status={sms_response.status_code} body={sms_response.text}")

    metrics = httpx.get(f"{base_url}/metrics", timeout=2.0)
    if metrics.status_code != 200:
        raise AssertionError(f"unexpected metrics response: status={metrics.status_code} body={metrics.text}")
    body = metrics.text
    if "demo_rate_limit_requests_total 4" not in body:
        raise AssertionError("metrics endpoint did not expose expected request count")
    if "demo_redis_health 1" not in body:
        raise AssertionError("metrics endpoint did not expose healthy redis status")

    print("PASS docker smoke")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL {error}", file=sys.stderr)
        raise SystemExit(1)
