#include "DataHandler.h" 
#include "Session.h"
#include <boost/asio.hpp>
#include <iostream>
#include <nlohmann/json.hpp>
#include "Logger.h"
#include "Utility.h"
#include "ZoneManager.h"
#include "SessionManager.h"
#include "MemoryTracker.h"
#include <string>
#include <chrono>
#include "AppContext.h"

using namespace std;
using namespace boost::asio;
using boost::asio::ip::udp;

DataHandler::DataHandler(boost::asio::io_context& io, std::shared_ptr<SessionManager> session_manager, int zone_count, size_t udp_shard_count)
    : shard_count(max(4u, thread::hardware_concurrency() * 2)),
    dispatcher_(this, session_manager.get()),
    // 글로벌 keepalive 관련 타이머 초기화
    keepalive_timer_(io),
    zone_manager_(io, zone_count),
    monitor_timer_(io),
    io_context_(io),
    session_manager_(session_manager),
    udp_global_limiter_(udp_shard_count),
    cleanup_timer_(io) {
	//udp_shard_count = max(4u, thread::hardware_concurrency() * 2); 
	// UDP 송신 큐를 직렬화하기 위한 strand 초기화
    udp_send_strand_ = std::make_unique<boost::asio::strand<boost::asio::any_io_executor>>(io.get_executor());
    start_keepalive_loop();  // 생성자에서 타이머 시작
    start_monitor_loop();    // 모니터 루프 시작
	start_cleanup_loop();    // session 클린업 타이머 시작
}

void DataHandler::dispatch(const std::shared_ptr<Session>& session, const json& msg) {
    dispatcher_.dispatch(session, msg);
}

void DataHandler::add_session(int session_id, std::shared_ptr<Session> session) {
    AppContext::instance().logger->info("[DEBUG][TCP] DataHandler address: {}", (void*)this);
    session_manager_->add_session(session);
}

void DataHandler::remove_session(int session_id) {
    // 1. SessionManager에서 제거하면서 세션 반환받기
    auto session = session_manager_->remove_session(session_id);
    // 2. ZoneManager에 세션 제거 요청
    if (session->get_zone_id() > -1) {
        zone_manager_.remove_session(session);
    }
    // 3. 반환받은 세션을 세션 풀로 반환 (락 범위 밖에서 안전하게)
    if (session) {
        auto pool = get_session_pool();
        AppContext::instance().logger->info("[DEBUG] get_session_pool() 호출됨, 주소: {}", (void*)pool.get());
        if (pool) {
            pool->release(session);  // is_closed() 여부와 상관없이 반드시 release
        }
    }
}

void DataHandler::broadcast(const std::string& msg, int sender_session_id, std::shared_ptr<Session> /*session*/) {
    auto shared_msg = std::make_shared<std::string>(msg);
    session_manager_->for_each_session([&](const std::shared_ptr<Session>& sess) {
        if (!sess) return;
        if (sess->get_session_id() == sender_session_id) return; // 자기 자신 제외
        if (sess->get_nickname().empty()) return;
        try {
            AppContext::instance().logger->info("[broadcast] target session_id= {}", sess->get_session_id(), " (sender_session_id= {}", sender_session_id);
            sess->post_write(shared_msg);
        }
        catch (const std::exception& e) {
            AppContext::instance().logger->info("[broadcast] Exception scheduling broadcast: {}", e.what());
        }
        });
}

// 긴급 서버공지, 전원강제알림 등 필요할 때
void DataHandler::broadcast_strict(const string& msg) {
    auto shared_msg = make_shared<string>(msg);
    for_each_session([shared_msg](const shared_ptr<Session>& sess) {
        if (sess) sess->post_write(shared_msg);
        });
}

std::shared_ptr<Session> DataHandler::find_session_by_nickname(const std::string& nickname) {
    return session_manager_->find_session_by_nickname(nickname);
}

// 전체 세션 순회
void DataHandler::for_each_session(const std::function<void(const std::shared_ptr<Session>&)> fn) {
    session_manager_->for_each_session(fn);
}

