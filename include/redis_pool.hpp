#pragma once

#include <hiredis/hiredis.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace rrl {

inline void free_redis_reply(redisReply* reply) {
    freeReplyObject(reply);
}

using RedisReplyPtr = std::unique_ptr<redisReply, void (*)(redisReply*)>;

// Redis连接配置
struct RedisConfig {
    std::string host = "127.0.0.1";
    int port = 6379;
    std::string password;
    int db = 0;
    int connect_timeout_ms = 1000;
    int socket_timeout_ms = 1000;
    int pool_size = 10;
    int max_retries = 3;
};

// Redis连接包装器(RAII管理)
class RedisConnection {
public:
    explicit RedisConnection(redisContext* ctx = nullptr);
    RedisConnection(redisContext* ctx, const RedisConfig& config);
    ~RedisConnection();

    // 禁用拷贝，允许移动
    RedisConnection(const RedisConnection&) = delete;
    RedisConnection& operator=(const RedisConnection&) = delete;

    RedisConnection(RedisConnection&& other) noexcept
        : ctx_(other.ctx_), config_(std::move(other.config_)) {
        other.ctx_ = nullptr;
    }

    RedisConnection& operator=(RedisConnection&& other) noexcept {
        if (this != &other) {
            close();
            ctx_ = other.ctx_;
            config_ = std::move(other.config_);
            other.ctx_ = nullptr;
        }
        return *this;
    }

    bool is_valid() const { return ctx_ != nullptr && !ctx_->err; }
    redisContext* raw() const { return ctx_; }

    // 执行命令并返回回复(RAII自动释放)
    RedisReplyPtr execute(const char* format, ...);

    // 执行命令(参数列表版本)
    RedisReplyPtr execute(int argc, const char** argv, const size_t* argvlen);

private:
    bool reconnect_once();
    bool ensure_connected();
    void close();

    redisContext* ctx_ = nullptr;
    std::shared_ptr<const RedisConfig> config_;
};

// 连接池统计信息
struct PoolStats {
    std::atomic<size_t> total_connections{0};
    std::atomic<size_t> active_connections{0};
    std::atomic<size_t> wait_count{0};
    std::atomic<uint64_t> total_requests{0};
    std::atomic<uint64_t> failed_requests{0};
};

struct PoolStatsSnapshot {
    size_t total_connections = 0;
    size_t active_connections = 0;
    size_t wait_count = 0;
    uint64_t total_requests = 0;
    uint64_t failed_requests = 0;
};

// Redis连接池
class RedisPool {
public:
    explicit RedisPool(const RedisConfig& config);
    ~RedisPool();

    // 禁用拷贝和移动
    RedisPool(const RedisPool&) = delete;
    RedisPool& operator=(const RedisPool&) = delete;

    // 获取连接(阻塞直到可用或超时)
    RedisConnection acquire(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

    // 归还连接到池
    void release(RedisConnection conn);

    // 获取统计信息
    PoolStatsSnapshot get_stats() const;

    // 健康检查
    bool health_check();

    // 动态调整池大小
    void resize(size_t new_size);

private:
    RedisConnection create_connection();
    static RedisConnection create_connection(const RedisConfig& config);
    void decrement_total_connections();
    void maintain_pool();

    RedisConfig config_;
    std::queue<RedisConnection> pool_;
    mutable std::mutex pool_mutex_;
    std::condition_variable cv_;
    std::atomic<bool> shutdown_{false};
    PoolStats stats_;
    std::thread maintainer_thread_;
};

// 连接Guard(自动归还)
class RedisConnectionGuard {
public:
    RedisConnectionGuard(RedisPool& pool, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000))
        : pool_(pool), conn_(pool.acquire(timeout)), acquired_(conn_.is_valid()) {}

    ~RedisConnectionGuard() {
        if (acquired_) {
            pool_.release(std::move(conn_));
        }
    }

    RedisConnection* operator->() { return &conn_; }
    RedisConnection& operator*() { return conn_; }
    bool is_valid() const { return conn_.is_valid(); }

private:
    RedisPool& pool_;
    RedisConnection conn_;
    bool acquired_;
};

} // namespace rrl
