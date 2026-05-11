#pragma once

#include "redis_pool.hpp"

#include <chrono>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace rrl {

enum class BackendStatus {
    Healthy,
    Unavailable,
    Fallback,
};

// 限流结果
struct RateLimitResult {
    bool allowed;           // 是否允许通过
    int current_count;      // 当前窗口内请求数
    int remaining;          // 剩余配额
    int reset_after_ms;     // 窗口重置时间(ms)
    int retry_after_ms;     // 建议重试时间(ms), 被拒绝时有效
    BackendStatus backend_status = BackendStatus::Healthy;  // 后端状态
};

// 限流配置
struct RateLimitConfig {
    int max_requests = 100;           // 窗口内最大请求数
    int window_size_ms = 1000;        // 窗口大小(ms)
    std::string key_prefix = "ratelimit:";  // Redis key前缀
};

// 滑动窗口限流器
class SlidingWindowLimiter {
public:
    explicit SlidingWindowLimiter(std::shared_ptr<RedisPool> pool,
                                   const RateLimitConfig& config = {});

    // 检查是否允许请求通过
    RateLimitResult allow(const std::string& key);

    // 带自定义数量的检查
    RateLimitResult allow(const std::string& key, int cost);

    // 获取当前配额状态(不消耗配额)
    RateLimitResult peek(const std::string& key);

    // 重置指定key的限流计数
    bool reset(const std::string& key);

    // 批量检查(用于批量请求场景)
    std::vector<RateLimitResult> allow_batch(
        const std::vector<std::string>& keys,
        const std::vector<int>& costs);

    // 更新配置
    void update_config(const RateLimitConfig& config);

private:
    std::shared_ptr<RedisPool> pool_;
    RateLimitConfig config_;
    mutable std::mutex config_mutex_;
    mutable std::mutex script_mutex_;
    std::shared_ptr<const std::string> script_sha_;

    // 使用Redis Lua脚本实现原子性滑动窗口
    RateLimitResult check_sliding_window(const std::string& key, int cost);
};

// 令牌桶限流器(备选实现)
class TokenBucketLimiter {
public:
    explicit TokenBucketLimiter(std::shared_ptr<RedisPool> pool,
                                int max_tokens = 100,
                                double refill_rate = 10.0,
                                std::string key_prefix = "tokenbucket:");

    RateLimitResult allow(const std::string& key);
    RateLimitResult allow(const std::string& key, int tokens_needed);
    RateLimitResult peek(const std::string& key);
    bool reset(const std::string& key);
    void update_limits(int max_tokens, double refill_rate);

private:
    RateLimitResult execute_bucket_script(const std::string& key, int tokens_needed, bool consume);

    std::shared_ptr<RedisPool> pool_;
    int max_tokens_;
    double refill_rate_;
    std::string key_prefix_;
    mutable std::mutex config_mutex_;
    mutable std::mutex script_mutex_;
    std::shared_ptr<const std::string> script_sha_;
};

enum class FallbackMode {
    FailOpen,
    FailClosed,
    LocalTokenBucket,
};

class LocalTokenBucketLimiter {
public:
    explicit LocalTokenBucketLimiter(int max_tokens = 100,
                                     double refill_rate = 10.0);

    RateLimitResult allow(const std::string& key);
    RateLimitResult allow(const std::string& key, int tokens_needed);
    void update_limits(int max_tokens, double refill_rate);

private:
    struct BucketState {
        double tokens = 0.0;
        std::chrono::steady_clock::time_point last_refill{};
    };

    int max_tokens_;
    double refill_rate_;
    std::unordered_map<std::string, BucketState> buckets_;
    mutable std::mutex mutex_;
};

class ResilientTokenBucketLimiter {
public:
    explicit ResilientTokenBucketLimiter(std::shared_ptr<TokenBucketLimiter> remote_limiter,
                                         FallbackMode fallback_mode = FallbackMode::LocalTokenBucket,
                                         int local_max_tokens = 50,
                                         double local_refill_rate = 5.0);

    RateLimitResult allow(const std::string& key);
    RateLimitResult allow(const std::string& key, int tokens_needed);
    void update_fallback_mode(FallbackMode fallback_mode);
    FallbackMode fallback_mode() const;
    uint64_t redis_error_count() const;
    uint64_t fallback_hit_count() const;

private:
    RateLimitResult fallback_result(const std::string& key, int tokens_needed, const std::exception* error);

    std::shared_ptr<TokenBucketLimiter> remote_limiter_;
    LocalTokenBucketLimiter local_fallback_;
    std::atomic<FallbackMode> fallback_mode_;
    std::atomic<uint64_t> redis_error_count_{0};
    std::atomic<uint64_t> fallback_hit_count_{0};
};

// 限流器工厂
class RateLimiterFactory {
public:
    static std::shared_ptr<SlidingWindowLimiter> create_sliding_window(
        const RedisConfig& redis_config,
        const RateLimitConfig& rate_config = {});

    static std::shared_ptr<TokenBucketLimiter> create_token_bucket(
        const RedisConfig& redis_config,
        int max_tokens = 100,
        double refill_rate = 10.0);

    static std::shared_ptr<ResilientTokenBucketLimiter> create_resilient_token_bucket(
        const RedisConfig& redis_config,
        int max_tokens = 100,
        double refill_rate = 10.0,
        FallbackMode fallback_mode = FallbackMode::LocalTokenBucket,
        int local_max_tokens = 50,
        double local_refill_rate = 5.0);
};

} // namespace rrl
