#pragma once
#include "SSLSession.h"
#include <string>
#include <random>
#include <sstream>
#include <boost/asio.hpp>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include "Logger.h"
#include <array>
#include <atomic>
#include <chrono>
#include <thread>


class SSLSession; // 전방 선언

constexpr int UDP_SHARD_COUNT = 16;

struct UdpRateLimiterShard {
    std::array<std::atomic<size_t>, UDP_SHARD_COUNT> counts;
    std::array<std::atomic<int64_t>, UDP_SHARD_COUNT> window_secs;
    UdpRateLimiterShard() {
        for (int i = 0; i < UDP_SHARD_COUNT; ++i) {
            counts[i].store(0);
            window_secs[i].store(0);
        }
    }
};

// 어디서든 include해서 쓸 수 있도록!
inline std::string generate_random_token(size_t length = 32) {
    static const char hex_chars[] = "0123456789abcdef";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);
    std::ostringstream oss;
    for (size_t i = 0; i < length; ++i) {
        oss << hex_chars[dis(gen)];
    }
    return oss.str();
}

void send_admin_alert(const std::string& message);

// JSON 파싱 헬퍼
inline std::optional<nlohmann::json> try_parse_json(const std::string& msg) {
    try {
        return nlohmann::json::parse(msg);
    }
    catch (const std::exception& e) {
        g_logger->info("[UDP] JSON Parse failed: {} / original: {}", e.what(), msg);
        return std::nullopt;
    }
}

// UDP 레이트 리밋 체크 헬퍼
//inline bool check_total_udp_rate_limit(
//    std::atomic<size_t>& total_count,
//    std::chrono::steady_clock::time_point& window,
//    size_t limit_per_sec
//) {
//    auto now = std::chrono::steady_clock::now();
//    static std::mutex m;
//    std::lock_guard<std::mutex> lock(m);
//    if (now - window > std::chrono::seconds(1)) {
//        window = now;
//        total_count = 0;
//    }
//    ++total_count;
//    return total_count <= limit_per_sec;
//}

bool check_user_udp_rate_limit(SSLSession& sess, size_t user_limit); 

inline bool sharded_udp_rate_limit(UdpRateLimiterShard& limiter, size_t limit_per_sec) {
    using namespace std::chrono;
    int64_t now_sec = duration_cast<seconds>(steady_clock::now().time_since_epoch()).count();
    size_t idx = std::hash<std::thread::id>{}(std::this_thread::get_id()) % UDP_SHARD_COUNT;

    int64_t old_sec = limiter.window_secs[idx].load(std::memory_order_relaxed);
    if (now_sec != old_sec) {
        limiter.window_secs[idx].store(now_sec, std::memory_order_relaxed);
        limiter.counts[idx].store(0, std::memory_order_relaxed);
    }
    limiter.counts[idx].fetch_add(1, std::memory_order_relaxed);

    // 전체 합산
    size_t total = 0;
    for (int i = 0; i < UDP_SHARD_COUNT; ++i) {
        total += limiter.counts[i].load(std::memory_order_relaxed);
    }
    return total <= limit_per_sec;
}
