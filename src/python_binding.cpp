#include "redis_pool.hpp"
#include "sliding_window_limiter.hpp"

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <memory>

namespace py = pybind11;

PYBIND11_MODULE(redis_limiter, m) {
    using namespace rrl;

    py::class_<RedisConfig>(m, "RedisConfig")
        .def(py::init<>())
        .def_readwrite("host", &RedisConfig::host)
        .def_readwrite("port", &RedisConfig::port)
        .def_readwrite("password", &RedisConfig::password)
        .def_readwrite("db", &RedisConfig::db)
        .def_readwrite("connect_timeout_ms", &RedisConfig::connect_timeout_ms)
        .def_readwrite("socket_timeout_ms", &RedisConfig::socket_timeout_ms)
        .def_readwrite("pool_size", &RedisConfig::pool_size)
        .def_readwrite("max_retries", &RedisConfig::max_retries);

    py::class_<PoolStatsSnapshot>(m, "PoolStats")
        .def_readonly("total_connections", &PoolStatsSnapshot::total_connections)
        .def_readonly("active_connections", &PoolStatsSnapshot::active_connections)
        .def_readonly("wait_count", &PoolStatsSnapshot::wait_count)
        .def_readonly("total_requests", &PoolStatsSnapshot::total_requests)
        .def_readonly("failed_requests", &PoolStatsSnapshot::failed_requests);

    py::class_<RedisPool, std::shared_ptr<RedisPool>>(m, "RedisPool")
        .def(py::init<const RedisConfig&>(), py::call_guard<py::gil_scoped_release>())
        .def("health_check", &RedisPool::health_check, py::call_guard<py::gil_scoped_release>())
        .def("resize", &RedisPool::resize, py::call_guard<py::gil_scoped_release>())
        .def("get_stats", &RedisPool::get_stats);

    py::class_<RateLimitConfig>(m, "RateLimitConfig")
        .def(py::init<>())
        .def_readwrite("max_requests", &RateLimitConfig::max_requests)
        .def_readwrite("window_size_ms", &RateLimitConfig::window_size_ms)
        .def_readwrite("key_prefix", &RateLimitConfig::key_prefix);

    py::class_<RateLimitResult>(m, "RateLimitResult")
        .def_readonly("allowed", &RateLimitResult::allowed)
        .def_readonly("current_count", &RateLimitResult::current_count)
        .def_readonly("remaining", &RateLimitResult::remaining)
        .def_readonly("reset_after_ms", &RateLimitResult::reset_after_ms)
        .def_readonly("retry_after_ms", &RateLimitResult::retry_after_ms)
        .def_readonly("backend_status", &RateLimitResult::backend_status);

    py::enum_<BackendStatus>(m, "BackendStatus")
        .value("Healthy", BackendStatus::Healthy)
        .value("Unavailable", BackendStatus::Unavailable)
        .value("Fallback", BackendStatus::Fallback)
        .export_values();

    py::enum_<FallbackMode>(m, "FallbackMode")
        .value("FailOpen", FallbackMode::FailOpen)
        .value("FailClosed", FallbackMode::FailClosed)
        .value("LocalTokenBucket", FallbackMode::LocalTokenBucket)
        .export_values();

    py::class_<SlidingWindowLimiter, std::shared_ptr<SlidingWindowLimiter>>(m, "SlidingWindowLimiter")
        .def(py::init<std::shared_ptr<RedisPool>, const RateLimitConfig&>(),
             py::arg("pool"),
             py::arg("config") = RateLimitConfig{})
        .def("allow",
             py::overload_cast<const std::string&>(&SlidingWindowLimiter::allow),
             py::arg("key"),
             py::call_guard<py::gil_scoped_release>())
        .def("allow",
             py::overload_cast<const std::string&, int>(&SlidingWindowLimiter::allow),
             py::arg("key"),
             py::arg("cost"),
             py::call_guard<py::gil_scoped_release>())
        .def("peek", &SlidingWindowLimiter::peek, py::call_guard<py::gil_scoped_release>())
        .def("reset", &SlidingWindowLimiter::reset, py::call_guard<py::gil_scoped_release>())
        .def("allow_batch", &SlidingWindowLimiter::allow_batch, py::call_guard<py::gil_scoped_release>())
        .def("update_config", &SlidingWindowLimiter::update_config);

    py::class_<TokenBucketLimiter, std::shared_ptr<TokenBucketLimiter>>(m, "TokenBucketLimiter")
        .def(py::init<std::shared_ptr<RedisPool>, int, double, std::string>(),
             py::arg("pool"),
             py::arg("max_tokens") = 100,
             py::arg("refill_rate") = 10.0,
             py::arg("key_prefix") = "tokenbucket:")
        .def("allow",
             py::overload_cast<const std::string&>(&TokenBucketLimiter::allow),
             py::arg("key"),
             py::call_guard<py::gil_scoped_release>())
        .def("allow",
             py::overload_cast<const std::string&, int>(&TokenBucketLimiter::allow),
             py::arg("key"),
             py::arg("tokens_needed"),
             py::call_guard<py::gil_scoped_release>())
        .def("peek", &TokenBucketLimiter::peek, py::call_guard<py::gil_scoped_release>())
        .def("reset", &TokenBucketLimiter::reset, py::call_guard<py::gil_scoped_release>())
        .def("update_limits", &TokenBucketLimiter::update_limits);

    py::class_<LocalTokenBucketLimiter>(m, "LocalTokenBucketLimiter")
        .def(py::init<int, double>(),
             py::arg("max_tokens") = 100,
             py::arg("refill_rate") = 10.0)
        .def("allow",
             py::overload_cast<const std::string&>(&LocalTokenBucketLimiter::allow),
             py::arg("key"),
             py::call_guard<py::gil_scoped_release>())
        .def("allow",
             py::overload_cast<const std::string&, int>(&LocalTokenBucketLimiter::allow),
             py::arg("key"),
             py::arg("tokens_needed"),
             py::call_guard<py::gil_scoped_release>())
        .def("update_limits", &LocalTokenBucketLimiter::update_limits);

    py::class_<ResilientTokenBucketLimiter, std::shared_ptr<ResilientTokenBucketLimiter>>(
        m, "ResilientTokenBucketLimiter")
        .def(py::init<std::shared_ptr<TokenBucketLimiter>, FallbackMode, int, double>(),
             py::arg("remote_limiter"),
             py::arg("fallback_mode") = FallbackMode::LocalTokenBucket,
             py::arg("local_max_tokens") = 50,
             py::arg("local_refill_rate") = 5.0)
        .def("allow",
             py::overload_cast<const std::string&>(&ResilientTokenBucketLimiter::allow),
             py::arg("key"),
             py::call_guard<py::gil_scoped_release>())
        .def("allow",
             py::overload_cast<const std::string&, int>(&ResilientTokenBucketLimiter::allow),
             py::arg("key"),
             py::arg("tokens_needed"),
             py::call_guard<py::gil_scoped_release>())
        .def("update_fallback_mode", &ResilientTokenBucketLimiter::update_fallback_mode)
        .def("fallback_mode", &ResilientTokenBucketLimiter::fallback_mode)
        .def("redis_error_count", &ResilientTokenBucketLimiter::redis_error_count)
        .def("fallback_hit_count", &ResilientTokenBucketLimiter::fallback_hit_count);

    py::class_<RateLimiterFactory>(m, "RateLimiterFactory")
        .def_static("create_sliding_window",
                    &RateLimiterFactory::create_sliding_window,
                    py::arg("redis_config"),
                    py::arg("rate_config") = RateLimitConfig{},
                    py::call_guard<py::gil_scoped_release>())
        .def_static("create_token_bucket",
                    &RateLimiterFactory::create_token_bucket,
                    py::arg("redis_config"),
                    py::arg("max_tokens") = 100,
                    py::arg("refill_rate") = 10.0,
                    py::call_guard<py::gil_scoped_release>())
        .def_static("create_resilient_token_bucket",
                    &RateLimiterFactory::create_resilient_token_bucket,
                    py::arg("redis_config"),
                    py::arg("max_tokens") = 100,
                    py::arg("refill_rate") = 10.0,
                    py::arg("fallback_mode") = FallbackMode::LocalTokenBucket,
                    py::arg("local_max_tokens") = 50,
                    py::arg("local_refill_rate") = 5.0,
                    py::call_guard<py::gil_scoped_release>());
}
