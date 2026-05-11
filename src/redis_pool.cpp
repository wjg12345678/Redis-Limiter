#include "redis_pool.hpp"

#include <algorithm>
#include <cstdarg>
#include <stdexcept>

namespace rrl {
namespace {

struct timeval timeout_from_ms(int timeout_ms) {
    const int safe_timeout_ms = std::max(0, timeout_ms);
    struct timeval tv;
    tv.tv_sec = safe_timeout_ms / 1000;
    tv.tv_usec = (safe_timeout_ms % 1000) * 1000;
    return tv;
}

int connection_attempts(const RedisConfig& config) {
    return std::max(1, config.max_retries + 1);
}

void sleep_before_retry(int failed_attempt) {
    const int delay_ms = std::min(100, 10 * (failed_attempt + 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
}

redisContext* open_redis_context(const RedisConfig& config) {
    auto connect_timeout = timeout_from_ms(config.connect_timeout_ms);
    redisContext* ctx = redisConnectWithTimeout(config.host.c_str(), config.port, connect_timeout);
    if (!ctx || ctx->err) {
        if (ctx) {
            redisFree(ctx);
        }
        return nullptr;
    }

    auto socket_timeout = timeout_from_ms(config.socket_timeout_ms);
    if (redisSetTimeout(ctx, socket_timeout) != REDIS_OK) {
        redisFree(ctx);
        return nullptr;
    }

    if (!config.password.empty()) {
        auto* reply = static_cast<redisReply*>(redisCommand(ctx, "AUTH %s", config.password.c_str()));
        const bool ok = reply && reply->type != REDIS_REPLY_ERROR;
        if (reply) {
            freeReplyObject(reply);
        }
        if (!ok) {
            redisFree(ctx);
            return nullptr;
        }
    }

    if (config.db != 0) {
        auto* reply = static_cast<redisReply*>(redisCommand(ctx, "SELECT %d", config.db));
        const bool ok = reply && reply->type != REDIS_REPLY_ERROR;
        if (reply) {
            freeReplyObject(reply);
        }
        if (!ok) {
            redisFree(ctx);
            return nullptr;
        }
    }

    return ctx;
}

}  // namespace

RedisConnection::RedisConnection(redisContext* ctx) : ctx_(ctx) {}

RedisConnection::RedisConnection(redisContext* ctx, const RedisConfig& config)
    : ctx_(ctx), config_(std::make_shared<const RedisConfig>(config)) {}

RedisConnection::~RedisConnection() {
    close();
}

void RedisConnection::close() {
    if (ctx_) {
        redisFree(ctx_);
        ctx_ = nullptr;
    }
}

bool RedisConnection::reconnect_once() {
    if (!config_) {
        return false;
    }

    close();
    ctx_ = open_redis_context(*config_);
    return is_valid();
}

bool RedisConnection::ensure_connected() {
    if (is_valid()) {
        return true;
    }
    if (!config_) {
        return false;
    }

    const int attempts = connection_attempts(*config_);
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (attempt > 0) {
            sleep_before_retry(attempt - 1);
        }
        if (reconnect_once()) {
            return true;
        }
    }
    return false;
}

RedisReplyPtr RedisConnection::execute(const char* format, ...) {
    if (!ensure_connected()) {
        return RedisReplyPtr(nullptr, free_redis_reply);
    }

    va_list ap;
    va_start(ap, format);
    redisReply* reply = static_cast<redisReply*>(redisvCommand(ctx_, format, ap));
    va_end(ap);
    return RedisReplyPtr(reply, free_redis_reply);
}

RedisReplyPtr RedisConnection::execute(int argc, const char** argv, const size_t* argvlen) {
    if (!ensure_connected()) {
        return RedisReplyPtr(nullptr, free_redis_reply);
    }

    redisReply* reply = static_cast<redisReply*>(redisCommandArgv(ctx_, argc, argv, argvlen));
    return RedisReplyPtr(reply, free_redis_reply);
}

RedisPool::RedisPool(const RedisConfig& config) : config_(config) {
    if (config_.pool_size <= 0) {
        throw std::invalid_argument("Redis pool size must be positive");
    }

    for (int i = 0; i < config_.pool_size; ++i) {
        auto conn = create_connection(config_);
        if (conn.is_valid()) {
            pool_.push(std::move(conn));
            stats_.total_connections++;
        }
    }

    maintainer_thread_ = std::thread(&RedisPool::maintain_pool, this);
}

RedisPool::~RedisPool() {
    shutdown_ = true;
    cv_.notify_all();
    if (maintainer_thread_.joinable()) {
        maintainer_thread_.join();
    }

    std::lock_guard<std::mutex> lock(pool_mutex_);
    while (!pool_.empty()) {
        pool_.pop();
    }
}

RedisConnection RedisPool::create_connection() {
    RedisConfig config_snapshot;
    {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        config_snapshot = config_;
    }
    return create_connection(config_snapshot);
}

RedisConnection RedisPool::create_connection(const RedisConfig& config) {
    const int attempts = connection_attempts(config);
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (attempt > 0) {
            sleep_before_retry(attempt - 1);
        }
        redisContext* ctx = open_redis_context(config);
        if (ctx) {
            return RedisConnection(ctx, config);
        }
    }
    return RedisConnection(nullptr);
}

void RedisPool::decrement_total_connections() {
    size_t current = stats_.total_connections.load();
    while (current > 0 &&
           !stats_.total_connections.compare_exchange_weak(current, current - 1)) {
    }
}

RedisConnection RedisPool::acquire(std::chrono::milliseconds timeout) {
    stats_.total_requests++;
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    while (true) {
        RedisConfig config_snapshot;
        bool should_create = false;

        {
            std::unique_lock<std::mutex> lock(pool_mutex_);
            if (shutdown_) {
                stats_.failed_requests++;
                return RedisConnection(nullptr);
            }

            if (!pool_.empty()) {
                auto conn = std::move(pool_.front());
                pool_.pop();
                if (conn.is_valid()) {
                    stats_.active_connections++;
                    return conn;
                }

                decrement_total_connections();
                stats_.failed_requests++;
                continue;
            }

            if (stats_.total_connections.load() < static_cast<size_t>(config_.pool_size)) {
                stats_.total_connections++;
                config_snapshot = config_;
                should_create = true;
            } else {
                stats_.wait_count++;
                if (cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
                    stats_.failed_requests++;
                    return RedisConnection(nullptr);
                }
                continue;
            }
        }

        if (!should_create) {
            continue;
        }

        auto conn = create_connection(config_snapshot);
        if (conn.is_valid()) {
            stats_.active_connections++;
            return conn;
        }

        decrement_total_connections();
        {
            std::lock_guard<std::mutex> lock(pool_mutex_);
            cv_.notify_one();
        }
        stats_.failed_requests++;
        return RedisConnection(nullptr);
    }
}

void RedisPool::release(RedisConnection conn) {
    if (!conn.is_valid()) {
        stats_.active_connections--;
        decrement_total_connections();
        stats_.failed_requests++;
        cv_.notify_one();
        return;
    }

    std::lock_guard<std::mutex> lock(pool_mutex_);
    if (shutdown_) {
        stats_.active_connections--;
        decrement_total_connections();
        return;
    }

    pool_.push(std::move(conn));
    stats_.active_connections--;
    cv_.notify_one();
}

PoolStatsSnapshot RedisPool::get_stats() const {
    PoolStatsSnapshot snapshot;
    snapshot.total_connections = stats_.total_connections.load();
    snapshot.active_connections = stats_.active_connections.load();
    snapshot.wait_count = stats_.wait_count.load();
    snapshot.total_requests = stats_.total_requests.load();
    snapshot.failed_requests = stats_.failed_requests.load();
    return snapshot;
}

bool RedisPool::health_check() {
    std::queue<RedisConnection> candidates;
    {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        std::swap(candidates, pool_);
    }

    std::queue<RedisConnection> healthy_connections;
    bool healthy = true;
    size_t dropped = 0;

    while (!candidates.empty()) {
        auto conn = std::move(candidates.front());
        candidates.pop();

        const auto reply = conn.is_valid() ? conn.execute("PING") : RedisReplyPtr(nullptr, free_redis_reply);
        if (reply && reply->type == REDIS_REPLY_STATUS &&
            reply->str != nullptr &&
            std::string(reply->str) == "PONG") {
            healthy_connections.push(std::move(conn));
        } else {
            dropped++;
            healthy = false;
        }
    }

    for (size_t i = 0; i < dropped; ++i) {
        decrement_total_connections();
    }

    {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        while (!healthy_connections.empty()) {
            pool_.push(std::move(healthy_connections.front()));
            healthy_connections.pop();
        }
        cv_.notify_all();
    }

    while (!shutdown_) {
        RedisConfig config_snapshot;
        {
            std::lock_guard<std::mutex> lock(pool_mutex_);
            if (shutdown_ ||
                stats_.total_connections.load() >= static_cast<size_t>(config_.pool_size)) {
                break;
            }
            stats_.total_connections++;
            config_snapshot = config_;
        }

        auto conn = create_connection(config_snapshot);
        if (!conn.is_valid()) {
            decrement_total_connections();
            stats_.failed_requests++;
            healthy = false;
            break;
        }

        std::lock_guard<std::mutex> lock(pool_mutex_);
        if (shutdown_) {
            decrement_total_connections();
            healthy = false;
            break;
        }
        pool_.push(std::move(conn));
        cv_.notify_one();
    }

    {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        return healthy &&
               stats_.total_connections.load() >= static_cast<size_t>(config_.pool_size);
    }
}

void RedisPool::resize(size_t new_size) {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    if (new_size == 0) {
        throw std::invalid_argument("Redis pool size must be positive");
    }

    if (new_size > pool_.size()) {
        const size_t need = new_size - pool_.size();
        for (size_t i = 0; i < need; ++i) {
            auto conn = create_connection(config_);
            if (conn.is_valid()) {
                pool_.push(std::move(conn));
                stats_.total_connections++;
            }
        }
    } else if (new_size < pool_.size()) {
        const size_t remove = pool_.size() - new_size;
        for (size_t i = 0; i < remove; ++i) {
            pool_.pop();
            stats_.total_connections--;
        }
    }

    config_.pool_size = static_cast<int>(new_size);
}

void RedisPool::maintain_pool() {
    std::unique_lock<std::mutex> lock(pool_mutex_);
    while (!shutdown_) {
        if (cv_.wait_for(lock, std::chrono::seconds(30), [this] { return shutdown_.load(); })) {
            break;
        }

        lock.unlock();
        health_check();
        lock.lock();
    }
}

} // namespace rrl
