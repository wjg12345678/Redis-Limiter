#include "sliding_window_limiter.hpp"

#include <cstdint>
#include <cmath>
#include <cstring>
#include <exception>
#include <functional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace rrl {
namespace {

constexpr const char* kSlidingWindowLua = R"lua(
local key = KEYS[1]
local redis_time = redis.call('TIME')
local now_ms = tonumber(redis_time[1]) * 1000 + math.floor(tonumber(redis_time[2]) / 1000)
local window_ms = tonumber(ARGV[1])
local limit = tonumber(ARGV[2])
local cost = tonumber(ARGV[3])
local consume = tonumber(ARGV[4])
local member = ARGV[5]
local min_score = now_ms - window_ms

redis.call('ZREMRANGEBYSCORE', key, 0, min_score)
local current = redis.call('ZCARD', key)
local allowed = 0

if consume == 1 and current + cost <= limit then
    for i = 1, cost do
        redis.call('ZADD', key, now_ms, member .. ':' .. i)
    end
    redis.call('PEXPIRE', key, window_ms)
    current = current + cost
    allowed = 1
elseif consume == 0 and current + cost <= limit then
    allowed = 1
end

local remaining = limit - current
if remaining < 0 then
    remaining = 0
end

local oldest = redis.call('ZRANGE', key, 0, 0, 'WITHSCORES')
local reset_after = window_ms
local retry_after = 0
if oldest[2] ~= nil then
    reset_after = math.max(0, window_ms - (now_ms - tonumber(oldest[2])))
end
if current + cost > limit then
    retry_after = reset_after
end

return {allowed, current, remaining, reset_after, retry_after}
)lua";

constexpr const char* kTokenBucketLua = R"lua(
local key = KEYS[1]
local redis_time = redis.call('TIME')
local now_ms = tonumber(redis_time[1]) * 1000 + math.floor(tonumber(redis_time[2]) / 1000)
local capacity = tonumber(ARGV[1])
local refill_per_ms = tonumber(ARGV[2])
local requested = tonumber(ARGV[3])
local consume = tonumber(ARGV[4])

local state = redis.call('HMGET', key, 'tokens', 'last_ms')
local tokens = tonumber(state[1])
local last_ms = tonumber(state[2])
if tokens == nil then
    tokens = capacity
end
if last_ms == nil then
    last_ms = now_ms
end

if now_ms < last_ms then
    last_ms = now_ms
end

local elapsed = now_ms - last_ms
if elapsed > 0 then
    tokens = math.min(capacity, tokens + elapsed * refill_per_ms)
    last_ms = now_ms
end

local allowed = 0
if tokens >= requested then
    allowed = 1
    if consume == 1 then
        tokens = tokens - requested
    end
end

redis.call('HSET', key, 'tokens', tokens, 'last_ms', last_ms)
local ttl_ms = math.ceil((capacity / refill_per_ms) * 2)
if ttl_ms < 1000 then
    ttl_ms = 1000
end
redis.call('PEXPIRE', key, ttl_ms)

local remaining = math.floor(tokens)
local reset_after = 0
if tokens < capacity then
    reset_after = math.ceil((capacity - tokens) / refill_per_ms)
end

local retry_after = 0
if allowed == 0 then
    retry_after = math.ceil((requested - tokens) / refill_per_ms)
    if retry_after < 0 then
        retry_after = 0
    end
end

return {allowed, remaining, reset_after, retry_after}
)lua";

RedisReplyPtr eval_script(RedisConnection& conn,
                          const std::string& script,
                          const std::vector<std::string>& keys,
                          const std::vector<std::string>& args) {
    std::vector<std::string> parts;
    parts.reserve(3 + keys.size() + args.size());
    parts.emplace_back("EVAL");
    parts.push_back(script);
    parts.push_back(std::to_string(keys.size()));
    parts.insert(parts.end(), keys.begin(), keys.end());
    parts.insert(parts.end(), args.begin(), args.end());

    std::vector<const char*> argv;
    std::vector<size_t> argvlen;
    argv.reserve(parts.size());
    argvlen.reserve(parts.size());
    for (const auto& part : parts) {
        argv.push_back(part.data());
        argvlen.push_back(part.size());
    }

    return conn.execute(static_cast<int>(argv.size()), argv.data(), argvlen.data());
}

