#include "DataHandler.h" 
#include "SSLSession.h"
#include <boost/asio.hpp>
#include <iostream>
#include <nlohmann/json.hpp>
#include "Logger.h"
#include "Utility.h"
#include "ZoneManager.h"

using json = nlohmann::json;
using namespace std;
using namespace boost::asio;
using boost::asio::ip::udp;

DataHandler::DataHandler(boost::asio::io_context& io, int zone_count)
    : shard_count(max(4u, thread::hardware_concurrency() * 2)),
    session_buckets(shard_count),
    session_mutexes(shard_count),
    dispatcher_(this),
    // 글로벌 keepalive 관련 타이머 초기화
    keepalive_timer_(io),
    zone_manager_(io, zone_count),
    io_context_(io) {
	// UDP 송신 큐를 직렬화하기 위한 strand 초기화
    udp_send_strand_ = std::make_unique<boost::asio::strand<boost::asio::any_io_executor>>(io.get_executor());
    //for (int zone_id = 1; zone_id <= zone_count; ++zone_id) {
    //    zones_[zone_id] = std::make_shared<Zone>(io, zone_id, MAX_ZONE_SESSION_COUNT);
    //}
    start_keepalive_loop();  // 생성자에서 타이머 시작
}

void DataHandler::add_session(int session_id, shared_ptr<SSLSession> session) {
    int shard = get_shard(session_id);
    lock_guard<mutex> lock(session_mutexes[shard]);
    auto it = session_buckets[shard].find(session_id);
    if (it != session_buckets[shard].end()) {
        //std::cerr << "[add_session] FATAL: session_id " << session_id << " already exists! Closing previous session." << std::endl;
        //LOG_INFO("[add_session] FATAL : session_id=", session_id, " already exists! Closing previous session. ");
        g_logger->warn("[add_session] FATAL : session_id= {}", session_id, " already exists! Closing previous session. ");
        if (it->second) it->second->close_session();
        session_buckets[shard].erase(it);
    }
    // 중복 검사 후 세션 추가
    session_buckets[shard][session_id] = session;
    //LOG_INFO("[add_session] session_id=", session_id, " added to shard ", shard);

}

void DataHandler::remove_session(int session_id) {
    int shard = get_shard(session_id);
    shared_ptr<SSLSession> session;
    {
        lock_guard<mutex> lock(session_mutexes[shard]);
        auto it = session_buckets[shard].find(session_id);
        if (it != session_buckets[shard].end()) {
            session = it->second; // shared_ptr를 먼저 확보
            session_buckets[shard].erase(it);
            //cout << "[remove_session] session_id=" << session_id << " removed from shard " << shard << endl;
            g_logger->info("[remove_session] session_id= {}", session_id, " removed from shard ", shard);
        }
        else {
            //cout << "[remove_session] session_id=" << session_id << " not found (already removed?)" << endl;
            g_logger->info("[remove_session] session_id= {}", session_id, " not found (already removed?)");
        }
    }
    // 락을 풀고 나서 release를 호출 (컨테이너/락과 완전히 분리)
    if (session && !session->is_closed()) {  // 중복 반환 방지!
        int zone_id = session->get_zone_id();
        if (zone_id > 0) {
            auto zone = zone_manager_.get_zone(zone_id);
            if (zone) {
                zone->remove_session(session);
                g_logger->info("[ZONE] 세션 {} → ZONE {}에서 제거", session_id, zone_id);
            }
            //auto it = zones_.find(zone_id);
            //if (it != zones_.end()) {
            //    it->second->remove_session(session);
            //    g_logger->info("[ZONE] 세션 {} → ZONE {}에서 제거", session_id, zone_id);
            //}
        }

        auto pool = get_session_pool();
        if (pool) {
            pool->release(session);
        }
    }
}

shared_ptr<SSLSession> DataHandler::get_session(int session_id) {
    int shard = get_shard(session_id);
    lock_guard<mutex> lock(session_mutexes[shard]);
    auto it = session_buckets[shard].find(session_id);
    if (it != session_buckets[shard].end())
        return it->second;
    return nullptr;
}

