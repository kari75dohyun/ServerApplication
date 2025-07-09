#include "DataHandler.h" 
#include "SSLSession.h"
#include <boost/asio.hpp>
#include <iostream>
#include <nlohmann/json.hpp>
#include "Logger.h"

using json = nlohmann::json;
using namespace std;
using namespace boost::asio;
using boost::asio::ip::udp;

DataHandler::DataHandler(boost::asio::io_context& io)
    : shard_count(std::max(4u, std::thread::hardware_concurrency() * 2)),
    session_buckets(shard_count),
    session_mutexes(shard_count),
    dispatcher_(this),
    // 글로벌 keepalive 관련 타이머 초기화
    keepalive_timer_(io) {
    start_keepalive_loop();  // 생성자에서 타이머 시작
}

void DataHandler::add_session(int session_id, std::shared_ptr<SSLSession> session) {
    int shard = get_shard(session_id);
    std::lock_guard<std::mutex> lock(session_mutexes[shard]);
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
    std::shared_ptr<SSLSession> session;
    {
        std::lock_guard<std::mutex> lock(session_mutexes[shard]);
        auto it = session_buckets[shard].find(session_id);
        if (it != session_buckets[shard].end()) {
            session = it->second; // shared_ptr를 먼저 확보
            session_buckets[shard].erase(it);
            //std::cout << "[remove_session] session_id=" << session_id << " removed from shard " << shard << std::endl;
            g_logger->info("[remove_session] session_id= {}", session_id, " removed from shard ", shard);
        }
        else {
            //std::cout << "[remove_session] session_id=" << session_id << " not found (already removed?)" << std::endl;
            g_logger->info("[remove_session] session_id= {}", session_id, " not found (already removed?)");
        }
    }
    // 락을 풀고 나서 release를 호출 (컨테이너/락과 완전히 분리)
    if (session && !session->is_closed()) {  // 중복 반환 방지!
        auto pool = get_session_pool();
        if (pool) {
            pool->release(session);
        }
    }
}

shared_ptr<SSLSession> DataHandler::get_session(int session_id) {
    int shard = get_shard(session_id);
    std::lock_guard<std::mutex> lock(session_mutexes[shard]);
    auto it = session_buckets[shard].find(session_id);
    if (it != session_buckets[shard].end())
        return it->second;
    return nullptr;
}

