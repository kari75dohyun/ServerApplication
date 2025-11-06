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
    life_state_.store(static_cast<uint8_t>(LifeState::Idle), std::memory_order_release);
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

std::shared_ptr<DataHandler> Session::get_handler_safe() {
    auto handler = data_handler_.lock();
    if (!handler) {
        AppContext::instance().logger->error(
            "[Session::get_handler_safe] DataHandler expired! session_id={}",
            session_id_
        );
        // DataHandler가 없으면 세션도 더 이상 의미 없음
        close_session();
    }
    return handler;
}

void Session::post_task(std::function<void()> fn) {
    auto self = shared_from_this();
    //std::cout << "[DEBUG] post_task called (session_id=" << session_id_ << ")" << std::endl;
	//dispatch => 즉시 처리 가능하면 실행 아니면 strand 큐에 넣기
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

// (1) post_write(기존 string용 -> shared_ptr 버전으로 변환해서 호출)
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
            [this, self, msg ,my_generation](const boost::system::error_code& ec, std::size_t /*length*/) {
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
                catch (const boost::system::system_error& e) {
                    using namespace boost::asio::error;
                    if (e.code() == operation_aborted) {
                        AppContext::instance().logger->info("[do_write_queue] Operation aborted (session_id={})", get_session_id());
                    }
                    else if (e.code() == broken_pipe) {
                        AppContext::instance().logger->warn("[do_write_queue] Broken pipe (client disconnected, session_id={})", get_session_id());
                    }
                    else {
                        AppContext::instance().logger->error("[do_write_queue] System error: {} (code={})", e.what(), e.code().value());
                        send_admin_alert("[ALERT] [do_write_queue] system_error: " + std::string(e.what()));
                    }
                    close_session();
                }
                catch (const std::bad_alloc& e) {
                    AppContext::instance().logger->critical("[do_write_queue] Memory allocation failure: {}", e.what());
                    send_admin_alert("[ALERT] Server memory allocation failure in do_write_queue!");
                    close_session();  // 즉시 세션 종료
                }
                catch (const std::exception& e) {
                    AppContext::instance().logger->error("[do_write_queue] Unexpected exception: {}", e.what());
                    close_session();
                }
                catch (...) {
                    AppContext::instance().logger->critical("[do_write_queue] Unknown fatal exception!");
                    send_admin_alert("[ALERT] Unknown fatal exception in do_write_queue()");
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

    udp_token_.clear();
    udp_token_issued_ = {};
    udp_token_ttl_sec_ = 300; // 기본값(설정에서 다시 세팅됨)

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
            //closed_ = true;
            //실제 종료 실행
            close_session();   // 바로 종료
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
    //if (closed_.exchange(true)) return;
    //set_state(SessionState::Closed);
    if (!try_close()) {
        AppContext::instance().logger->debug("[Session] 중복 close_session() 무시 id={}", session_id_);
        return;
    }
    set_state(SessionState::Closed);

    // 타이머 취소
    login_timer_.cancel();
    retry_timer_.cancel();

    //auto self = shared_from_this();

    try {
        // TCP 소켓 안전하게 닫기 (비동기 종료 없음)
        boost::system::error_code ec;

		socket_.cancel(ec);
        if (ec) AppContext::instance().logger->warn("[close_session] socket cancel error: {}", ec.message());
        socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        if (ec) AppContext::instance().logger->error("[close_session] tcp shutdown error: {}", ec.message());
        socket_.close(ec);
        if (ec) AppContext::instance().logger->error("[close_session] tcp close error: {}", ec.message());

        finalize_close();
        // DataHandler가 존/풀 제거를 책임짐
        if (auto handler = data_handler_.lock()) {
            handler->remove_session(session_id_);
        } else {
            AppContext::instance().logger->warn("[close_session] DataHandler expired while closing session {}", session_id_);
        }

        cleanup();  // 내부 상태 초기화
    }
    catch (const std::exception& e) {
        AppContext::instance().logger->error("[close_session] Exception: {}", e.what());

        // 예외가 나더라도 최소한의 정리(catch-safe cleanup)는 수행
        try {
            cleanup();
        }
        catch (const std::exception& ce) {
            AppContext::instance().logger->critical("[close_session] cleanup() failed inside catch: {}", ce.what());
        }
    }

}

// Lifecycle 관리 메서드
//bool Session::try_activate() {
//    uint8_t expected = static_cast<uint8_t>(LifeState::Idle);
//    return life_state_.compare_exchange_strong(expected, static_cast<uint8_t>(LifeState::Active));
//}

//bool Session::try_close() {
//    uint8_t expected = static_cast<uint8_t>(LifeState::Active);
//    if (life_state_.compare_exchange_strong(expected, static_cast<uint8_t>(LifeState::Closing)))
//        return true;
//    expected = static_cast<uint8_t>(LifeState::Idle);
//    if (life_state_.compare_exchange_strong(expected, static_cast<uint8_t>(LifeState::Closing)))
//        return true;
//    return false;
//}

//void Session::finalize_close() {
//    life_state_.store(static_cast<uint8_t>(LifeState::Closed), std::memory_order_release);
//}

//void Session::mark_released() {
//    life_state_.store(static_cast<uint8_t>(LifeState::Released), std::memory_order_release);
//}

//Session::LifeState Session::get_life_state() const {
//    return static_cast<LifeState>(life_state_.load(std::memory_order_acquire));
//}

//void Session::set_active(bool v) {
//    if (v) try_activate();
//    else {
//        uint8_t expected = static_cast<uint8_t>(LifeState::Active);
//        life_state_.compare_exchange_strong(expected, static_cast<uint8_t>(LifeState::Idle));
//    }
//}

// Lifecycle 관리 메서드
bool Session::try_activate() {      
    uint8_t expected = static_cast<uint8_t>(LifeState::Idle);
    return life_state_.compare_exchange_strong(expected, static_cast<uint8_t>(LifeState::Active));
}

bool Session::try_close() {
    uint8_t expected = static_cast<uint8_t>(LifeState::Active);
    if (life_state_.compare_exchange_strong(expected, static_cast<uint8_t>(LifeState::Closing)))
        return true;
    expected = static_cast<uint8_t>(LifeState::Idle);
    if (life_state_.compare_exchange_strong(expected, static_cast<uint8_t>(LifeState::Closing)))
        return true;
    return false;
}

void Session::finalize_close() {
    life_state_.store(static_cast<uint8_t>(LifeState::Closed), std::memory_order_release);
}

void Session::mark_released() {
    life_state_.store(static_cast<uint8_t>(LifeState::Released), std::memory_order_release);
}

Session::LifeState Session::get_life_state() const {
    return static_cast<LifeState>(life_state_.load(std::memory_order_acquire));
}

void Session::set_active(bool v) {
    if (v) try_activate();
    else {
        uint8_t expected = static_cast<uint8_t>(LifeState::Active);
        life_state_.compare_exchange_strong(expected, static_cast<uint8_t>(LifeState::Idle));
    }
}

bool Session::is_active() const {
    return get_life_state() == LifeState::Active;
}

void Session::cleanup() {
    //내부 상태 초기화 (재사용 준비)
    nickname_.clear();
    line_buffer_.clear();
    message_.clear();
    memset(data_, 0, sizeof(data_));

    // UDP 관련 초기화 (엔드포인트는 DataHandler가 remove_session 시점에 이미 제거된 상태여야 함)
    udp_endpoint_.reset();
    udp_ep_update_time_ = std::chrono::steady_clock::time_point{};
    last_udp_alive_time_.store(std::chrono::steady_clock::time_point{});
    udp_token_.clear();

    // UDP flood counters
    udp_packet_count_ = 0;
    udp_packet_window_ = std::chrono::steady_clock::time_point{};

    // 존 관련: 내부 필드만 초기화(존 오브젝트에서의 제거는 DataHandler/ZoneManager 책임)

    zone_.reset();
    zone_id_ = -1;

    // task / write 큐 정리 (비동기 작업이 남아있으면 문제가 될 수 있으므로 안전하게 비운다.
    {
        while (!task_queue_.empty()) task_queue_.pop();
        while (!write_queue_.empty()) write_queue_.pop();
    }

    // 타이머 취소
    boost::system::error_code ec;
    login_timer_.cancel();
    retry_timer_.cancel();

    // 세대 증가(이전 async 콜백들이 더이상 유효하지 않음을 보장)
    generation_.fetch_add(1, std::memory_order_relaxed);

    // 활성/closed 플래그 재설정
    closed_.store(false);

    read_pending_.store(false);

    task_running_ = false;

    nickname_registered_.store(false);

    //active_.store(false);
    //AppContext::instance().logger->info("[Session::cleanup] session_id={} internal state cleared (no zone/pool ops)", session_id_);
    life_state_.store(static_cast<uint8_t>(LifeState::Idle), std::memory_order_release);
    AppContext::instance().logger->info("[Session::cleanup] session_id={} 내부 상태 초기화 완료 (life=Idle)", session_id_);
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

    auto handler = get_handler_safe();
    if (!handler) return; // 이미 close_session() 호출됨

    // do_read를 직접 호출하지 않고, post_task로 감싼다.
    post_task([this, self, handler]() {
        auto& strand = get_strand();
        get_socket().async_read_some(
            buffer(get_data(), sizeof(data_)),
            boost::asio::bind_executor(strand, [this, self, handler](const boost::system::error_code& ec, size_t length) {
            // boost::asio::bind_executor는 이 함수에 strand라는 꼬리표를 붙여서 새로운 함수로 만들어 주세요.나중에 이 새 함수가 호출되면 그 때 strand에서 실행되게 된다. (실행 규칙을 가진 함수 생성)
                // [2] 콜백 진입 시 반드시 해제!
                release_read();

                try {
                    if (!ec) {
                        // 1. 누적 버퍼에 append
                        get_msg_buffer().append(get_data(), length);

                        // 2. 여러 메시지 추출 및 처리
                        while (auto opt_msg = get_msg_buffer().extract_message()) {
                            try {
                                //json msg = json::parse(*opt_msg);
                                std::string msg_copy = *opt_msg;   // 안전하게 복사
                                json msg = json::parse(msg_copy);
                                handler->dispatch(self, msg);
                            }
                            catch (const nlohmann::json::parse_error& e) {
                                cerr << "[JSON parsing error_0] " << e.what() << " / data: " << *opt_msg << endl;
                                AppContext::instance().logger->warn(
                                    "[Session_0][JSON] 파싱 오류: {} / data: {}", e.what(), *opt_msg);
                                // 클라이언트에 오류 메시지 전송
                                nlohmann::json err = { {"type", "error"}, {"msg", "Invalid JSON format"} };
                                post_write(err.dump() + "\n");
                                continue;
                            }
                            catch (const std::exception& e) {
                                cerr << "[JSON parsing error_1] " << e.what() << " / data: " << *opt_msg << endl;
                                AppContext::instance().logger->error(
                                    "[Session_1][dispatch 예외] {} / data: {}", e.what(), *opt_msg);
                                // dispatch 내부 오류는 세션 종료로 처리
                                close_session();
                                return;
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
                        handler->broadcast(notice.dump() + "\n", get_session_id(), self);

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
                            handler->broadcast(notice.dump() + "\n", get_session_id(), self);

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
                    AppContext::instance().logger->critical("[Session][FATAL] do_read 내부 예외: {}", e.what());
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

void Session::set_udp_token(const std::string& token) {
    udp_token_ = token;
    udp_token_issued_ = std::chrono::steady_clock::now();
    udp_token_ttl_sec_ = AppContext::instance().config.value("udp_token_ttl_seconds", 300);
}