void DataHandler::do_handshake(shared_ptr<SSLSession> session) {
    session->post_task([this, session]() {
        auto self = session;
        auto& strand = session->get_strand();
        session->get_socket().async_handshake(boost::asio::ssl::stream_base::server,
            boost::asio::bind_executor(strand, [this, self](const boost::system::error_code& ec) {
                if (!ec) {
                    self->set_state(SessionState::LoginWait);
                    self->set_message(R"({"type":"notice","msg":"Enter your nickname:"})" "\n");
                    //do_write(self);
                    self->post_write(self->get_message());

                    do_read(self); // 최초 read 예약! (핸드쉐이크 성공 직후)
                }
                else {
                    cerr << "Handshake failed: " << ec.message() << endl;
                }
                self->run_next_task();
                })
        );
        });
}

void DataHandler::do_read(shared_ptr<SSLSession> session) {
    // [1] 중복 read 방지!
    if (session->get_state() == SessionState::Closed) {
        g_logger->warn("Closed session: 콜백/메시지 무시 [session_id={}]", session->get_session_id());
        return;
    }
    if (!session->try_acquire_read()) {
        //cerr << "[WARN] 중복 do_read 감지! session_id=" << session->get_session_id() << endl;
        g_logger->info("[WARN] 중복 do_read 감지! session_id= {}", session->get_session_id());
        return;
    }
    // do_read를 직접 호출하지 않고, post_task로 감싼다.
    session->post_task([this, session]() {
        auto self = session;
        auto& strand = session->get_strand();
        session->get_socket().async_read_some(
            buffer(session->get_data(), sizeof(session->get_data())),
            boost::asio::bind_executor(strand, [this, self](const boost::system::error_code& ec, size_t length) {
                // [2] 콜백 진입 시 반드시 해제!
                self->release_read();

                try {
                    if (!ec) {
                        // 1. 누적 버퍼에 append
                        self->get_msg_buffer().append(self->get_data(), length);

                        // 2. 여러 메시지 추출 및 처리
                        while (auto opt_msg = self->get_msg_buffer().extract_message()) {
                            try {
                                //cout << "[DEBUG] dispatching message" << endl;
                                json msg = json::parse(*opt_msg);
                                dispatcher_.dispatch(self, msg);
                            }
                            catch (const exception& e) {
                                cerr << "[JSON parsing error] " << e.what() << " / data: " << *opt_msg << endl;
                                self->set_message(R"({"type":"error","msg":"Message parsing failed"})" "\n");
                                //do_write(self);
                                self->post_write(self->get_message());
                                // 에러 시에도 계속 다음 메시지 분리/처리
                            }
                        }

                        // 3. 계속해서 read (이 구조면 wrote 체크 필요 없음)
                        do_read(self);
                    }
                    else if (ec == boost::asio::error::eof) {
                        cout << "Client disconnected." << endl;
                        // 퇴장 알림
                        string nickname = self->get_nickname();
                        json notice;
                        notice["type"] = "notice";
                        notice["msg"] = nickname + " has left.";
                        broadcast(notice.dump() + "\n", self->get_session_id(), self);

                        self->close_session();  // 세션 종료
                    }
                    else if (ec == boost::asio::error::connection_reset) {
                        cout << "Client forcibly disconnected." << endl;
                        string nickname = self->get_nickname();
                        if (nickname.empty()) {
                            cerr << "Client disconnected without a nickname." << endl;
                        }
                        else {
                            json notice;
                            notice["type"] = "notice";
                            notice["msg"] = nickname + " has left.";
                            broadcast(notice.dump() + "\n", self->get_session_id(), self);
                            std::cout << "Client with nickname '" << nickname << "' disconnected." << std::endl;
                        }

                        self->close_session();
                    }
                    else if (ec == boost::asio::error::operation_aborted) {
                        // [995] 소켓 종료, 타이머 취소 등에서 발생하는 "정상 종료 케이스"
                        // cout << "[INFO] Read cancelled by server shutdown or session close." << endl;
                        // 로그를 아예 안 찍거나, INFO/DEBUG로만 출력
                    }
                    else {
                        cerr << "Read failed: [" << ec.value() << "] " << ec.message() << endl;
                        self->close_session();
                    }
                }
                catch (const exception& e) {
                    cerr << "[FATAL][do_read handler 예외] " << e.what() << endl;
                    self->close_session();
                }
                self->run_next_task(); // 항상 마지막에!

                })
        );
        });
}

