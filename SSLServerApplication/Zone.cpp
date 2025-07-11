#include "Zone.h"
#include "SSLSession.h"
#include "Logger.h"

Zone::Zone(boost::asio::io_context& io, int zone_id, size_t max_sessions)
        : strand_(boost::asio::make_strand(io)), zone_id_(zone_id), max_sessions_(max_sessions)
    {  
        // zone_id 사용해서 zone 관리  
    }

void Zone::broadcast(const std::string& msg, boost::asio::ip::udp::socket& udp_socket, const std::string& sender_nickname) {
    // 모든 세션에 대해 큐에 쌓음 (strand 내부에서 처리)
    auto self = shared_from_this();
    boost::asio::dispatch(strand_, [this, self, msg, &udp_socket, sender_nickname]() {
        // === UDP 큐 길이 제한 ===
        if (udp_send_queue_.size() > kMaxUdpQueueSize) {
            // 정책1: 새 메시지 drop
            g_logger->warn("[UDP][Zone] Send queue overflow (zone_id={}, size={}) - dropping message", zone_id_, udp_send_queue_.size());
            return;
            // 또는
            // 정책2: 오래된 것부터 지우고 추가
            // while (udp_send_queue_.size() > kMaxUdpQueueSize) udp_send_queue_.pop();
        }

        // 세션 락!
        std::lock_guard<std::mutex> lock(session_mutex_);

        for (const auto& sess : sessions_) {
            if (!sess) continue;
            if (sess->get_nickname() == sender_nickname) continue;    // 자기 자신 제외
            auto udp_ep = sess->get_udp_endpoint();
            if (udp_ep) {
                auto data = std::make_shared<std::string>(msg);
                udp_send_queue_.emplace(data, *udp_ep);
            }
        }
        try_send_next(udp_socket);
        });
}

void Zone::try_send_next(boost::asio::ip::udp::socket& udp_socket) {
    auto self = shared_from_this();
    boost::asio::dispatch(strand_, [this, self, &udp_socket]() {
        while (current_parallel_send_ < max_parallel_send_ && !udp_send_queue_.empty()) {
            auto [data, ep] = udp_send_queue_.front();
            udp_send_queue_.pop();
            ++current_parallel_send_;

            udp_socket.async_send_to(boost::asio::buffer(*data), ep,
                [this, self, &udp_socket, data, ep](const boost::system::error_code& ec, std::size_t /*bytes*/) {
                    if (ec) {
                        g_logger->warn("[UDP][zone queue send error] {}:{} {}", ep.address().to_string(), ep.port(), ec.message());
                    }
                    // strand 덕분에 lock 없이 안전
                    --current_parallel_send_;
                    try_send_next(udp_socket);
                }
            );
        }
        });
}

bool Zone::add_session(std::shared_ptr<SSLSession> sess) {
    std::lock_guard<std::mutex> lock(session_mutex_);
    if (sessions_.size() >= max_sessions_) {
        return false; // 300명이 꽉 찼으면 실패!
    }
    sessions_.insert(sess);
    return true;
}

void Zone::remove_session(std::shared_ptr<SSLSession> sess) {
    std::lock_guard<std::mutex> lock(session_mutex_);
    sessions_.erase(sess);
}