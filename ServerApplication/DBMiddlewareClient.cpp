#include "DBMiddlewareClient.h"
#include "AppContext.h"
#include "Logger.h"
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif
#include <nlohmann/json.hpp>
#include "generated/wire.pb.h"

using boost::asio::ip::tcp;

DBMiddlewareClient::DBMiddlewareClient(boost::asio::io_context& io,
    std::string host,
    uint16_t port,
    OnMessageFn on_message,
    std::string login_nickname,
    std::string secret)
    : socket_(io),
    strand_(boost::asio::make_strand(io)),
    resolver_(io),
    reconnect_timer_(io),
    heartbeat_timer_(io),
    host_(std::move(host)),
    port_(port),
    server_login_nickname_(std::move(login_nickname)),
    secret_(std::move(secret)),
    on_message_(std::move(on_message)),
    timeout_count_(0) {
}


void DBMiddlewareClient::start() {
    stopping_.store(false);
    boost::asio::dispatch(strand_, [self = shared_from_this()] {
        self->do_connect();
        });
}

void DBMiddlewareClient::stop() {
    stopping_.store(true);
    boost::asio::dispatch(strand_, [self = shared_from_this()] {
        self->close_socket();
        self->reconnect_timer_.cancel();
        self->heartbeat_timer_.cancel();
        });
}

void DBMiddlewareClient::do_connect() {
    if (stopping_.load()) return;

    // (선택) 잘못된 포트 보호 가드: 게임서버 포트로 연결하려 하면 중단
    int game_port = AppContext::instance().config.value("tcp_port", 12345);
    if (static_cast<int>(port_) == game_port) {
        AppContext::instance().logger->error(
            "[DBMW] Misconfiguration: dbmw_port({}) == tcp_port({}) — abort connect",
            port_, game_port);
        return;
    }

    AppContext::instance().logger->info("[DBMW] connecting to {}:{}", host_, port_);
    auto self = shared_from_this();

    resolver_.async_resolve(host_, std::to_string(port_),
        boost::asio::bind_executor(strand_,
            [this, self](const boost::system::error_code& ec, tcp::resolver::results_type results) {
                if (ec) {
                    AppContext::instance().logger->warn("[DBMW] resolve failed: {}", ec.message());
                    schedule_reconnect();
                    return;
                }

                boost::asio::async_connect(socket_, results,
                    boost::asio::bind_executor(strand_,
                        [this, self](const boost::system::error_code& ec2, const tcp::endpoint&) {
                            if (ec2) {
                                AppContext::instance().logger->warn("[DBMW] connect failed: {}", ec2.message());
                                schedule_reconnect();
                                return;
                            }

                            connected_.store(true);
                            reconnect_sec_ = 2;
                            server_login_sent_.store(false);
                            authed_.store(false);

                            AppContext::instance().logger->info("[DBMW] connected");

                            // 접속 직후 로그인 (secret + json -> framed)
                            server_send_login();

                            // 수신 시작
                            do_read_some();

                            // 하트비트 타이머 시작
                            arm_heartbeat_timer();
                        }));
            }));
}

void DBMiddlewareClient::note_heartbeat_ack() {
    missed_heartbeat_.store(0, std::memory_order_relaxed);
}

void DBMiddlewareClient::schedule_reconnect() {
    connected_.store(false);
    if (stopping_.load()) return;

    reconnect_sec_ = std::min(reconnect_sec_ * 2, reconnect_sec_max_);
    AppContext::instance().logger->info("[DBMW] reconnect in {}s", reconnect_sec_);

    reconnect_timer_.expires_after(std::chrono::seconds(reconnect_sec_));
    auto self = shared_from_this();
    reconnect_timer_.async_wait(
        boost::asio::bind_executor(strand_,
            [this, self](const boost::system::error_code& ec) {
                if (!ec && !stopping_.load()) do_connect();
            }));
}

