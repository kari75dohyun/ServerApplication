#include <iostream>  
#include "Session.h"  
#include "DataHandler.h"  
#include <boost/asio.hpp>
#include "Logger.h"
#include <random>
#include <sstream>
#include "Utility.h"
#include <nlohmann/json.hpp>
#include "AppContext.h"

using namespace std;
using namespace boost::asio;
using boost::asio::ip::tcp;
using json = nlohmann::json;

Session::Session(tcp::socket socket, int session_id, weak_ptr<DataHandler> data_handler)
    : socket_(std::move(socket)),
    session_id_(session_id),
    data_handler_(data_handler),
    strand_(boost::asio::make_strand(socket_.get_executor())),
    login_timer_(strand_),
    retry_timer_(strand_) {
    // Session에서 각자 keepalive 타이머를 관리 하는 방식
    //ping_timer_(socket_.get_executor()),
    //keepalive_timer_(socket_.get_executor()) {
    memset(data_, 0, sizeof(data_));
    // 글로벌 구조에서는 세션 생성시점에 마지막 pong 시간 초기화!
    last_alive_time_ = std::chrono::steady_clock::now();
}

Session::~Session() {
    //cerr << "[세션 소멸] id=" << session_id_ << endl;  
    //LOG_ERROR("[세션 소멸] id=", session_id_);
    AppContext::instance().logger->error("[세션 소멸] id= {}", session_id_);
}

void Session::start() {
    do_read();
    start_login_timeout();    // 타이머 시작 추가!
}

int Session::get_session_id() const {
    return session_id_;
}

void Session::post_task(std::function<void()> fn) {
    auto self = shared_from_this();
    //std::cout << "[DEBUG] post_task called (session_id=" << session_id_ << ")" << std::endl;
    boost::asio::dispatch(strand_, [this, self, fn = std::move(fn)]() mutable {
        //std::cout << "[DEBUG] task enqueued" << std::endl;
        if (task_queue_.size() >= static_cast<size_t>(AppContext::instance().config.value("max_task_queue", 1000))) {
            //std::cerr << "[WARN] task_queue_ overflow! (session_id=" << session_id_ << ")\n";
            //LOG_WARN("[WARN] task_queue_ overflow! (session_id=", session_id_);
            AppContext::instance().logger->warn("[WARN] task_queue_ overflow! (session_id= {}", session_id_);
            close_session();  // 또는 에러 메시지, 혹은 그냥 무시
            return;
        }
        task_queue_.push(std::move(fn));
        if (!task_running_) {
            task_running_ = true;
            run_next_task();
        }
        });
}

void Session::run_next_task() {
    //std::cout << "[DEBUG] run_next_task()" << std::endl;
    if (task_queue_.empty()) {
        task_running_ = false;
        return;
    }
    auto fn = std::move(task_queue_.front());
    task_queue_.pop();
    //std::cout << "[DEBUG] running fn()" << std::endl;
    fn();  // 비동기 작업 진입, 콜백 마지막에 run_next_task() 호출!
}

// (1) post_write(기존 string용 → shared_ptr 버전으로 변환해서 호출)
void Session::post_write(const std::string& msg) {
    post_write(std::make_shared<std::string>(msg));
}

// (2) post_write(shared_ptr 버전, 핵심 로직)
void Session::post_write(const std::shared_ptr<std::string> msg) {
    auto self = shared_from_this();
    boost::asio::dispatch(strand_, [this, self, msg]() {
		// 기존 write_queue_ 사이즈 초과시 무조건 close 하던거 삭제 enqueue_write 에서 처리 
        //if (write_queue_.size() >= MAX_WRITE_QUEUE) {
        //    std::cerr << "[WARN] write_queue_ overflow! (session_id=" << session_id_ << ")\n";
        //    close_session();
        //    return;
        //}  
        bool idle = write_queue_.empty();
        //write_queue_.push(msg);
        enqueue_write(msg);
        if (idle) {
            write_in_progress_ = true;
            do_write_queue();
        }
        });
}