int require_integer(const redisReply* reply, size_t index) {
    if (!reply || reply->type != REDIS_REPLY_ARRAY || index >= static_cast<size_t>(reply->elements)) {
        throw std::runtime_error("Unexpected Redis reply shape");
    }
    const auto* item = reply->element[index];
    if (!item || item->type != REDIS_REPLY_INTEGER) {
        throw std::runtime_error("Expected integer in Redis reply");
    }
    return static_cast<int>(item->integer);
}

std::string make_member_id(int cost) {
    static const auto process_nonce = [] {
        std::random_device device;
        return (static_cast<std::uint64_t>(device()) << 32) ^ device();
    }();
    static std::atomic<std::uint64_t> sequence{0};
    std::ostringstream builder;
    builder << process_nonce
            << "-"
            << cost
            << "-"
            << std::hash<std::thread::id>{}(std::this_thread::get_id())
            << "-"
            << sequence.fetch_add(1, std::memory_order_relaxed);
    return builder.str();
}

RateLimitResult make_unavailable_result() {
    return {false, 0, 0, 0, 0, BackendStatus::Unavailable};
}

bool is_backend_unavailable(const RateLimitResult& result) {
    return result.backend_status == BackendStatus::Unavailable;
}

RateLimitResult make_fail_open_result() {
    return {true, 0, 0, 0, 0, BackendStatus::Fallback};
}

RateLimitResult make_fail_closed_result() {
    return {false, 0, 0, 1000, 1000, BackendStatus::Fallback};
}

bool is_noscript_error(const redisReply* reply) {
    return reply != nullptr &&
           reply->type == REDIS_REPLY_ERROR &&
           reply->str != nullptr &&
           std::strncmp(reply->str, "NOSCRIPT", 8) == 0;
}

std::string require_string(const redisReply* reply) {
    if (!reply || reply->type != REDIS_REPLY_STRING || reply->str == nullptr) {
        throw std::runtime_error("Expected string in Redis reply");
    }
    return reply->str;
}

RedisReplyPtr load_script(RedisConnection& conn, const std::string& script) {
    const char* argv[] = {"SCRIPT", "LOAD", script.c_str()};
    const size_t argvlen[] = {6, 4, script.size()};
    return conn.execute(3, argv, argvlen);
}

std::shared_ptr<const std::string> load_and_cache_script_sha(
    RedisConnection& conn,
    const std::string& script,
    std::shared_ptr<const std::string>& sha_cache) {
    auto load_reply = load_script(conn, script);
    if (!load_reply) {
        return nullptr;
    }
    if (load_reply->type == REDIS_REPLY_ERROR) {
        throw std::runtime_error(load_reply->str ? load_reply->str : "Redis SCRIPT LOAD error");
    }

    auto loaded_sha = std::make_shared<const std::string>(require_string(load_reply.get()));
    std::atomic_store_explicit(&sha_cache, loaded_sha, std::memory_order_release);
    return loaded_sha;
}

std::shared_ptr<const std::string> ensure_script_sha(
    RedisConnection& conn,
    const std::string& script,
    std::shared_ptr<const std::string>& sha_cache,
    std::mutex& script_mutex) {
    auto cached_sha = std::atomic_load_explicit(&sha_cache, std::memory_order_acquire);
    if (cached_sha) {
        return cached_sha;
    }

    std::lock_guard<std::mutex> script_lock(script_mutex);
    cached_sha = std::atomic_load_explicit(&sha_cache, std::memory_order_acquire);
    if (cached_sha) {
        return cached_sha;
    }

    return load_and_cache_script_sha(conn, script, sha_cache);
}