void DBMiddlewareClient::do_read_some() {
    if (!connected_.load()) return;

    auto self = shared_from_this();
    socket_.async_read_some(
        boost::asio::buffer(read_buf_),
        boost::asio::bind_executor(strand_,
            [this, self](const boost::system::error_code& ec, std::size_t len) {
                if (ec) {
                    AppContext::instance().logger->warn("[DBMW] read error: {}", ec.message());
                    close_socket();
                    schedule_reconnect();
                    return;
                }

                msg_buf_mgr_.append(read_buf_.data(), len);
                while (auto opt = msg_buf_mgr_.extract_message()) {
                    const std::string& raw = *opt;

                    // 1. secret 접두어 제거
                    std::string_view view(raw);
                    if (view.size() >= secret_.size() && memcmp(view.data(), secret_.data(), secret_.size()) == 0) {
                        view.remove_prefix(secret_.size());
                    }

                    // 2. Protobuf Envelope 시도
                    wire::Envelope env;
                    if (env.ParseFromArray(view.data(), (int)view.size())) {
                        if (on_message_pb_) on_message_pb_(env);
                        continue;
                    }

                    // 3. 실패하면 JSON 시도
                    try {
                        //JSON 파싱
                        auto j = nlohmann::json::parse(*opt);

                        // 타입별 처리는 전부 Router(on_message_)에서
                        if (on_message_) {
                            on_message_(j);
                        }
                    }
                    catch (const std::exception& e) {
                        AppContext::instance().logger->warn(
                            "[DBMW] json parse error: {}", e.what());
                    }
                }

                //while (auto opt = msg_buf_mgr_.extract_message()) {
                //    try {
                //        //auto j = nlohmann::json::parse(*opt);

                //        //// 로그인 성공 신호 감지
                //        //auto t = j.value("type", std::string());
                //        //if (t == "login_ok" || (t == "login" && j.value("result", std::string()) == "ok")) {
                //        //    authed_.store(true);
                //        //    AppContext::instance().logger->info("[DBMW] login_ok");
                //        //}

                //        if (on_message_) on_message_(j);
                //    }
                //    catch (const std::exception& e) {
                //        AppContext::instance().logger->warn("[DBMW] json parse error: {}", e.what());
                //    }
                //}

                if (msg_buf_mgr_.was_last_clear_by_invalid_length()) {
                    AppContext::instance().logger->warn("[DBMW] invalid packet length -> reconnect");
                    close_socket();
                    schedule_reconnect();
                    return;
                }

                do_read_some();
            }));
}

void DBMiddlewareClient::flush_write_queue() {
    if (write_queue_.empty()) { write_in_progress_ = false; return; }
    auto data = write_queue_.front();

    auto self = shared_from_this();
    boost::asio::async_write(
        socket_, boost::asio::buffer(*data),
        boost::asio::bind_executor(strand_,
            [this, self, data](const boost::system::error_code& ec, std::size_t) {
                if (ec) {
                    AppContext::instance().logger->warn("[DBMW] write error: {}", ec.message());
                    close_socket();
                    schedule_reconnect();
                    return;
                }
                write_queue_.pop();
                flush_write_queue();
            }));
}

void DBMiddlewareClient::close_socket() {
    boost::system::error_code ec;
    if (socket_.is_open()) {
        socket_.shutdown(tcp::socket::shutdown_both, ec);
        socket_.close(ec);
    }
    connected_.store(false);
}

/* =========================
 *   공통 전송 유틸들
 * ========================= */

 // 임의 body를 4바이트 길이(빅엔디안)로 프레이밍해서 전송
void DBMiddlewareClient::send_framed(const std::string& body) {
    uint32_t len = static_cast<uint32_t>(body.size());
    uint32_t len_net = htonl(len);

    auto frame = std::make_shared<std::string>();
    frame->resize(4 + body.size());
    std::memcpy(frame->data(), &len_net, 4);
    std::memcpy(frame->data() + 4, body.data(), body.size());

    auto self = shared_from_this();
    boost::asio::dispatch(strand_, [this, self, frame] {
        if (!connected_.load()) {
            AppContext::instance().logger->warn("[DBMW] send dropped: not connected");
            return;
        }
        bool idle = write_queue_.empty();
        write_queue_.push(frame);
        if (idle && !write_in_progress_) {
            write_in_progress_ = true;
            flush_write_queue();
        }
        });
}

std::string DBMiddlewareClient::generate_request_id() {
   static std::atomic<uint64_t> counter{ 0 };
   uint64_t id = ++counter;
   return "req_" + std::to_string(id);
}