// 글로벌 keepalive 관련 함수
void DataHandler::start_keepalive_loop() {
    keepalive_timer_.expires_after(std::chrono::seconds(keepalive_timeout_));
    keepalive_timer_.async_wait([this](const boost::system::error_code& ec) {
        if (!ec) {
            do_keepalive_check();
            // UDP 엔드포인트 만료도 같이 처리.
            expire_stale_udp_endpoints(std::chrono::seconds(static_cast<size_t>(AppContext::instance().config.value("udp_expire_timeout_seconds", 300)))); // 5분 기준, 원하는 값으로
            start_keepalive_loop(); // 반복
        }
        });
}

// 글로벌 keepalive 체크
void DataHandler::do_keepalive_check() {
    auto now = std::chrono::steady_clock::now();

    // 1. close해야 할 세션을 임시로 모아둘 벡터
    vector<shared_ptr<Session>> sessions_to_close;

    for_each_session([this, now, &sessions_to_close](shared_ptr<Session> sess) {
        if (!sess) return;

        if (sess->is_nickname_registered() == false) return; // 닉네임 등록 안된 세션은 skip

        // 클라가 하트비트 보내는 걸로 변경 되어서 삭제
        //// 1. ping 보내기 (ping_interval_ 간격마다)
        //if ((now - sess->get_last_alive_time()) > ping_interval_) {
        //    cout << "[PING] session_id=" << sess->get_session_id() << endl;
        //    sess->post_write(R"({"type":"ping"})" "\n");
        //}

        // 2. keepalive 타임아웃 체크
        if ((now - sess->get_last_alive_time()) > keepalive_timeout_) {
            // 여기서 직접 close_session() 하지 말고, 임시 벡터에 push!
            sessions_to_close.push_back(sess);
        }
        });

    // 2. 락 해제 후(즉, 세션 안전하게 순회 후) 실제 close_session 호출
    for (auto& sess : sessions_to_close) {
        //cout << "[KEEPALIVE TIMEOUT] session_id=" << sess->get_session_id() << " - close session" << endl;
        AppContext::instance().logger->info("[KEEPALIVE TIMEOUT] session_id= {}", sess->get_session_id(), "- close session");
        sess->close_session();
    }
}

void DataHandler::expire_stale_udp_endpoints(std::chrono::seconds timeout) {
    auto now = std::chrono::steady_clock::now();
    for_each_session([now, timeout](shared_ptr<Session> sess) {
        if (!sess) return;
        if (sess->get_udp_endpoint()) {
            auto last = sess->get_last_udp_alive_time();
            if ((now - last) > timeout) {
                sess->clear_udp_endpoint(); // 만료 처리
                // 로그 남기기
                AppContext::instance().logger->info("[UDP][만료] session_id={} 엔드포인트 만료됨 ({}초 이상 응답없음)",
                    sess->get_session_id(), std::chrono::duration_cast<std::chrono::seconds>(now - last).count());
            }
        }
    });
}

