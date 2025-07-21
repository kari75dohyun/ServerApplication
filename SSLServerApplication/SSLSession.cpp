#include <iostream>  
#include "SSLSession.h"  
#include "DataHandler.h"  
#include <boost/asio.hpp>
#include "Logger.h"
#include <random>
#include <sstream>
#include "Utility.h"
#include <nlohmann/json.hpp>

using namespace std;
using namespace boost::asio;
using boost::asio::ip::tcp;
using json = nlohmann::json;

SSLSession::SSLSession(tcp::socket socket, ssl::context& context, int session_id, weak_ptr<DataHandler> data_handler)
    : socket_(std::move(socket), context),
    session_id_(session_id),
    data_handler_(data_handler),
    strand_(boost::asio::make_strand(socket_.get_executor())),
    ssl_context_(context),
    login_timer_(strand_),
    retry_timer_(strand_) {
    // Session에서 각자 keepalive 타이머를 관리 하는 방식
    //ping_timer_(socket_.get_executor()),
    //keepalive_timer_(socket_.get_executor()) {
    memset(data_, 0, sizeof(data_));
    // 글로벌 구조에서는 세션 생성시점에 마지막 pong 시간 초기화!
    last_alive_time_ = std::chrono::steady_clock::now();
}

SSLSession::~SSLSession() {
    //cerr << "[세션 소멸] id=" << session_id_ << endl;  
    //LOG_ERROR("[세션 소멸] id=", session_id_);
    g_logger->error("[세션 소멸] id= {}", session_id_);
}

void SSLSession::start() {
    auto self(shared_from_this());
    if (auto handler = data_handler_.lock()) {
        //handler->do_handshake(self);
        do_handshake(0);
        start_login_timeout();    // 타이머 시작 추가!
        //start_ping_sender();      // 서버에서 ping 를 보낸다.
        //start_keepalive_timer();  // 핸드쉐이크 후 keepalive 타이머 시작
    }
    else {
        //cerr << "DataHandler expired!" << endl;
        //LOG_ERROR("DataHandler expired!"); 
        g_logger->error("DataHandler expired!");
    }
}

int SSLSession::get_session_id() const {
    return session_id_;
}