//  JSON만 body로 -> 프레이밍 후 전송
void DBMiddlewareClient::send_json(nlohmann::json j) {
    // 1) 요청 ID 붙이기 (없으면 생성)
    std::string req_id;
    if (j.contains("request_id")) {
        req_id = j["request_id"].get<std::string>();
    }
    else {
        req_id = generate_request_id(); // UUID나 증가 카운터
        j["request_id"] = req_id;
    }

    auto payload = j.dump();
    send_framed(payload);   // 기존 전송

	// 2) 타이머 생성 (5초)
    auto timer = std::make_shared<boost::asio::steady_timer>(strand_);
    timer->expires_after(std::chrono::seconds(5));
    {
        std::lock_guard<std::mutex> lock(request_timers_mutex_);
        request_timers_[req_id] = timer;
    }

    // 3) 타임아웃 핸들러
    auto self = shared_from_this();
    timer->async_wait([this, self, req_id, timer](const boost::system::error_code& ec) {
        if (!ec) {
            timeout_count_.fetch_add(1, std::memory_order_relaxed);
            AppContext::instance().logger->warn("[DBMW] Request timeout detected! id={}", req_id);
            // 여기서 세션 끊거나 alert 전송 가능
        }
        // 완료된 timer는 map에서 제거
        std::lock_guard<std::mutex> lock(request_timers_mutex_);
        request_timers_.erase(req_id);
        });
}

// secret + JSON -> 프레이밍 후 전송 (login/keepalive 전용)
void DBMiddlewareClient::send_secure_json(nlohmann::json j) {
    //std::string body = secret_ + j.dump();
    //send_framed(body);
    // 요청 ID 삽입
    std::string req_id;
    if (j.contains("request_id")) {
        req_id = j["request_id"].get<std::string>();
        
    }
    else {
        req_id = generate_request_id();
        j["request_id"] = req_id;
    }
    
    std::string body = secret_ + j.dump();
    send_framed(body);
    // 요청별 타이머 (5초)
    auto timer = std::make_shared<boost::asio::steady_timer>(strand_);
    timer->expires_after(std::chrono::seconds(5));
    {
        std::lock_guard<std::mutex> lock(request_timers_mutex_);
        request_timers_[req_id] = timer;
    }

    auto self = shared_from_this();
    timer->async_wait([this, self, req_id, timer](const boost::system::error_code& ec) {
        if (!ec) {
            timeout_count_.fetch_add(1, std::memory_order_relaxed);
            AppContext::instance().logger->warn("[DBMW] Secure request timeout: req_id={}", req_id);
        }
        // 완료된 timer는 map에서 제거
        std::lock_guard<std::mutex> lock(request_timers_mutex_);
        request_timers_.erase(req_id);
        });
}

/* =========================
 *   로그인 / 하트비트
 * ========================= */

void DBMiddlewareClient::server_send_login() {
    if (server_login_sent_.exchange(true)) return;

    nlohmann::json Serverlogin = {
        {"type", "serverlogin"},
        {"nickname", server_login_nickname_}
    };

    AppContext::instance().logger->info(
        "[DBMW] send login nickname={}", server_login_nickname_
    );

    // secret + json 을 바디로 -> framed 전송
    send_secure_json(Serverlogin);
}

void DBMiddlewareClient::arm_heartbeat_timer() {
    heartbeat_timer_.expires_after(std::chrono::seconds(heartbeat_sec_));
    auto self = shared_from_this();
    heartbeat_timer_.async_wait(
        boost::asio::bind_executor(strand_,
            [this, self](const boost::system::error_code& e) {
                if (e || stopping_.load()) return;

                if (connected_.load() && authed_.load()) {
                    nlohmann::json heartbeat = {
                        {"type","keepalive"},
                        {"nickname", server_login_nickname_}
                    };

                    // secret + json 을 바디로 -> framed 전송
                    send_secure_json(heartbeat);
                    AppContext::instance().logger->info("[DBMW] keepalive sent");

                    // 보낸 뒤 “미수신 카운터” 증가. ACK 오면 note_heartbeat_ack()이 0으로 리셋해줌
                    int misses = missed_heartbeat_.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (misses >= heartbeat_miss_limit_) {
                        AppContext::instance().logger->warn(
                            "[DBMW] heartbeat missed {} times — reconnecting", misses);
                        close_socket();
                        schedule_reconnect();
                        return;
                    }
                }

                // 재-arming
                arm_heartbeat_timer();
            }));
}

void DBMiddlewareClient::send_proto(const google::protobuf::MessageLite& msg) {
    std::string body;
    body.reserve(msg.ByteSizeLong());
    msg.SerializeToString(&body);    // 실패 체크는 필요시 추가
    send_framed(body);
}

void DBMiddlewareClient::send_secure_proto(const google::protobuf::MessageLite& msg) {
    std::string plain;
    msg.SerializeToString(&plain);
    std::string body = secret_ + plain;
    send_framed(body);
}