RedisReplyPtr evalsha_script(RedisConnection& conn,
                             const std::string& sha,
                             const std::vector<std::string>& keys,
                             const std::vector<std::string>& args) {
    std::vector<std::string> parts;
    parts.reserve(3 + keys.size() + args.size());
    parts.emplace_back("EVALSHA");
    parts.push_back(sha);
    parts.push_back(std::to_string(keys.size()));
    parts.insert(parts.end(), keys.begin(), keys.end());
    parts.insert(parts.end(), args.begin(), args.end());

    std::vector<const char*> argv;
    std::vector<size_t> argvlen;
    argv.reserve(parts.size());
    argvlen.reserve(parts.size());
    for (const auto& part : parts) {
        argv.push_back(part.data());
        argvlen.push_back(part.size());
    }

    return conn.execute(static_cast<int>(argv.size()), argv.data(), argvlen.data());
}

RedisReplyPtr eval_cached_script(RedisConnection& conn,
                                 const std::string& script,
                                 std::shared_ptr<const std::string>& sha_cache,
                                 std::mutex& script_mutex,
                                 const std::vector<std::string>& keys,
                                 const std::vector<std::string>& args) {
    // Keep the Redis EVALSHA path outside script_mutex; the mutex only guards SHA cache reloads.
    auto sha = ensure_script_sha(conn, script, sha_cache, script_mutex);
    if (!sha) {
        return RedisReplyPtr(nullptr, free_redis_reply);
    }

    auto reply = evalsha_script(conn, *sha, keys, args);
    if (!reply) {
        return reply;
    }
    if (is_noscript_error(reply.get())) {
        std::shared_ptr<const std::string> current_sha;
        {
            std::lock_guard<std::mutex> script_lock(script_mutex);
            current_sha = std::atomic_load_explicit(&sha_cache, std::memory_order_acquire);
            if (!current_sha || current_sha == sha) {
                current_sha = load_and_cache_script_sha(conn, script, sha_cache);
                if (!current_sha) {
                    return RedisReplyPtr(nullptr, free_redis_reply);
                }
            }
        }
        reply = evalsha_script(conn, *current_sha, keys, args);
    }
    return reply;
}

RateLimitResult execute_sliding_window_script(RedisConnection& conn,
                                              std::shared_ptr<const std::string>& script_sha,
                                              std::mutex& script_mutex,
                                              const std::string& redis_key,
                                              int window_size_ms,
                                              int max_requests,
                                              int effective_cost,
                                              bool consume) {
    auto reply = eval_cached_script(conn,
                                    kSlidingWindowLua,
                                    script_sha,
                                    script_mutex,
                                    {redis_key},
                                    {std::to_string(window_size_ms),
                                     std::to_string(max_requests),
                                     std::to_string(effective_cost),
                                     consume ? "1" : "0",
                                     make_member_id(effective_cost)});

    if (!reply) {
        return make_unavailable_result();
    }
    if (reply->type == REDIS_REPLY_ERROR) {
        throw std::runtime_error(reply->str ? reply->str : "Redis EVAL error");
    }

    RateLimitResult result;
    result.allowed = require_integer(reply.get(), 0) == 1;
    result.current_count = require_integer(reply.get(), 1);
    result.remaining = require_integer(reply.get(), 2);
    result.reset_after_ms = require_integer(reply.get(), 3);
    result.retry_after_ms = require_integer(reply.get(), 4);
    result.backend_status = BackendStatus::Healthy;
    return result;
}

}  // namespace

SlidingWindowLimiter::SlidingWindowLimiter(std::shared_ptr<RedisPool> pool,
                                           const RateLimitConfig& config)
    : pool_(std::move(pool)), config_(config) {
    if (!pool_) {
        throw std::invalid_argument("SlidingWindowLimiter requires a Redis pool");
    }
    if (config_.max_requests <= 0 || config_.window_size_ms <= 0) {
        throw std::invalid_argument("Invalid sliding window configuration");
    }
}