void DataHandler::do_handshake(std::shared_ptr<SSLSession> session) {
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
                    std::cerr << "Handshake failed: " << ec.message() << std::endl;
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
        //std::cerr << "[WARN] 중복 do_read 감지! session_id=" << session->get_session_id() << std::endl;
        g_logger->info("[WARN] 중복 do_read 감지! session_id= {}", session->get_session_id());
        return;
    }
    // do_read를 직접 호출하지 않고, post_task로 감싼다.
    session->post_task([this, session]() {
        auto self = session;
        auto& strand = session->get_strand();
        session->get_socket().async_read_some(
            buffer(session->get_data(), sizeof(session->get_data())),
            boost::asio::bind_executor(strand, [this, self](const boost::system::error_code& ec, std::size_t length) {
                // [2] 콜백 진입 시 반드시 해제!
                self->release_read();

                try {
                    if (!ec) {
                        // 1. 누적 버퍼에 append
                        self->get_msg_buffer().append(self->get_data(), length);

                        // 2. 여러 메시지 추출 및 처리
                        while (auto opt_msg = self->get_msg_buffer().extract_message()) {
                            try {
                                //std::cout << "[DEBUG] dispatching message" << std::endl;
                                json msg = json::parse(*opt_msg);
                                dispatcher_.dispatch(self, msg);
                            }
                            catch (const std::exception& e) {
                                std::cerr << "[JSON parsing error] " << e.what() << " / data: " << *opt_msg << std::endl;
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
                        std::cout << "Client disconnected." << std::endl;
                        // 퇴장 알림
                        std::string nickname = self->get_nickname();
                        json notice;
                        notice["type"] = "notice";
                        notice["msg"] = nickname + " has left.";
                        broadcast(notice.dump() + "\n", self->get_session_id(), self);

                        self->close_session();  // 세션 종료
                    }
                    else if (ec == boost::asio::error::connection_reset) {
                        std::cout << "Client forcibly disconnected." << std::endl;
                        std::string nickname = self->get_nickname();
                        if (nickname.empty()) {
                            std::cerr << "Client disconnected without a nickname." << std::endl;
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
                        //std::cout << "[INFO] Read cancelled by server shutdown or session close." << std::endl;
                        // 로그를 아예 안 찍거나, INFO/DEBUG로만 출력
                    }
                    else {
                        std::cerr << "Read failed: [" << ec.value() << "] " << ec.message() << std::endl;
                        self->close_session();
                    }
                }
                catch (const std::exception& e) {
                    std::cerr << "[FATAL][do_read handler 예외] " << e.what() << std::endl;
                    self->close_session();
                }
                self->run_next_task(); // 항상 마지막에!

                })
        );
        });
}

void DataHandler::broadcast(const std::string& msg, int sender_session_id, std::shared_ptr<SSLSession> /*session*/) {
    auto shared_msg = std::make_shared<std::string>(msg);
    std::vector<std::shared_ptr<SSLSession>> targets;

    // 1. 각 샤드별로 lock (세션 포인터만 복사)
    for (unsigned int shard = 0; shard < shard_count; ++shard) {
        std::lock_guard<std::mutex> lock(session_mutexes[shard]);
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
            //std::cout << "[broadcast] target session_id=" << sess->get_session_id()
            //    << " (sender_session_id=" << sender_session_id << ")" << std::endl;

            g_logger->info("[broadcast] target session_id= {}", sess->get_session_id(), " (sender_session_id= {}", sender_session_id);

            sess->post_write(shared_msg);  // <--- 모든 세션이 같은 메시지 객체를 참조!
        }
        catch (const std::exception& e) {
            //std::cerr << "[broadcast] Exception scheduling broadcast: " << e.what() << std::endl;
            g_logger->info("[broadcast] Exception scheduling broadcast: {}", e.what());
        }
    }
}

// 전체 세션을 "정확하게" 순회 (락 순서대로 걸고 해제)
void DataHandler::for_each_session(std::function<void(const std::shared_ptr<SSLSession>&)> fn) {
    // 1. 모든 샤드 lock (락 순서 꼭 지켜야 deadlock 방지)
    for (unsigned int i = 0; i < shard_count; ++i)
        session_mutexes[i].lock();

    // 2. 모든 세션에 대해 콜백 호출
    for (unsigned int i = 0; i < shard_count; ++i) {
        for (const auto& [id, sess] : session_buckets[i]) {
            fn(sess);
        }
    }

    // 3. lock 해제 (순서 무관, but 관례적으로 0~N순서)
    for (unsigned int i = 0; i < shard_count; ++i)
        session_mutexes[i].unlock();
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
void DataHandler::broadcast_strict(const std::string& msg) {
    auto shared_msg = std::make_shared<std::string>(msg);
    for_each_session([shared_msg](const std::shared_ptr<SSLSession>& sess) {
        if (sess) sess->post_write(shared_msg);
        });
}

void DataHandler::register_nickname(const std::string& nickname, std::shared_ptr<SSLSession> session) {
    std::lock_guard<std::mutex> lock(nickname_mutex_);
    nickname_to_session_[nickname] = session;
}
void DataHandler::unregister_nickname(const std::string& nickname, std::shared_ptr<SSLSession> session) {
    std::lock_guard<std::mutex> lock(nickname_mutex_);
    auto it = nickname_to_session_.find(nickname);
    if (it != nickname_to_session_.end()) {
        // 세션 포인터 일치 시에만 제거
        if (!it->second.expired() && it->second.lock() == session)
            nickname_to_session_.erase(it);
    }
}
std::shared_ptr<SSLSession> DataHandler::find_session_by_nickname(const std::string& nickname) {
    std::lock_guard<std::mutex> lock(nickname_mutex_);
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
            start_keepalive_loop(); // 반복
        }
        });
}