void Session::do_write_queue() {
    if (get_state() == SessionState::Closed) return;
    auto self = shared_from_this();
    if (write_queue_.empty()) {
        write_in_progress_ = false;
        return;
    }
    auto msg = write_queue_.front();
    uint64_t my_generation = generation_.load(std::memory_order_relaxed); // 세대 캡처

    // === 길이 프리픽스 만들기 ===
    uint32_t len = static_cast<uint32_t>(msg->size());
    uint32_t len_net = htonl(len); // 네트워크 바이트 오더(빅엔디안)
    std::array<char, 4> len_buf;
    memcpy(len_buf.data(), &len_net, 4);

    // === 버퍼 합치기 ===
    std::vector<boost::asio::const_buffer> buffers;
    buffers.push_back(boost::asio::buffer(len_buf));
    buffers.push_back(boost::asio::buffer(*msg));

    //boost::asio::async_write(socket_, boost::asio::buffer(*msg),
    boost::asio::async_write(socket_, buffers,      // 4바이트 프리픽스로 변경
        boost::asio::bind_executor(strand_,
            [this, self, my_generation](const boost::system::error_code& ec, std::size_t /*length*/) {
                try {
                    // === generation check ===
                    if (my_generation != generation_.load(std::memory_order_relaxed)) {
                        AppContext::instance().logger->warn("[do_write_queue] Stale write callback (세대 mismatch)! 세션ID={} 무시", get_session_id());
                        return;
                    }

                    if (ec) {
                        AppContext::instance().logger->error("[do_write_queue] error: {}", ec.message());
                        //close_session();
                        close_session();
                        return;
                    }
                    write_queue_.pop();
                    do_write_queue();
                }
                catch (const std::exception& e) {
                    AppContext::instance().logger->error("[do_write_queue][Exception] {}", e.what());
                    //close_session();
                    close_session();
                }
            }
        )
    );
}

void Session::start_login_timeout() {
    if (get_state() == SessionState::Closed) {
        AppContext::instance().logger->warn("Closed session: Callback/Ignore Message [session_id={}]", get_session_id());
        return;
    }
    login_timer_.expires_after(std::chrono::seconds(AppContext::instance().config.value("login_timeout_seconds", 90))); // 예: 90초
    auto self = shared_from_this();
    login_timer_.async_wait([this, self](const boost::system::error_code& ec) {
        if (!ec && !nickname_registered_) {
            std::cerr << "[LOGIN TIMEOUT] session_id=" << session_id_ << " Login timed out, session ended!" << std::endl;
            post_write(R"({"type":"notice","msg":"Your connection has been terminated due to a login timeout."})" "\n");
            //close_session();
            close_session();
        }
        });
}

void Session::on_nickname_registered() {
    nickname_registered_ = true;
    set_state(SessionState::Ready); // 로그인 성공 상태로!
    login_timer_.cancel(); // 인수 제거하여 타이머 취소  
}

void Session::reset(boost::asio::ip::tcp::socket&& new_socket, int session_id) {
    boost::system::error_code ec;

    // (1) 기존 소켓 비동기 작업 모두 취소 및 안전하게 닫기
    if (socket_.is_open()) {
        socket_.cancel(ec);
        socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        socket_.close(ec);
    }

    // (2) 새 소켓 move 할당
    socket_ = std::move(new_socket);

    // (3) strand, 타이머 등 재할당 (필수)
    strand_ = boost::asio::make_strand(socket_.get_executor());
    login_timer_ = boost::asio::steady_timer(strand_);

    // (4) 내부 상태 모두 초기화
    session_id_ = session_id;
    nickname_.clear();
    line_buffer_.clear();
    message_.clear();
    memset(data_, 0, sizeof(data_));

    closed_ = false;
    read_pending_ = false;
    task_running_ = false;
    while (!task_queue_.empty()) task_queue_.pop();
    while (!write_queue_.empty()) write_queue_.pop();

    set_state(SessionState::Handshaking);   // 초기 상태

    udp_endpoint_.reset();
    udp_ep_update_time_ = std::chrono::steady_clock::time_point{};
    last_udp_alive_time_.store(std::chrono::steady_clock::time_point{});
    udp_token_.clear();
    udp_packet_count_ = 0;
    udp_packet_window_ = std::chrono::steady_clock::time_point{};

    zone_id_ = 0;

    last_alive_time_ = std::chrono::steady_clock::now();
    nickname_registered_ = false;

    msg_buf_mgr_.clear();

    generation_.fetch_add(1, std::memory_order_relaxed);

    AppContext::instance().logger->info("[Session][reset] 세션 {} 내부 상태 초기화 완료", session_id_);
}



std::string Session::get_client_ip() const {
    try {
        return socket_.lowest_layer().remote_endpoint().address().to_string();
    }
    catch (const std::exception&) {
        return "unknown";
    }
}