RateLimitResult SlidingWindowLimiter::allow(const std::string& key) {
    return allow(key, 1);
}

RateLimitResult SlidingWindowLimiter::allow(const std::string& key, int cost) {
    return check_sliding_window(key, cost);
}

RateLimitResult SlidingWindowLimiter::peek(const std::string& key) {
    return check_sliding_window(key, 0);
}

bool SlidingWindowLimiter::reset(const std::string& key) {
    RedisConnectionGuard guard(*pool_);
    if (!guard.is_valid()) {
        return false;
    }

    const std::string redis_key = config_.key_prefix + key;
    auto reply = guard->execute("DEL %s", redis_key.c_str());
    return reply && reply->type == REDIS_REPLY_INTEGER;
}

std::vector<RateLimitResult> SlidingWindowLimiter::allow_batch(const std::vector<std::string>& keys,
                                                               const std::vector<int>& costs) {
    if (keys.size() != costs.size()) {
        throw std::invalid_argument("keys and costs must have the same length");
    }

    std::vector<RateLimitResult> results;
    results.reserve(keys.size());
    for (size_t i = 0; i < keys.size(); ++i) {
        results.push_back(allow(keys[i], costs[i]));
    }
    return results;
}

void SlidingWindowLimiter::update_config(const RateLimitConfig& config) {
    if (config.max_requests <= 0 || config.window_size_ms <= 0) {
        throw std::invalid_argument("Invalid sliding window configuration");
    }

    std::lock_guard<std::mutex> lock(config_mutex_);
    config_ = config;
}

RateLimitResult SlidingWindowLimiter::check_sliding_window(const std::string& key, int cost) {
    if (cost < 0) {
        throw std::invalid_argument("cost must be non-negative");
    }

    RateLimitConfig local_config;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        local_config = config_;
    }

    RedisConnectionGuard guard(*pool_);
    if (!guard.is_valid()) {
        return make_unavailable_result();
    }

    const bool consume = cost > 0;
    const int effective_cost = consume ? cost : 1;
    const std::string redis_key = local_config.key_prefix + key;
    return execute_sliding_window_script(*guard,
                                         script_sha_,
                                         script_mutex_,
                                         redis_key,
                                         local_config.window_size_ms,
                                         local_config.max_requests,
                                         effective_cost,
                                         consume);
}

TokenBucketLimiter::TokenBucketLimiter(std::shared_ptr<RedisPool> pool,
                                       int max_tokens,
                                       double refill_rate,
                                       std::string key_prefix)
    : pool_(std::move(pool)),
      max_tokens_(max_tokens),
      refill_rate_(refill_rate),
      key_prefix_(std::move(key_prefix)) {
    if (!pool_) {
        throw std::invalid_argument("TokenBucketLimiter requires a Redis pool");
    }
    if (max_tokens_ <= 0 || refill_rate_ <= 0.0) {
        throw std::invalid_argument("Invalid token bucket configuration");
    }
}

RateLimitResult TokenBucketLimiter::allow(const std::string& key) {
    return allow(key, 1);
}

RateLimitResult TokenBucketLimiter::allow(const std::string& key, int tokens_needed) {
    return execute_bucket_script(key, tokens_needed, true);
}

RateLimitResult TokenBucketLimiter::peek(const std::string& key) {
    return execute_bucket_script(key, 1, false);
}

bool TokenBucketLimiter::reset(const std::string& key) {
    RedisConnectionGuard guard(*pool_);
    if (!guard.is_valid()) {
        return false;
    }

    const std::string redis_key = key_prefix_ + key;
    auto reply = guard->execute("DEL %s", redis_key.c_str());
    return reply && reply->type == REDIS_REPLY_INTEGER;
}

void TokenBucketLimiter::update_limits(int max_tokens, double refill_rate) {
    if (max_tokens <= 0 || refill_rate <= 0.0) {
        throw std::invalid_argument("Invalid token bucket configuration");
    }

    std::lock_guard<std::mutex> lock(config_mutex_);
    max_tokens_ = max_tokens;
    refill_rate_ = refill_rate;
}