void SSLSession::post_task(std::function<void()> fn) {
    auto self = shared_from_this();
    //std::cout << "[DEBUG] post_task called (session_id=" << session_id_ << ")" << std::endl;
    boost::asio::dispatch(strand_, [this, self, fn = std::move(fn)]() mutable {
        //std::cout << "[DEBUG] task enqueued" << std::endl;
        if (task_queue_.size() >= MAX_TASK_QUEUE) {
            //std::cerr << "[WARN] task_queue_ overflow! (session_id=" << session_id_ << ")\n";
            //LOG_WARN("[WARN] task_queue_ overflow! (session_id=", session_id_);
            g_logger->warn("[WARN] task_queue_ overflow! (session_id= {}", session_id_);
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

void SSLSession::run_next_task() {
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
void SSLSession::post_write(const std::string& msg) {
    post_write(std::make_shared<std::string>(msg));
}

// (2) post_write(shared_ptr 버전, 핵심 로직)
void SSLSession::post_write(const std::shared_ptr<std::string> msg) {
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

void SSLSession::do_write_queue() {
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
                        g_logger->warn("[do_write_queue] Stale write callback (세대 mismatch)! 세션ID={} 무시", get_session_id());
                        return;
                    }

                    if (ec) {
                        g_logger->error("[do_write_queue] error: {}", ec.message());
                        //close_session();
                        close_session();
                        return;
                    }
                    write_queue_.pop();
                    do_write_queue();
                }
                catch (const std::exception& e) {
                    g_logger->error("[do_write_queue][Exception] {}", e.what());
                    //close_session();
                    close_session();
                }
            }
        )
    );
}

void SSLSession::start_login_timeout() {
    if (get_state() == SessionState::Closed) {
        g_logger->warn("Closed session: Callback/Ignore Message [session_id={}]", get_session_id());
        return;
    }
    login_timer_.expires_after(std::chrono::seconds(LOGIN_TIMEOUT_SECONDS)); // 예: 90초
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

void SSLSession::on_nickname_registered() {
    nickname_registered_ = true;
    set_state(SessionState::Ready); // 로그인 성공 상태로!
    login_timer_.cancel(); // 인수 제거하여 타이머 취소  
}

void SSLSession::reset(boost::asio::ip::tcp::socket&& new_socket, int session_id) {
    boost::system::error_code ec;

    // (1) 기존 소켓 비동기 작업 모두 취소 및 안전하게 닫기
    if (socket_.next_layer().is_open()) {
        socket_.next_layer().cancel(ec);
        socket_.next_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        socket_.next_layer().close(ec);
    }

    // (2) 새 소켓 move 할당
    socket_.next_layer() = std::move(new_socket);

    // (3) strand, 타이머 등 재할당 (필수)
    strand_ = boost::asio::make_strand(socket_.get_executor());
    login_timer_ = boost::asio::steady_timer(strand_);

    // (4) 내부 상태 모두 초기화 (★추가/확인★)
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

    set_state(SessionState::Handshaking);   // 새로운 세션 시작시 항상 초기 상태로!

    // UDP 관련 모든 멤버 초기화
    udp_endpoint_.reset();
    udp_ep_update_time_ = std::chrono::steady_clock::time_point{};
    last_udp_alive_time_.store(std::chrono::steady_clock::time_point{}); // 또는 std::chrono::steady_clock::now();
    udp_token_.clear();
    udp_packet_count_ = 0;
    udp_packet_window_ = std::chrono::steady_clock::time_point{};

    // ZONE/세션 풀 관련
    zone_id_ = 0;

    // keepalive/heartbeat
    last_alive_time_ = std::chrono::steady_clock::now();

    nickname_registered_ = false;

    // 버퍼
    msg_buf_mgr_.clear();

    generation_.fetch_add(1, std::memory_order_relaxed); // 세대 증가!

    g_logger->info("[SSLSession][reset] 세션 {} 내부 상태 초기화 완료", session_id_);
}


std::string SSLSession::get_client_ip() const {
    try {
        return socket_.lowest_layer().remote_endpoint().address().to_string();
    }
    catch (const std::exception&) {
        return "unknown";
    }
}

unsigned short SSLSession::get_client_port() const {
    try {
        return socket_.lowest_layer().remote_endpoint().port();
    }
    catch (const std::exception&) {
        return 0;
    }
}

void SSLSession::update_udp_alive_time() {
    last_udp_alive_time_ = std::chrono::steady_clock::now();
}

std::chrono::steady_clock::time_point SSLSession::get_last_udp_alive_time() const {
    return last_udp_alive_time_.load();
}

void SSLSession::enqueue_write(std::shared_ptr<std::string> msg) {
    // 1. 80% 초과 경고만
    if (write_queue_.size() >= kWriteQueueWarnThreshold) {
        g_logger->warn("[SSLSession][enqueue_write] write_queue 임계치(80%) 초과: size={}", write_queue_.size());
    }

    // 2. FULL(100%)이면 가장 오래된 것 drop, 연속이면 close
    if (write_queue_.size() >= kMaxWriteQueueSize) {
        g_logger->warn("[SSLSession][enqueue_write] write_queue FULL! 가장 오래된 메시지 drop, 새 메시지 push");
        write_queue_.pop();

        // 연속 FULL 카운트 증가
        ++write_queue_overflow_count_;
        if (write_queue_overflow_count_ >= kWriteQueueOverflowLimit) {
            g_logger->error("[SSLSession][enqueue_write] write_queue FULL 연속 {}회, 세션 종료!", write_queue_overflow_count_);
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

void SSLSession::close_session() {
    if (closed_.exchange(true)) return;
    set_state(SessionState::Closed);    // 세션 상태 종료로!

    auto self = shared_from_this();
    try {
        // ⭐️ SSL 종료 핸드쉐이크 (async_shutdown) 먼저 호출
        socket_.async_shutdown(
            boost::asio::bind_executor(strand_,
                [this, self](const boost::system::error_code& ec) {
                    if (ec) {
                        //std::cerr << "[close_session] async_shutdown` error: " << ec.message() << std::endl;
                        //LOG_ERROR("[close_session] async_shutdown error: ", ec.message());
                        g_logger->error("[close_session] async_shutdown error: {}", ec.message());
                    }

                    // TCP 소켓 안전하게 닫기
                    boost::system::error_code ec2;
                    socket_.lowest_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec2);
                    if (ec2) g_logger->error("[close_session] tcp shutdown error: {}", ec2.message()); //LOG_ERROR("[close_session] tcp shutdown error: ", ec2.message());  //std::cerr << "[close_session] tcp shutdown error: " << ec2.message() << std::endl;
                    socket_.lowest_layer().close(ec2);
                    if (ec2) g_logger->error("[close_session] tcp close error: {}", ec2.message()); //LOG_ERROR("[close_session] tcp close error: ", ec2.message());//std::cerr << "[close_session] tcp close error: " << ec2.message() << std::endl;

                    // ⭐️ remove_session을 마지막에 호출 (release 등 포함)
                    if (auto handler = data_handler_.lock()) {
                        handler->remove_session(session_id_);
                    }
                }
            )
        );
    }
    catch (const std::exception& e) {
        //std::cerr << "[close_session] Exception: " << e.what() << std::endl;
        //LOG_ERROR("[close_session] Exception: ", e.what());
        g_logger->error("[close_session] Exception: {}", e.what());
    }
}

void SSLSession::do_handshake(int retry_count)
{
    auto self = shared_from_this();
    socket_.async_handshake(boost::asio::ssl::stream_base::server,
        boost::asio::bind_executor(strand_,
            [this, self, retry_count](const boost::system::error_code& ec) {
                if (!ec) {
                    // 핸드쉐이크 성공 -> 정상처리
                    set_state(SessionState::LoginWait);
                    set_message(R"({"type":"notice","msg":"Enter your nickname:"})" "\n");
                    //do_write(self);
                    post_write(self->get_message());
                    do_read(); //반드시 필요!
                }
                else if (retry_count < kMaxCloseRetries) {
                    g_logger->warn("[handshake][retry] failed: {}, retry_count={}", ec.message(), retry_count + 1);
                    retry_timer_.expires_after(std::chrono::milliseconds(kRetryDelayMs));
                    retry_timer_.async_wait(boost::asio::bind_executor(strand_,
                        [this, self, retry_count](const boost::system::error_code& timer_ec) {
                            if (!timer_ec) do_handshake(retry_count + 1);
                        }
                    ));
                }
                else {
                    g_logger->error("[handshake][retry] 3회 재시도 실패, 세션 종료");
                    close_session();
                }
            }
        )
    );
}

void SSLSession::do_read() 
{
    auto self = shared_from_this();
    // [1] 중복 read 방지!
    if (get_state() == SessionState::Closed) {
        g_logger->warn("Closed session: 콜백/메시지 무시 [session_id={}]", get_session_id());
        return;
    }
    if (!try_acquire_read()) {
        //cerr << "[WARN] 중복 do_read 감지! session_id=" << session->get_session_id() << endl;
        g_logger->info("[WARN] 중복 do_read 감지! session_id= {}", get_session_id());
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
                                //cout << "[DEBUG] dispatching message" << endl;
                                json msg = json::parse(*opt_msg);
                                if (auto handler = data_handler_.lock()) {
                                    handler->dispatch(self, msg);  // 바로 이렇게!
                                }
                                //dispatcher_.dispatch(self, msg);
                            }
                            catch (const exception& e) {
                                cerr << "[JSON parsing error] " << e.what() << " / data: " << *opt_msg << endl;
                                set_message(R"({"type":"error","msg":"Message parsing failed"})" "\n");
                                //do_write(self);
                                post_write(get_message());
                                // 에러 시에도 계속 다음 메시지 분리/처리
                            }
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
                            g_logger->warn("[Connection reset] Client disconnected without a nickname. {}", self->get_session_id());
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