#include "Zone.h"
#include "SSLSession.h"
#include "Logger.h"

Zone::Zone(boost::asio::io_context& io, int zone_id, size_t max_sessions)
        : strand_(boost::asio::make_strand(io)), zone_id_(zone_id), max_sessions_(max_sessions)
    {  
        // zone_id 사용해서 zone 관리  
    }

void Zone::broadcast(const std::string& msg, boost::asio::ip::udp::socket& udp_socket, const std::string& sender_nickname) {
    auto self = shared_from_this();
    boost::asio::dispatch(strand_, [this, self, msg, &udp_socket, sender_nickname]() {
        // === UDP 큐 길이 제한 ===
        while (udp_send_queue_.size() >= kMaxUdpQueueSize) {
            g_logger->warn("[UDP][Zone] Send queue overflow (zone_id={}, size={}), dropping oldest", zone_id_, udp_send_queue_.size());
            udp_send_queue_.pop();
        }

        //if (udp_send_queue_.size() > kMaxUdpQueueSize) {
        // 정책1: 새 메시지 drop
        //g_logger->warn("[UDP][Zone] Send queue overflow (zone_id={}, size={}) - dropping message", zone_id_, udp_send_queue_.size());
        //return;

        // 세션 락!
        std::lock_guard<std::mutex> lock(session_mutex_);

        // map 순회 & weak_ptr->shared_ptr 안전 획득
        for (auto it = sessions_.begin(); it != sessions_.end(); ) {
            std::shared_ptr<SSLSession> sess = it->second.lock();
            if (!sess) {
                // 세션 소멸: map에서 clean up
                it = sessions_.erase(it);
                continue;
            }
            if (sess->get_nickname() == sender_nickname) {
                ++it;
                continue;
            }
            auto udp_ep = sess->get_udp_endpoint();
            if (udp_ep) {
                auto data = std::make_shared<std::string>(msg);
                udp_send_queue_.emplace(data, *udp_ep);
            }
            ++it;
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

bool Zone::add_session(const std::shared_ptr<SSLSession>& sess) {
    std::lock_guard<std::mutex> lock(session_mutex_);
    int id = sess->get_session_id();
    if (sessions_.size() >= max_sessions_) return false;
    if (sessions_.count(id) > 0) {
        g_logger->warn("[ZONE] 이미 등록된 세션 add_session 시도 (중복 무시)");
        return false;
    }
    sessions_[id] = sess;  // weak_ptr로 등록
    return true;
}

void Zone::remove_session(const std::shared_ptr<SSLSession>& sess) {
    std::lock_guard<std::mutex> lock(session_mutex_);
    int id = sess->get_session_id();
    if (sessions_.erase(id) > 0) {
        g_logger->info("[ZONE] 세션 정상적으로 제거");
    }
    else {
        g_logger->warn("[ZONE] 없는 세션 remove_session 시도 (무시 또는 디버깅)");
    }
}

