#pragma once
#include <string>
#include <random>
#include <sstream>
#include <boost/asio.hpp>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <array>
#include <atomic>
#include <chrono>
#include <thread>

class Session;

struct UdpRateLimiterShard {
    std::vector<std::atomic<size_t>>  counts;
    std::vector<std::atomic<int64_t>> window_secs;

    explicit UdpRateLimiterShard(size_t shard_count)
        : counts(shard_count), window_secs(shard_count) {
        for (size_t i = 0; i < shard_count; ++i) {
            counts[i].store(0, std::memory_order_relaxed);
            window_secs[i].store(0, std::memory_order_relaxed);
        }
    }
};

inline std::string generate_random_token(size_t length = 32) {
    static const char hex_chars[] = "0123456789abcdef";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);
    std::ostringstream oss;
    for (size_t i = 0; i < length; ++i) oss << hex_chars[dis(gen)];
    return oss.str();
}

void send_admin_alert(const std::string& message);

std::optional<nlohmann::json> try_parse_json(const std::string& msg);

// 샤드 수는 동적으로 읽기
inline bool sharded_udp_rate_limit(UdpRateLimiterShard& limiter, size_t limit_per_sec) {
    using namespace std::chrono;
    const auto shard_count = limiter.counts.size();
    if (shard_count == 0) return true;

    int64_t now_sec = duration_cast<seconds>(steady_clock::now().time_since_epoch()).count();
    size_t idx = std::hash<std::thread::id>{}(std::this_thread::get_id()) % shard_count;

    int64_t old_sec = limiter.window_secs[idx].load(std::memory_order_relaxed);
    if (now_sec != old_sec) {
        limiter.window_secs[idx].store(now_sec, std::memory_order_relaxed);
        limiter.counts[idx].store(0, std::memory_order_relaxed);
    }
    limiter.counts[idx].fetch_add(1, std::memory_order_relaxed);

    size_t total = 0;
    for (size_t i = 0; i < shard_count; ++i) {
        total += limiter.counts[i].load(std::memory_order_relaxed);
    }
    return total <= limit_per_sec;
}

std::string get_env_secret(const std::string& env_name);
void load_config(const std::string& path = "config.json");
