#include <iostream>  
#include "SSLSession.h"  
#include "DataHandler.h"  
#include <boost/asio.hpp>
#include "Logger.h"

using namespace std;
using namespace boost::asio;
using boost::asio::ip::tcp;

SSLSession::SSLSession(tcp::socket socket, ssl::context& context, int session_id, weak_ptr<DataHandler> data_handler)
    : socket_(std::move(socket), context),
    session_id_(session_id),
    data_handler_(data_handler),
    strand_(boost::asio::make_strand(socket_.get_executor())),
    ssl_context_(context),
    login_timer_(strand_) {
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
        handler->do_handshake(self);
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

void SSLSession::close_session() {
    if (closed_.exchange(true)) return;
    set_state(SessionState::Closed);    // 세션 상태 종료로!

    auto self = shared_from_this();
    try {
        // ⭐️ SSL 종료 핸드쉐이크 (async_shutdown) 먼저 호출
        socket_.async_shutdown(
            boost::asio::bind_executor(strand_,
                [this, self](const boost::system::error_code& ec) {
                    if (ec && ec != boost::asio::error::eof) {
                        //std::cerr << "[close_session] async_shutdown error: " << ec.message() << std::endl;
                        //LOG_ERROR("[close_session] async_shutdown error: ", ec.message());
                        g_logger->error("[close_session] async_shutdown error: {}", ec.message());
                    }
                    // TCP 소켓 안전하게 닫기
                    boost::system::error_code ec2;
                    socket_.lowest_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec2);
                    if (ec2) {
                        g_logger->error("[close_session] tcp shutdown error: {}", ec2.message()); //LOG_ERROR("[close_session] tcp shutdown error: ", ec2.message());  //std::cerr << "[close_session] tcp shutdown error: " << ec2.message() << std::endl;
                    }
                    socket_.lowest_layer().close(ec2);
                    if (ec2) {
                        g_logger->error("[close_session] tcp close error: {}", ec2.message()); //LOG_ERROR("[close_session] tcp close error: ", ec2.message());//std::cerr << "[close_session] tcp close error: " << ec2.message() << std::endl;
                    }

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

// (1) post_write(기존 string용 → shared_ptr 버전으로 변환해서 호출)
void SSLSession::post_write(const std::string& msg) {
    post_write(std::make_shared<std::string>(msg));
}
// (2) post_write(shared_ptr 버전, 핵심 로직)
void SSLSession::post_write(std::shared_ptr<std::string> msg) {
    auto self = shared_from_this();
    boost::asio::dispatch(strand_, [this, self, msg]() {
        if (write_queue_.size() >= MAX_WRITE_QUEUE) {
            std::cerr << "[WARN] write_queue_ overflow! (session_id=" << session_id_ << ")\n";
            close_session();
            return;
        }
        bool idle = write_queue_.empty();
        write_queue_.push(msg);
        if (idle) {
            write_in_progress_ = true;
            do_write_queue();
        }
        });
}
// (3) do_write_queue (shared_ptr<std::string> 사용)
void SSLSession::do_write_queue() {
    if (get_state() == SessionState::Closed) {
        g_logger->warn("Closed session: 콜백/메시지 무시 [session_id={}]", get_session_id());
        return;
    }
    auto self = shared_from_this();
    if (write_queue_.empty()) {
        write_in_progress_ = false;
        return;
    }
    auto msg = write_queue_.front(); // shared_ptr<std::string>
    boost::asio::async_write(socket_, boost::asio::buffer(*msg),
        boost::asio::bind_executor(strand_,
            [this, self](const boost::system::error_code& ec, std::size_t /*length*/) {
                if (!ec) {
                    write_queue_.pop();
                    do_write_queue(); // 다음 메시지 있으면 계속 write
                }
                else {
                    write_in_progress_ = false;
                    close_session();
                }
            }
        )
    );
}

void SSLSession::start_login_timeout() {
    if (get_state() == SessionState::Closed) {
        g_logger->warn("Closed session: 콜백/메시지 무시 [session_id={}]", get_session_id());
        return;
    }
    login_timer_.expires_after(std::chrono::seconds(LOGIN_TIMEOUT_SECONDS)); // 예: 90초
    auto self = shared_from_this();
    login_timer_.async_wait([this, self](const boost::system::error_code& ec) {
        if (!ec && !nickname_registered_) {
            std::cerr << "[LOGIN TIMEOUT] session_id=" << session_id_ << " 로그인 시간 초과, 세션 종료!" << std::endl;
            post_write(R"({"type":"notice","msg":"로그인 시간이 초과되어 연결이 종료됩니다."})" "\n");
            close_session();
        }
        });
}

void SSLSession::on_nickname_registered() {
    nickname_registered_ = true;
    set_state(SessionState::Ready); // 로그인 성공 상태로!
    login_timer_.cancel(); // 인수 제거하여 타이머 취소  
}

//void SSLSession::start_ping_sender() {
//    if (closed_) return;
//    ping_timer_.expires_after(ping_interval_);
//    auto self = shared_from_this();
//    ping_timer_.async_wait([this, self](const boost::system::error_code& ec) {
//        if (!ec && !closed_) {
//            std::cout << "[PING] session_id=" << session_id_ << " → 클라이언트로 ping 전송" << std::endl;
//            post_write(R"({"type":"ping"})" "\n"); // 클라로 ping 전송
//            start_ping_sender();
//        }
//        });
//}
//
//void SSLSession::start_keepalive_timer() {
//    if (closed_) return;
//    keepalive_timer_.expires_after(keepalive_timeout_);
//    auto self = shared_from_this();
//    keepalive_timer_.async_wait([this, self](const boost::system::error_code& ec) {
//        if (!ec && !closed_) {
//            std::cout << "[KEEPALIVE TIMEOUT] session_id=" << session_id_
//                << " → 일정 시간 pong 없음, 세션 종료" << std::endl;
//            close_session();
//        }
//        });
//}
//
//// 클라에서 pong 받으면 keepalive 타이머 리셋!
//void SSLSession::on_pong_received() {
//    if (closed_) return;
//    keepalive_timer_.expires_after(keepalive_timeout_);
//    // latency 측정 등 부가 기능도 구현 가능
//}

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

    // (3) strand 등 재할당
    strand_ = boost::asio::make_strand(socket_.get_executor());
    login_timer_ = boost::asio::steady_timer(strand_);  // 꼭 같이 맞춰줘야 안전!

    // (4) 내부 상태 모두 초기화
    session_id_ = session_id;
    nickname_.clear();
    line_buffer_.clear();
    message_.clear();
    memset(data_, 0, sizeof(data_));
    closed_ = false;
    read_pending_ = false;
    set_state(SessionState::Handshaking);   // 새로운 세션 시작시 항상 초기 상태로!
    // (5) 타이머, 버퍼, 큐 등 모두 초기화 (필요시)
    // ...
}

std::string SSLSession::get_client_ip() const {
    try {
        return socket_.lowest_layer().remote_endpoint().address().to_string();
    }
    catch (const std::exception& e) {
        return "unknown";
    }
}

unsigned short SSLSession::get_client_port() const {
    try {
        return socket_.lowest_layer().remote_endpoint().port();
    }
    catch (const std::exception& e) {
        return 0;
    }
}