unsigned short Session::get_client_port() const {
    try {
        return socket_.lowest_layer().remote_endpoint().port();
    }
    catch (const std::exception&) {
        return 0;
    }
}

void Session::update_udp_alive_time() {
    last_udp_alive_time_ = std::chrono::steady_clock::now();
}

std::chrono::steady_clock::time_point Session::get_last_udp_alive_time() const {
    return last_udp_alive_time_.load();
}

void Session::enqueue_write(std::shared_ptr<std::string> msg) {
    // 1. 80% 초과 경고만
    if (write_queue_.size() >= static_cast<size_t>(AppContext::instance().config.value("write_queue_warn_threshold", 80))) {
        AppContext::instance().logger->warn("[Session][enqueue_write] write_queue 임계치(80%) 초과: size={}", write_queue_.size());
    }

    // 2. FULL(100%)이면 가장 오래된 것 drop, 연속이면 close
    if (write_queue_.size() >= static_cast<size_t>(AppContext::instance().config.value("max_write_queue_size", 100))) {
        AppContext::instance().logger->warn("[Session][enqueue_write] write_queue FULL! 가장 오래된 메시지 drop, 새 메시지 push");
        write_queue_.pop();

        // 연속 FULL 카운트 증가
        ++write_queue_overflow_count_;
        if (write_queue_overflow_count_ >= static_cast<size_t>(AppContext::instance().config.value("write_queue_overflow_limit", 10))) {
            AppContext::instance().logger->error("[Session][enqueue_write] write_queue FULL 연속 {}회, 세션 종료!", write_queue_overflow_count_);
            closed_ = true;
            // 실제 종료 처리가 필요하다면 여기에 추가!
            // 예: socket_.lowest_layer().close();
            return;
        }
    }
    else {
        write_queue_overflow_count_ = 0; // 정상상태면 count 리셋
    }

    // 3. push
    write_queue_.push(msg);
}

void Session::close_session() {
    if (closed_.exchange(true)) return;
    set_state(SessionState::Closed);

    auto self = shared_from_this();

    try {
        // TCP 소켓 안전하게 닫기 (비동기 종료 없음)
        boost::system::error_code ec;

        socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        if (ec) AppContext::instance().logger->error("[close_session] tcp shutdown error: {}", ec.message());

        socket_.close(ec);
        if (ec) AppContext::instance().logger->error("[close_session] tcp close error: {}", ec.message());

        // 마지막에 세션 제거 (핸들러에서 release 등 포함)
        if (auto handler = data_handler_.lock()) {
            handler->remove_session(session_id_);
        }

		cleanup();  // 만약 제거가 안됐으면, 존에서 제거 및 풀 반환
    }
    catch (const std::exception& e) {
        AppContext::instance().logger->error("[close_session] Exception: {}", e.what());
    }
}


void Session::cleanup() {
    auto self = shared_from_this();
    // 1. 존에서 제거 (존에 등록돼 있을 경우만)
    if (zone_id_ != -1) {
        auto zone = get_zone();
        if (zone) {
            zone->remove_session(self);
            set_zone_id(-1);  // 존 제거 후 무조건 초기화
            clear_zone();
            AppContext::instance().logger->info("[Session] 존에서 세션 제거 완료: zone_id={}, session_id={}", zone_id_, session_id_);
        }
    }

    // 2. Pool 반환 (세션 풀 사용 시)
    if (released_.exchange(true)) return;

    if (release_callback_) {
        release_callback_(shared_from_this());
    }
    else {
        AppContext::instance().logger->warn("[Session] release_callback_ 이 설정되지 않음! 세션이 풀로 반환되지 않을 수 있음.");
    }
}