// UDP 메시지 수신 처리
void DataHandler::on_udp_receive(const std::string& msg, const udp::endpoint& from, udp::socket& udp_socket) {
    AppContext::instance().logger->info("[DEBUG][UDP] DataHandler address: {}", (void*)this);
    AppContext::instance().logger->info("[DEBUG][UDP] SessionManager address: {}", (void*)session_manager_.get());
    // [1] 서버 전체 UDP 패킷 제한 (중복방지)
    //if (!check_total_udp_rate_limit(udp_total_packet_count_, udp_total_packet_window_, TOTAL_LIMIT_PER_SEC)) {
    if (!sharded_udp_rate_limit(udp_global_limiter_, static_cast<size_t>(static_cast<size_t>(AppContext::instance().config.value("total_limit_per_sec", 1000))))) {
        AppContext::instance().logger->warn("[UDP][FLOOD] 서버 전체 초과 ({}/1초)", udp_total_packet_count_.load());
        return;
    }

    // [2] JSON 파싱
    auto jmsg_opt = try_parse_json(msg);
    if (!jmsg_opt) {
        std::string response = R"({"type":"error","msg":"Invalid JSON"})";
        auto data = std::make_shared<std::string>(response);
        udp_socket.async_send_to(
            boost::asio::buffer(*data), from,
            [data](const boost::system::error_code& ec, size_t) {
                if (ec) AppContext::instance().logger->error("[UDP] Send error2: {}", ec.message());
            }
        );
        return;
    }
    const auto& res = *jmsg_opt;

    const std::string nickname = res.value("nickname", "");
    const std::string type = res.value("type", "");
    const std::string token = res.value("token", "");

    // [3] 닉네임 체크
    if (nickname.empty()) {
        AppContext::instance().logger->warn("[UDP] 임의 패킷 차단: 닉네임 없음 / EP({}:{})", from.address().to_string(), from.port());
        return;
    }

    // [4] 세션 조회
    auto session = session_manager_->find_session_by_nickname(nickname);
    if (!session) {
        AppContext::instance().logger->warn("[UDP] 임의 패킷 차단: 닉네임({})/EP({}:{})", nickname, from.address().to_string(), from.port());
        return;
    }

    // [5] 토큰 검사 (udp_register만 예외)
    if (type != "udp_register") {
        if (token.empty() || session->get_udp_token() != token) {
            AppContext::instance().logger->warn("[UDP] Invalid or missing udp_token! nickname={}, remote_ep={}", nickname, from.address().to_string());
            nlohmann::json resp = { {"type", "error"}, {"msg", token.empty() ? "UDP token is required" : "Invalid UDP token"} };
            auto data = std::make_shared<std::string>(resp.dump());
            udp_socket.async_send_to(boost::asio::buffer(*data), from, [data](const boost::system::error_code&, size_t) {});
            return;
        }
    }

    // [6] 유저별 UDP Flood 제한
    if (!session->checkUdpRateLimit()) {
    AppContext::instance().logger->warn("[UDP][FLOOD] 유저({}) 초과! IP({}:{})",
        session->get_nickname(), from.address().to_string(), from.port());

    nlohmann::json resp = {
        {"type", "error"},
        {"msg", "UDP rate limit exceeded"}
    };
    auto data = std::make_shared<std::string>(resp.dump());
    udp_socket.async_send_to(boost::asio::buffer(*data), from,
        [data](const boost::system::error_code& ec, size_t) {
            if (ec) {
                AppContext::instance().logger->warn("[UDP] UDP rate limit 응답 전송 실패: {}", ec.message());
            }
        });
        return;
    }
    //if (!check_user_udp_rate_limit(*session, static_cast<size_t>(AppContext::instance().config.value("user_limit_per_sec", 10)))) {
    //    AppContext::instance().logger->warn("[UDP][FLOOD] 유저({}) 초과 ({}/1초)", session->get_nickname(), session->get_udp_packet_count());
    //    nlohmann::json resp = { {"type", "error"}, {"msg", "UDP rate limit exceeded"} };
    //    auto data = std::make_shared<std::string>(resp.dump());
    //    udp_socket.async_send_to(boost::asio::buffer(*data), from,
    //        [data](const boost::system::error_code& ec, size_t) {
    //            AppContext::instance().logger->warn("[UDP][브로드캐스트] 전송 에러 1{}", ec.message());
    //        });
    //    return;
    //}

    // [7] 엔드포인트 변화 감지 및 갱신
    auto prev_ep = session->get_udp_endpoint();
    if (!prev_ep || *prev_ep != from) {
        AppContext::instance().logger->info("[UDP] endpoint 변경 감지! 이전: {}:{} → 신규: {}:{}",
            prev_ep ? prev_ep->address().to_string() : "N/A",
            prev_ep ? prev_ep->port() : 0,
            from.address().to_string(),
            from.port());
    }
    session->set_udp_endpoint(from);
    session->update_udp_alive_time();

    // [8] dispatcher에 전달 (모든 검증 완료 후!)
    dispatcher_.dispatch_udp(session, msg, from, udp_socket);
}

