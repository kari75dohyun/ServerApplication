#include "Zone.h"
#include "Session.h"
#include "Logger.h"
#include "Utility.h"
#include "AppContext.h"

Zone::Zone(boost::asio::io_context& io, int zone_id, size_t max_sessions)
        : strand_(boost::asio::make_strand(io)), zone_id_(zone_id), max_sessions_(max_sessions)
    {  
        // zone_id 사용해서 zone 관리  
    }

void Zone::broadcast(const std::string& msg, boost::asio::ip::udp::socket& udp_socket, const std::string& sender_nickname) {
    auto self = shared_from_this();

    boost::asio::dispatch(strand_, [this, self, msg, &udp_socket, sender_nickname]() {
        std::vector<boost::asio::ip::udp::endpoint> targets;

        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            for (auto& [id, weak_sess] : sessions_) {
                auto sess = weak_sess.lock();
                if (!sess) continue;
                if (sess->get_nickname() == sender_nickname) continue;
                auto ep = sess->get_udp_endpoint();
                if (ep) targets.push_back(*ep);
            }
        }

        for (auto& ep : targets) {
            auto data = std::make_shared<std::string>(msg);
            udp_socket.async_send_to(
                boost::asio::buffer(*data), ep,
                [ep, data](const boost::system::error_code& ec, std::size_t bytes) {
                    if (ec) {
                        AppContext::instance().logger->warn("[UDP][Zone] Send error to {}:{} - {}", ep.address().to_string(), ep.port(), ec.message());
                    }
                });
        }

        AppContext::instance().logger->info("[Zone:{}] Broadcasted to {} sessions", zone_id_, targets.size());
        });
}

//void Zone::broadcast(const std::string& msg, boost::asio::ip::udp::socket& udp_socket, const std::string& sender_nickname) {
//    auto self = shared_from_this();
//
//    boost::asio::dispatch(strand_, [this, self, msg, &udp_socket, sender_nickname]() {
//        std::vector<boost::asio::ip::udp::endpoint> targets;
//
//        {
//            std::lock_guard<std::mutex> lock(session_mutex_);
//            for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
//                std::shared_ptr<Session> sess = it->second.lock();
//                if (!sess) continue;
//                if (sess->get_nickname() == sender_nickname) continue;
//                auto udp_ep = sess->get_udp_endpoint();
//                if (udp_ep) {
//                    targets.push_back(*udp_ep);
//                }
//            }
//        }
//
//        size_t target_count = targets.size();
//
//        // [A] 송신 큐에 대상 모두 enqueue
//        for (auto& ep : targets) {
//            auto data = std::make_shared<std::string>(msg);
//            // 큐 사이즈 제한(오버시 drop)
//            while (udp_send_queue_.size() >= static_cast<size_t>(AppContext::instance().config.value("max_udp_queue_size", 10000))) {
//                AppContext::instance().logger->warn("[UDP][Zone] udp_send_queue_ overflow (size={}), dropping oldest", udp_send_queue_.size());
//                udp_send_queue_.pop();
//            }
//            udp_send_queue_.emplace(data, ep);
//        }
//
//        AppContext::instance().logger->info("[UDP][Zone:{}] broadcast message sent from '{}' to {} clients", zone_id_, sender_nickname, target_count);
//
//        // [B] 전송 시도 (병렬 제한/순차 송신)
//        try_send_next(udp_socket);
//        });
//}

//void Zone::try_send_next(boost::asio::ip::udp::socket& udp_socket) {
//    auto self = shared_from_this();
//
//    boost::asio::dispatch(strand_, [this, self, &udp_socket]() {
//        // 병렬 송신 개수 제한
//        while (current_parallel_send_ < max_parallel_send_ && !udp_send_queue_.empty()) {
//            auto [data, ep] = udp_send_queue_.front();
//            udp_send_queue_.pop();
//            ++current_parallel_send_;
//
//            udp_socket.async_send_to(
//                boost::asio::buffer(*data), ep,
//                [this, self, &udp_socket, data, ep](const boost::system::error_code& ec, std::size_t /*bytes*/) {
//                    if (ec) {
//                        AppContext::instance().logger->warn("[UDP][zone queue send error] {}:{} {}", ep.address().to_string(), ep.port(), ec.message());
//                    }
//                    // 송신 완료 시 병렬 카운트 감소, 다음 송신 시도
//                    --current_parallel_send_;
//                    try_send_next(udp_socket);
//                }
//            );
//        }
//        });
//}

//void Zone::broadcast(const std::string& msg, boost::asio::ip::udp::socket& udp_socket, const std::string& sender_nickname) {
//    auto self = shared_from_this();
//
//    boost::asio::dispatch(strand_, [this, self, msg, &udp_socket, sender_nickname]() {
//        // 1. 복사할 벡터 선언
//        std::vector<std::tuple<std::shared_ptr<SSLSession>, boost::asio::ip::udp::endpoint>> targets;
//
//        {
//            std::lock_guard<std::mutex> lock(session_mutex_);
//            for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
//                std::shared_ptr<SSLSession> sess = it->second.lock();
//                if (!sess) continue;
//                if (sess->get_nickname() == sender_nickname) continue;
//                auto udp_ep = sess->get_udp_endpoint();
//                if (udp_ep) {
//                    targets.emplace_back(sess, *udp_ep); // weak_ptr->shared_ptr
//                }
//            }
//            // 여기서 락 즉시 해제
//        }
//
//        // 2. 이제 락 없이 send queue에 메시지 push/send
//        for (auto& [sess, ep] : targets) {
//            auto data = std::make_shared<std::string>(msg);
//            udp_socket.async_send_to(boost::asio::buffer(*data), ep,
//                [data](const boost::system::error_code&, std::size_t) {/*...*/});
//        }
//        });
//}

bool Zone::add_session(const std::shared_ptr<Session>& sess) {
    std::lock_guard<std::mutex> lock(session_mutex_);
    int id = sess->get_session_id();
    if (sessions_.size() >= max_sessions_) return false;
    if (sessions_.count(id) > 0) {
        AppContext::instance().logger->warn("[ZONE] 이미 등록된 세션 add_session 시도 (중복 무시)");
        return false;
    }
    sessions_[id] = sess;  // weak_ptr로 등록
    return true;
}

void Zone::remove_session(const std::shared_ptr<Session>& sess) {
    std::lock_guard<std::mutex> lock(session_mutex_);
    int id = sess->get_session_id();
    if (sessions_.erase(id) > 0) {
        AppContext::instance().logger->info("[ZONE] 세션 정상적으로 제거");
    }
    else {
        AppContext::instance().logger->warn("[ZONE] 없는 세션 remove_session 시도 (무시 또는 디버깅)");
    }
}

// zone 내의 모든 세션에 대해 주어진 함수를 실행
void Zone::for_each_session(const std::function<void(const std::shared_ptr<Session>&)>& fn) {
    std::lock_guard<std::mutex> lock(session_mutex_);
    for (auto it = sessions_.begin(); it != sessions_.end(); ) {
        if (auto session = it->second.lock()) {
            fn(session);
            ++it;
        }
        else {
            // 만료된 세션은 정리
            it = sessions_.erase(it);
        }
    }
}