void Session::do_read() 
{
    auto self = shared_from_this();
    // [1] 중복 read 방지!
    if (get_state() == SessionState::Closed) {
        AppContext::instance().logger->warn("Closed session: 콜백/메시지 무시 [session_id={}]", get_session_id());
        return;
    }
    if (!try_acquire_read()) {
        //cerr << "[WARN] 중복 do_read 감지! session_id=" << session->get_session_id() << endl;
        AppContext::instance().logger->info("[WARN] 중복 do_read 감지! session_id= {}", get_session_id());
        return;
    }
    // do_read를 직접 호출하지 않고, post_task로 감싼다.
    post_task([this, self]() {
        auto& strand = get_strand();
        get_socket().async_read_some(
            buffer(get_data(), sizeof(get_data())),
            boost::asio::bind_executor(strand, [this, self](const boost::system::error_code& ec, size_t length) {
                // [2] 콜백 진입 시 반드시 해제!
                release_read();

                try {
                    if (!ec) {
                        // 1. 누적 버퍼에 append
                        get_msg_buffer().append(get_data(), length);

                        // 2. 여러 메시지 추출 및 처리
                        while (auto opt_msg = get_msg_buffer().extract_message()) {
                            try {
                                json msg = json::parse(*opt_msg);

                                if (auto handler = data_handler_.lock()) {
                                    handler->dispatch(self, msg);  // 바로 이렇게!
                                }
                            }
                            catch (const exception& e) {
                                cerr << "[JSON parsing error] " << e.what() << " / data: " << *opt_msg << endl;
                                set_message(R"({"type":"error","msg":"Message parsing failed"})" "\n");
                                //do_write(self);
                                post_write(get_message());
                                // 에러 시에도 계속 다음 메시지 분리/처리
                            }
                        }
                        // 비정상 길이 감지 로그 및 세션 종료 
                        if (get_msg_buffer().was_last_clear_by_invalid_length()) {
                            AppContext::instance().logger->warn("[TCP] 비정상/과도한 패킷 길이 감지! 세션 강제 종료 session_id={}", get_session_id());
                            close_session();
                            return;  // read loop 탈출
                        }

                        // 3. 계속해서 read (이 구조면 wrote 체크 필요 없음)
                        do_read();
                    }
                    else if (ec == boost::asio::error::eof) {
                        cout << "Client disconnected." << endl;
                        // 퇴장 알림
                        string nickname = self->get_nickname();
                        json notice;
                        notice["type"] = "notice";
                        notice["msg"] = nickname + " has left.";
                        // broadcast는 data_handler로 위임
                        if (auto handler = data_handler_.lock()) {
                            handler->broadcast(notice.dump() + "\n", get_session_id(), self);
                        }
                        //broadcast(notice.dump() + "\n", self->get_session_id(), self);

                        self->close_session();  // 세션 종료
                    }
                    else if (ec == boost::asio::error::connection_reset) {
                        cout << "Client forcibly disconnected." << endl;
                        string nickname = self->get_nickname();
                        if (nickname.empty()) {
                            AppContext::instance().logger->warn("[Connection reset] Client disconnected without a nickname. {}", self->get_session_id());
                            cerr << "Client disconnected without a nickname." << endl;
                        }
                        else {
                            json notice;
                            notice["type"] = "notice";
                            notice["msg"] = nickname + " has left.";
                            // broadcast는 data_handler로 위임
                            if (auto handler = data_handler_.lock()) {
                                handler->broadcast(notice.dump() + "\n", get_session_id(), self);
                            }
                            //broadcast(notice.dump() + "\n", self->get_session_id(), self);
                            std::cout << "Client with nickname '" << nickname << "' disconnected." << std::endl;
                        }

                        //self->close_session();
                        close_session();
                    }
                    else if (ec == boost::asio::error::operation_aborted) {
                        // [995] 소켓 종료, 타이머 취소 등에서 발생하는 "정상 종료 케이스"
                        // cout << "[INFO] Read cancelled by server shutdown or session close." << endl;
                        // 로그를 아예 안 찍거나, INFO/DEBUG로만 출력
                    }
                    else {
                        cerr << "Read failed: [" << ec.value() << "] " << ec.message() << endl;
                        //self->close_session();
                        close_session();
                    }
                }
                catch (const exception& e) {
                    cerr << "[FATAL][do_read handler 예외] " << e.what() << endl;
                    //self->close_session();
                    close_session();
                }
                self->run_next_task(); // 항상 마지막에!

                })
        );
        });
}

bool Session::checkUdpRateLimit() {
    using namespace std::chrono;

    auto now = steady_clock::now();
    auto elapsed = duration_cast<milliseconds>(now - lastUdpTokenRefill_).count();

    // 토큰 리필
    int refill = static_cast<int>((elapsed / 1000.0) * refillRatePerSec_);
    if (refill > 0) {
        udpTokens_ = std::min(udpTokens_ + refill, maxTokens_);
        lastUdpTokenRefill_ = now;
    }

    if (udpTokens_ > 0) {
        --udpTokens_;
        return true;  // 허용
    }
    else {
        return false;  // 너무 자주 보내서 차단
    }
}