void DataHandler::udp_broadcast(const std::string& msg, boost::asio::ip::udp::socket& udp_socket, const std::string& sender_nickname) {
    boost::asio::post(*udp_send_strand_, [this, msg, &udp_socket, sender_nickname]() {
        // 큐 사이즈가 넘치면 오래된 것부터 제거
        while (udp_send_queue_.size() >= static_cast<size_t>(AppContext::instance().config.value("max_udp_queue_size", 10000))) {
            AppContext::instance().logger->warn("[UDP][Global] udp_send_queue_ overflow (size={}), dropping oldest", udp_send_queue_.size());
            udp_send_queue_.pop();
        }
        //if (udp_send_queue_.size() > kMaxUdpQueueSize) {
            //// 정책1: 새 메시지 drop 및 경고
            //g_logger->warn("[UDP][Global] udp_send_queue_ overflow (size={}) - dropping message", udp_send_queue_.size());
            //return;
        //}
        for_each_session([&](const std::shared_ptr<Session> sess) {
            if (!sess) return;
            if (sess->get_nickname().empty()) return;
            if (sess->get_nickname() == sender_nickname) return; // 자기 자신 제외

            auto udp_ep = sess->get_udp_endpoint();
            if (udp_ep) {
                nlohmann::json recv = nlohmann::json::parse(msg);
                nlohmann::json send;
                send["type"] = recv.value("type", "");
                send["nickname"] = recv.value("nickname", "");
                send["msg"] = recv.value("msg", "");
                auto data = std::make_shared<std::string>(send.dump());
                udp_send_queue_.emplace(data, *udp_ep);
            }
            });

        // 큐 송신 시도
        try_send_next_udp(udp_socket);
        });
}

void DataHandler::try_send_next_udp(boost::asio::ip::udp::socket& udp_socket) {
    if (udp_send_in_progress_ || udp_send_queue_.empty()) {
        return;
    }
    udp_send_in_progress_ = true;

    auto [data, ep] = udp_send_queue_.front();
    udp_send_queue_.pop();

    // === 안전성 체크 ===
    if (!data || data->empty()) {
        AppContext::instance().logger->error("[try_send_next_udp] data is nullptr or empty!");
        udp_send_in_progress_ = false;
        return;
    }
    if (ep.address().is_unspecified() || ep.port() == 0) {
        AppContext::instance().logger->error("[try_send_next_udp] endpoint is unspecified/zero! {}:{}", ep.address().to_string(), ep.port());
        udp_send_in_progress_ = false;
        return;
    }
    if (!udp_socket.is_open()) {
        AppContext::instance().logger->error("[try_send_next_udp] udp_socket is closed!");
        udp_send_in_progress_ = false;
        return;
    }

    udp_socket.async_send_to(
        boost::asio::buffer(*data), ep,
        boost::asio::bind_executor(*udp_send_strand_,
            [this, &udp_socket, data](const boost::system::error_code& ec, std::size_t /*bytes*/) {
                if (ec) {
                    AppContext::instance().logger->warn("[UDP][send queue][strand] {} {}", ec.message(), "async_send_to 에러");
                }
                udp_send_in_progress_ = false;
                try_send_next_udp(udp_socket);
            }
        )
    );
}

// 미인증 세션 정리 함수 
void DataHandler::cleanup_unauth_sessions(size_t max_unauth) {
    vector<shared_ptr<Session>> unauth_sessions;

    for_each_session([&](const shared_ptr<Session> sess) {
        if (!sess) return;
        SessionState state = sess->get_state();
        if (state == SessionState::Handshaking || state == SessionState::LoginWait) {
            unauth_sessions.push_back(sess);
        }
        });

    // 임계치 초과시, 오래된 세션부터 정리 (예: 타임스탬프 기준 정렬)
    if (unauth_sessions.size() > max_unauth) {
        // 오래된 순으로 정렬 (예: get_last_alive_time() 활용)
        sort(unauth_sessions.begin(), unauth_sessions.end(),
            [](const auto& a, const auto& b) {
                return a->get_last_alive_time() < b->get_last_alive_time();
            });

        size_t count_to_close = unauth_sessions.size() - max_unauth;
        for (size_t i = 0; i < count_to_close; ++i) {
            unauth_sessions[i]->close_session();
        }
    }
}

void DataHandler::udp_broadcast_zone(int zone_id, const std::string& msg, boost::asio::ip::udp::socket& udp_socket, const std::string& sender_nickname) {
    auto zone = zone_manager_.get_zone(zone_id);
    if (zone) {
        zone->broadcast(msg, udp_socket, sender_nickname);
    }
}