RateLimitResult TokenBucketLimiter::execute_bucket_script(const std::string& key,
                                                          int tokens_needed,
                                                          bool consume) {
    if (tokens_needed <= 0) {
        throw std::invalid_argument("tokens_needed must be positive");
    }

    int max_tokens = 0;
    double refill_rate = 0.0;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        max_tokens = max_tokens_;
        refill_rate = refill_rate_;
    }

    RedisConnectionGuard guard(*pool_);
    if (!guard.is_valid()) {
        return make_unavailable_result();
    }

    const std::string redis_key = key_prefix_ + key;
    const double refill_per_ms = refill_rate / 1000.0;
    auto reply = eval_cached_script(*guard,
                                    kTokenBucketLua,
                                    script_sha_,
                                    script_mutex_,
                                    {redis_key},
                                    {std::to_string(max_tokens),
                                     std::to_string(refill_per_ms),
                                     std::to_string(tokens_needed),
                                     consume ? "1" : "0"});

    if (!reply) {
        return make_unavailable_result();
    }
    if (reply->type == REDIS_REPLY_ERROR) {
        throw std::runtime_error(reply->str ? reply->str : "Redis EVAL error");
    }

    const int remaining = require_integer(reply.get(), 1);
    RateLimitResult result;
    result.allowed = require_integer(reply.get(), 0) == 1;
    result.current_count = max_tokens - remaining;
    result.remaining = remaining;
    result.reset_after_ms = require_integer(reply.get(), 2);
    result.retry_after_ms = require_integer(reply.get(), 3);
    result.backend_status = BackendStatus::Healthy;
    return result;
}

std::shared_ptr<SlidingWindowLimiter> RateLimiterFactory::create_sliding_window(
    const RedisConfig& redis_config,
    const RateLimitConfig& rate_config) {
    auto pool = std::make_shared<RedisPool>(redis_config);
    return std::make_shared<SlidingWindowLimiter>(std::move(pool), rate_config);
}

std::shared_ptr<TokenBucketLimiter> RateLimiterFactory::create_token_bucket(const RedisConfig& redis_config,
                                                                             int max_tokens,
                                                                             double refill_rate) {
    auto pool = std::make_shared<RedisPool>(redis_config);
    return std::make_shared<TokenBucketLimiter>(std::move(pool), max_tokens, refill_rate);
}

LocalTokenBucketLimiter::LocalTokenBucketLimiter(int max_tokens, double refill_rate)
    : max_tokens_(max_tokens), refill_rate_(refill_rate) {
    if (max_tokens_ <= 0 || refill_rate_ <= 0.0) {
        throw std::invalid_argument("Invalid local token bucket configuration");
    }
}

RateLimitResult LocalTokenBucketLimiter::allow(const std::string& key) {
    return allow(key, 1);
}