void DataHandler::broadcast(const string& msg, int sender_session_id, shared_ptr<SSLSession> /*session*/) {
    auto shared_msg = make_shared<string>(msg);
    vector<shared_ptr<SSLSession>> targets;

    // 1. 각 샤드별로 lock (세션 포인터만 복사)
    for (unsigned int shard = 0; shard < shard_count; ++shard) {
        lock_guard<mutex> lock(session_mutexes[shard]);
        for (const auto& [id, sess] : session_buckets[shard]) {
            if (sess && id != sender_session_id) {
                if (sess->get_nickname().empty()) continue;
                targets.push_back(sess);
            }
        }
    }

    // 2. 락 해제 후 각 세션에 post_write로 write 작업 직렬화
    for (auto& sess : targets) {
        try {
            //cout << "[broadcast] target session_id=" << sess->get_session_id()
            //    << " (sender_session_id=" << sender_session_id << ")" << endl;

            g_logger->info("[broadcast] target session_id= {}", sess->get_session_id(), " (sender_session_id= {}", sender_session_id);

            sess->post_write(shared_msg);  // <--- 모든 세션이 같은 메시지 객체를 참조!
        }
        catch (const exception& e) {
            //cerr << "[broadcast] Exception scheduling broadcast: " << e.what() << endl;
            g_logger->info("[broadcast] Exception scheduling broadcast: {}", e.what());
        }
    }
}

// 전체 세션 수 정확히 세기
size_t DataHandler::get_total_session_count() {
    size_t total = 0;
    for (unsigned int i = 0; i < shard_count; ++i)
        session_mutexes[i].lock();
    for (unsigned int i = 0; i < shard_count; ++i)
        total += session_buckets[i].size();
    for (unsigned int i = 0; i < shard_count; ++i)
        session_mutexes[i].unlock();
    return total;
}

// 긴급 서버공지, 전원강제알림 등 필요할 때
void DataHandler::broadcast_strict(const string& msg) {
    auto shared_msg = make_shared<string>(msg);
    for_each_session([shared_msg](const shared_ptr<SSLSession>& sess) {
        if (sess) sess->post_write(shared_msg);
        });
}

void DataHandler::register_nickname(const string& nickname, shared_ptr<SSLSession> session) {
    lock_guard<mutex> lock(nickname_mutex_);
    nickname_to_session_[nickname] = session;
}
void DataHandler::unregister_nickname(const string& nickname, shared_ptr<SSLSession> session) {
    lock_guard<mutex> lock(nickname_mutex_);
    auto it = nickname_to_session_.find(nickname);
    if (it != nickname_to_session_.end()) {
        // 세션 포인터 일치 시에만 제거
        if (!it->second.expired() && it->second.lock() == session)
            nickname_to_session_.erase(it);
    }
}
shared_ptr<SSLSession> DataHandler::find_session_by_nickname(const string& nickname) {
    lock_guard<mutex> lock(nickname_mutex_);
    auto it = nickname_to_session_.find(nickname);
    if (it != nickname_to_session_.end()) {
        return it->second.lock();
    }
    return nullptr;
}