// 세션을 zone에 등록
void DataHandler::assign_session_to_zone(std::shared_ptr<Session> session, int zone_id) {
    auto zone = zone_manager_.enter_zone(zone_id);
    if (zone->add_session(session)) {
        session->set_zone_id(zone_id);
		session->set_zone(zone); // 세션에 존 정보 설정
        AppContext::instance().logger->info("[ZONE] 세션 {} → ZONE {} 배정", session->get_session_id(), zone_id);
    }
    else {
        AppContext::instance().logger->warn("[ZONE] 세션 {} → ZONE {} add_session 실패 (존 인원수/중복 체크)", session->get_session_id(), zone_id);
    }
}


std::shared_ptr<Zone> DataHandler::get_zone(int zone_id) {
    return zone_manager_.get_zone(zone_id);
}
// 활성 세션 모니터링 루프 시작
void DataHandler::start_monitor_loop()
{
    monitor_timer_.expires_after(std::chrono::seconds(10)); // 60초마다 출력
    monitor_timer_.async_wait([this](const boost::system::error_code& ec) {
        if (!ec) {
            // 1. 전체 카운트
            AppContext::instance().logger->info("[SERVER] Active sessions: {}", session_manager_->get_total_session_count());

            // 2. (추가) 세션 상세 정보 출력
            if (session_pool_) {
                size_t active_count = 0;
                session_pool_->for_each_active([&active_count](const std::shared_ptr<Session>& sess) {
                    ++active_count;
                    AppContext::instance().logger->info("[ACTIVE] session_id={}, nickname={}", sess->get_session_id(), sess->get_nickname());
                    });
                AppContext::instance().logger->info("[SERVER] 활성 세션 수: {}", active_count);
            }
            MemoryTracker::log_memory_usage();   //메모리 사용량도 같이 남김

            start_monitor_loop(); // 반복
        }
        });
}

void DataHandler::register_bad_udp_packet(const boost::asio::ip::udp::endpoint& endpoint) {
    std::string key = endpoint.address().to_string() + ":" + std::to_string(endpoint.port());
    auto now = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(bad_udp_mutex_);

    auto it = bad_udp_map_.find(key);
    if (it == bad_udp_map_.end()) {
        // 처음 실패 기록
        bad_udp_map_[key] = { 1, now };
        AppContext::instance().logger->info("[DataHandler] UDP bad packet registered (new): {}", key);
    }
    else {
        auto& info = it->second;

        // 시간 경과가 차단 유지 시간보다 크면 초기화
        if (now - info.first_fail_time > block_time_) {
            info.fail_count = 1;
            info.first_fail_time = now;
            AppContext::instance().logger->info("[DataHandler] UDP bad packet counter reset: {}", key);
        }
        else {
            ++info.fail_count;
            AppContext::instance().logger->info("[DataHandler] UDP bad packet count increased to {}: {}", info.fail_count, key);
        }
    }
}

bool DataHandler::is_blocked_udp(const boost::asio::ip::udp::endpoint& endpoint) {
    std::string key = endpoint.address().to_string() + ":" + std::to_string(endpoint.port());
    auto now = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(bad_udp_mutex_);
    auto it = bad_udp_map_.find(key);
    if (it == bad_udp_map_.end()) return false;

    const auto& info = it->second;

    // 차단 기간 지나면 해제
    if (now - info.first_fail_time > block_time_) {
        bad_udp_map_.erase(it);
        AppContext::instance().logger->info("[DataHandler] UDP bad packet block expired: {}", key);
        return false;
    }

    // 차단 조건: 실패 횟수가 제한 이상인 경우
    return info.fail_count >= bad_udp_limit_;
}

void DataHandler::start_cleanup_loop() {
    cleanup_timer_.expires_after(std::chrono::seconds(60)); // 1분마다
    cleanup_timer_.async_wait([this](const boost::system::error_code& ec) {
        if (!ec) {
            session_manager_->cleanup_inactive_sessions(std::chrono::seconds(300)); // 5분 이상 비활성 시 종료
            start_cleanup_loop(); // 재귀 호출
        }
        });
}

std::shared_ptr<Session> DataHandler::getSessionByEndpoint(const udp::endpoint& endpoint) {
    std::lock_guard<std::mutex> lock(udp_endpoint_mutex_);

    auto it = udp_endpoint_to_session_.find(endpoint);
    if (it != udp_endpoint_to_session_.end()) {
        return it->second.lock();
    }
    return nullptr;
}