RateLimitResult LocalTokenBucketLimiter::allow(const std::string& key, int tokens_needed) {
    if (tokens_needed <= 0) {
        throw std::invalid_argument("tokens_needed must be positive");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto& bucket = buckets_[key];
    const auto now = std::chrono::steady_clock::now();

    if (bucket.last_refill.time_since_epoch().count() == 0) {
        bucket.tokens = static_cast<double>(max_tokens_);
        bucket.last_refill = now;
    }

    const double elapsed_seconds = std::chrono::duration<double>(now - bucket.last_refill).count();
    if (elapsed_seconds > 0.0) {
        bucket.tokens = std::min<double>(max_tokens_, bucket.tokens + (elapsed_seconds * refill_rate_));
        bucket.last_refill = now;
    }

    RateLimitResult result;
    result.allowed = bucket.tokens >= tokens_needed;
    if (result.allowed) {
        bucket.tokens -= tokens_needed;
    }

    result.remaining = std::max(0, static_cast<int>(std::floor(bucket.tokens)));
    result.current_count = max_tokens_ - result.remaining;
    if (bucket.tokens < max_tokens_) {
        result.reset_after_ms =
            static_cast<int>(std::ceil(((max_tokens_ - bucket.tokens) / refill_rate_) * 1000.0));
    } else {
        result.reset_after_ms = 0;
    }

    if (!result.allowed) {
        result.retry_after_ms =
            static_cast<int>(std::ceil(((tokens_needed - bucket.tokens) / refill_rate_) * 1000.0));
        if (result.retry_after_ms < 0) {
            result.retry_after_ms = 0;
        }
    } else {
        result.retry_after_ms = 0;
    }

    result.backend_status = BackendStatus::Fallback;

    return result;
}

void LocalTokenBucketLimiter::update_limits(int max_tokens, double refill_rate) {
    if (max_tokens <= 0 || refill_rate <= 0.0) {
        throw std::invalid_argument("Invalid local token bucket configuration");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    max_tokens_ = max_tokens;
    refill_rate_ = refill_rate;
    for (auto& [_, bucket] : buckets_) {
        bucket.tokens = std::min<double>(bucket.tokens, max_tokens_);
    }
}

ResilientTokenBucketLimiter::ResilientTokenBucketLimiter(std::shared_ptr<TokenBucketLimiter> remote_limiter,
                                                         FallbackMode fallback_mode,
                                                         int local_max_tokens,
                                                         double local_refill_rate)
    : remote_limiter_(std::move(remote_limiter)),
      local_fallback_(local_max_tokens, local_refill_rate),
      fallback_mode_(fallback_mode) {
    if (!remote_limiter_) {
        throw std::invalid_argument("ResilientTokenBucketLimiter requires a remote limiter");
    }
}

RateLimitResult ResilientTokenBucketLimiter::allow(const std::string& key) {
    return allow(key, 1);
}

RateLimitResult ResilientTokenBucketLimiter::allow(const std::string& key, int tokens_needed) {
    try {
        const auto result = remote_limiter_->allow(key, tokens_needed);
        if (is_backend_unavailable(result)) {
            redis_error_count_++;
            return fallback_result(key, tokens_needed, nullptr);
        }
        return result;
    } catch (const std::exception& error) {
        redis_error_count_++;
        return fallback_result(key, tokens_needed, &error);
    } catch (...) {
        redis_error_count_++;
        return fallback_result(key, tokens_needed, nullptr);
    }
}

void ResilientTokenBucketLimiter::update_fallback_mode(FallbackMode fallback_mode) {
    fallback_mode_.store(fallback_mode);
}

FallbackMode ResilientTokenBucketLimiter::fallback_mode() const {
    return fallback_mode_.load();
}

uint64_t ResilientTokenBucketLimiter::redis_error_count() const {
    return redis_error_count_.load();
}

uint64_t ResilientTokenBucketLimiter::fallback_hit_count() const {
    return fallback_hit_count_.load();
}

RateLimitResult ResilientTokenBucketLimiter::fallback_result(const std::string& key,
                                                             int tokens_needed,
                                                             const std::exception* error) {
    (void)error;
    switch (fallback_mode_.load()) {
        case FallbackMode::FailOpen:
            fallback_hit_count_++;
            return make_fail_open_result();
        case FallbackMode::FailClosed:
            fallback_hit_count_++;
            return make_fail_closed_result();
        case FallbackMode::LocalTokenBucket:
        default:
            fallback_hit_count_++;
            return local_fallback_.allow(key, tokens_needed);
    }
}

std::shared_ptr<ResilientTokenBucketLimiter> RateLimiterFactory::create_resilient_token_bucket(
    const RedisConfig& redis_config,
    int max_tokens,
    double refill_rate,
    FallbackMode fallback_mode,
    int local_max_tokens,
    double local_refill_rate) {
    auto remote = create_token_bucket(redis_config, max_tokens, refill_rate);
    return std::make_shared<ResilientTokenBucketLimiter>(
        std::move(remote), fallback_mode, local_max_tokens, local_refill_rate);
}

}  // namespace rrl