// 글로벌 keepalive 관련 함수
void DataHandler::start_keepalive_loop() {
    keepalive_timer_.expires_after(std::chrono::seconds(keepalive_timeout_));
    keepalive_timer_.async_wait([this](const boost::system::error_code& ec) {
        if (!ec) {
            do_keepalive_check();
            // UDP 엔드포인트 만료도 같이 처리.
            expire_stale_udp_endpoints(std::chrono::seconds(UDP_EXPIRE_TIMEOUT_SECONDS)); // 5분 기준, 원하는 값으로
            start_keepalive_loop(); // 반복
        }
        });
}

// 글로벌 keepalive 체크
void DataHandler::do_keepalive_check() {
    auto now = std::chrono::steady_clock::now();

    // 1. close해야 할 세션을 임시로 모아둘 벡터
    vector<shared_ptr<SSLSession>> sessions_to_close;

    for_each_session([this, now, &sessions_to_close](shared_ptr<SSLSession> sess) {
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
        g_logger->info("[KEEPALIVE TIMEOUT] session_id= {}", sess->get_session_id(), "- close session");
        sess->close_session();
    }
}

void DataHandler::expire_stale_udp_endpoints(std::chrono::seconds timeout) {
    auto now = std::chrono::steady_clock::now();
    for_each_session([now, timeout](shared_ptr<SSLSession> sess) {
        if (!sess) return;
        if (sess->get_udp_endpoint()) {
            auto last = sess->get_last_udp_alive_time();
            if ((now - last) > timeout) {
                sess->clear_udp_endpoint(); // 만료 처리
                // 로그 남기기
                g_logger->info("[UDP][만료] session_id={} 엔드포인트 만료됨 ({}초 이상 응답없음)",
                    sess->get_session_id(), std::chrono::duration_cast<std::chrono::seconds>(now - last).count());
            }
        }
    });
}

// UDP 메시지 수신 처리
void DataHandler::on_udp_receive(const std::string& msg, const udp::endpoint& from, udp::socket& udp_socket) {
    // [1] 서버 전체 UDP 패킷 제한 (중복방지)
    if (!check_total_udp_rate_limit(udp_total_packet_count_, udp_total_packet_window_, TOTAL_LIMIT_PER_SEC)) {
        g_logger->warn("[UDP][FLOOD] 서버 전체 초과 ({}/1초)", udp_total_packet_count_.load());
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
                if (ec) g_logger->error("[UDP] Send error2: {}", ec.message());
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
        g_logger->warn("[UDP] 임의 패킷 차단: 닉네임 없음 / EP({}:{})", from.address().to_string(), from.port());
        return;
    }

    // [4] 세션 조회
    auto session = find_session_by_nickname(nickname);
    if (!session) {
        g_logger->warn("[UDP] 임의 패킷 차단: 닉네임({})/EP({}:{})", nickname, from.address().to_string(), from.port());
        return;
    }

    // [5] 토큰 검사 (udp_register만 예외)
    if (type != "udp_register") {
        if (token.empty() || session->get_udp_token() != token) {
            g_logger->warn("[UDP] Invalid or missing udp_token! nickname={}, remote_ep={}", nickname, from.address().to_string());
            nlohmann::json resp = { {"type", "error"}, {"msg", token.empty() ? "UDP token is required" : "Invalid UDP token"} };
            auto data = std::make_shared<std::string>(resp.dump());
            udp_socket.async_send_to(boost::asio::buffer(*data), from, [data](const boost::system::error_code&, size_t) {});
            return;
        }
    }

    // [6] 유저별 UDP Flood 제한
    if (!check_user_udp_rate_limit(*session, USER_LIMIT_PER_SEC)) {
        g_logger->warn("[UDP][FLOOD] 유저({}) 초과 ({}/1초)", session->get_nickname(), session->get_udp_packet_count());
        nlohmann::json resp = { {"type", "error"}, {"msg", "UDP rate limit exceeded"} };
        auto data = std::make_shared<std::string>(resp.dump());
        udp_socket.async_send_to(boost::asio::buffer(*data), from,
            [data](const boost::system::error_code& ec, size_t) {
                g_logger->warn("[UDP][브로드캐스트] 전송 에러 1{}", ec.message());
            });
        return;
    }

    // [7] 엔드포인트 변화 감지 및 갱신
    auto prev_ep = session->get_udp_endpoint();
    if (!prev_ep || *prev_ep != from) {
        g_logger->info("[UDP] endpoint 변경 감지! 이전: {}:{} → 신규: {}:{}",
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
        while (udp_send_queue_.size() >= kMaxUdpQueueSize) {
            g_logger->warn("[UDP][Global] udp_send_queue_ overflow (size={}), dropping oldest", udp_send_queue_.size());
            udp_send_queue_.pop();
        }
        //if (udp_send_queue_.size() > kMaxUdpQueueSize) {
            //// 정책1: 새 메시지 drop 및 경고
            //g_logger->warn("[UDP][Global] udp_send_queue_ overflow (size={}) - dropping message", udp_send_queue_.size());
            //return;
        //}
        for_each_session([&](const std::shared_ptr<SSLSession> sess) {
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
        g_logger->error("[try_send_next_udp] data is nullptr or empty!");
        udp_send_in_progress_ = false;
        return;
    }
    if (ep.address().is_unspecified() || ep.port() == 0) {
        g_logger->error("[try_send_next_udp] endpoint is unspecified/zero! {}:{}", ep.address().to_string(), ep.port());
        udp_send_in_progress_ = false;
        return;
    }
    if (!udp_socket.is_open()) {
        g_logger->error("[try_send_next_udp] udp_socket is closed!");
        udp_send_in_progress_ = false;
        return;
    }

    udp_socket.async_send_to(
        boost::asio::buffer(*data), ep,
        boost::asio::bind_executor(*udp_send_strand_,
            [this, &udp_socket, data](const boost::system::error_code& ec, std::size_t /*bytes*/) {
                if (ec) {
                    g_logger->warn("[UDP][send queue][strand] {} {}", ec.message(), "async_send_to 에러");
                }
                udp_send_in_progress_ = false;
                try_send_next_udp(udp_socket);
            }
        )
    );
}

// 미인증 세션 정리 함수 
void DataHandler::cleanup_unauth_sessions(size_t max_unauth) {
    vector<shared_ptr<SSLSession>> unauth_sessions;

    for_each_session([&](const shared_ptr<SSLSession> sess) {
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
    //auto it = zones_.find(zone_id);
    //if (it != zones_.end() && it->second) {
    //    it->second->broadcast(msg, udp_socket, sender_nickname);
    //}
}

// 세션을 zone에 등록
void DataHandler::assign_session_to_zone(std::shared_ptr<SSLSession> session, int zone_id) {
    //auto it = zones_.find(zone_id);
    //if (it != zones_.end()) {
    //    if (it->second->add_session(session)) {
    //        session->set_zone_id(zone_id);
    //        g_logger->info("[ZONE] 세션 {} → ZONE {} 배정", session->get_session_id(), zone_id);
    //    }
    //    else {
    //        g_logger->warn("[ZONE] 세션 {} → ZONE {} add_session 실패 (존 인원수/중복 체크)", session->get_session_id(), zone_id);
    //    }
    //}
    auto zone = zone_manager_.get_or_create_zone(zone_id, MAX_ZONE_SESSION_COUNT);
    if (zone->add_session(session)) {
        session->set_zone_id(zone_id);
        g_logger->info("[ZONE] 세션 {} → ZONE {} 배정", session->get_session_id(), zone_id);
    }
    else {
        g_logger->warn("[ZONE] 세션 {} → ZONE {} add_session 실패 (존 인원수/중복 체크)", session->get_session_id(), zone_id);
    }
}


std::shared_ptr<Zone> DataHandler::get_zone(int zone_id) {
    return zone_manager_.get_zone(zone_id);
    //auto it = zones_.find(zone_id);
    //if (it != zones_.end()) {
    //    return it->second;
    //}
    //return nullptr;
}