// 글로벌 keepalive 체크
void DataHandler::do_keepalive_check() {
    auto now = std::chrono::steady_clock::now();

    // 1. close해야 할 세션을 임시로 모아둘 벡터
    std::vector<std::shared_ptr<SSLSession>> sessions_to_close;

    for_each_session([this, now, &sessions_to_close](std::shared_ptr<SSLSession> sess) {
        if (!sess) return;

        if (sess->is_nickname_registered() == false) return; // 닉네임 등록 안된 세션은 skip

        // 클라가 하트비트 보내는 걸로 변경 되어서 삭제
        //// 1. ping 보내기 (ping_interval_ 간격마다)
        //if ((now - sess->get_last_alive_time()) > ping_interval_) {
        //    std::cout << "[PING] session_id=" << sess->get_session_id() << std::endl;
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
        //std::cout << "[KEEPALIVE TIMEOUT] session_id=" << sess->get_session_id() << " - close session" << std::endl;
        g_logger->info("[KEEPALIVE TIMEOUT] session_id= {}", sess->get_session_id(), "- close session");
        sess->close_session();
    }
}

// UDP 메시지 수신 처리
void DataHandler::on_udp_receive(const std::string& msg, const udp::endpoint& from, udp::socket& udp_socket) {
    std::shared_ptr<SSLSession> session = nullptr;
    std::string nickname;

    try {
        auto j = nlohmann::json::parse(msg);
        nickname = j.value("nickname", "");
        if (!nickname.empty()) {
            session = find_session_by_nickname(nickname);
            if (session) {
                session->set_udp_endpoint(from);
				g_logger->info("[UDP] EndPoint registe from nickname: {}", nickname);
            }
        }
    }
    catch (const std::exception& e) {
        // JSON 파싱 실패 시, 원래 메시지 그대로 에코
        g_logger->info("[UDP] JSON Parse failed: {}", e.what(), " / original: {}", msg);
        std::string response = "Echo(UDP): " + msg;
        auto data = std::make_shared<std::string>(response);
        udp_socket.async_send_to(
            boost::asio::buffer(*data), from,
            [data](const boost::system::error_code& ec, std::size_t) {
                if (ec) {
                    //LOG_ERROR("[UDP] Send error: ", ec.message());
                    g_logger->error("[UDP] Send error: {}", ec.message());
                }
            }
        );
    }

    dispatcher_.dispatch_udp(session, msg, from, udp_socket);
}

void DataHandler::udp_broadcast(const std::string& msg, udp::socket& udp_socket, const std::string& sender_nickname) {
    for_each_session([&](std::shared_ptr<SSLSession> sess) {
        if (!sess) return;
        if (sess->get_nickname().empty()) return;
        if (sess->get_nickname() == sender_nickname) return; // 자기자신 제외

        auto udp_ep = sess->get_udp_endpoint();
        if (udp_ep) {
            auto data = std::make_shared<std::string>(msg);
            udp_socket.async_send_to(
                boost::asio::buffer(*data), *udp_ep,
                [data](const boost::system::error_code& ec, std::size_t /*bytes_sent*/) {
                    if (ec) g_logger->error("UDP send error: {}", ec.message());
                }
            );
        }
        });
}

// 미인증 세션 정리 함수 
void DataHandler::cleanup_unauth_sessions(size_t max_unauth) {
    std::vector<std::shared_ptr<SSLSession>> unauth_sessions;

    for_each_session([&](std::shared_ptr<SSLSession> sess) {
        if (!sess) return;
        SessionState state = sess->get_state();
        if (state == SessionState::Handshaking || state == SessionState::LoginWait) {
            unauth_sessions.push_back(sess);
        }
        });

    // 임계치 초과시, 오래된 세션부터 정리 (예: 타임스탬프 기준 정렬)
    if (unauth_sessions.size() > max_unauth) {
        // 오래된 순으로 정렬 (예: get_last_alive_time() 활용)
        std::sort(unauth_sessions.begin(), unauth_sessions.end(),
            [](const auto& a, const auto& b) {
                return a->get_last_alive_time() < b->get_last_alive_time();
            });

        size_t count_to_close = unauth_sessions.size() - max_unauth;
        for (size_t i = 0; i < count_to_close; ++i) {
            unauth_sessions[i]->close_session();
        }
    }
}

