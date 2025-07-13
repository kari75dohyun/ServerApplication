#pragma once
#include <string>
#include <random>
#include <sstream>
#include <boost/asio.hpp>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include "Logger.h"
#include "SSLSession.h"

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
inline bool check_total_udp_rate_limit(
    std::atomic<size_t>& total_count,
    std::chrono::steady_clock::time_point& window,
    size_t limit_per_sec
) {
    auto now = std::chrono::steady_clock::now();
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    if (now - window > std::chrono::seconds(1)) {
        window = now;
        total_count = 0;
    }
    ++total_count;
    return total_count <= limit_per_sec;
}

inline bool check_user_udp_rate_limit(
    SSLSession& sess,
    size_t user_limit
) {
    auto now = std::chrono::steady_clock::now();
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    if (now - sess.get_udp_packet_window() > std::chrono::seconds(1)) {
        sess.set_udp_packet_window(now);
        sess.set_udp_packet_count(0);
    }
    sess.inc_udp_packet_count();
    return sess.get_udp_packet_count() <= user_limit;
}