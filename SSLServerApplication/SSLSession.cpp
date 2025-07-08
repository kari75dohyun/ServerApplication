#include <iostream>  
#include "SSLSession.h"  
#include "DataHandler.h"  
#include <boost/asio.hpp>  

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
    cerr << "[세션 소멸] id=" << session_id_ << endl;  
}

void SSLSession::start() {  
    auto self(shared_from_this());  
    if (auto handler = data_handler_.lock()) {  
        handler->do_handshake(self);
        start_login_timeout();    // 타이머 시작 추가!
        //start_ping_sender();      // 서버에서 ping 를 보낸다.
        //start_keepalive_timer();  // 핸드쉐이크 후 keepalive 타이머 시작
    } else {  
        cerr << "DataHandler expired!" << endl;  
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
            std::cerr << "[WARN] task_queue_ overflow! (session_id=" << session_id_ << ")\n";
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

    auto self = shared_from_this();
    try {
        // ⭐️ SSL 종료 핸드쉐이크 (async_shutdown) 먼저 호출
        socket_.async_shutdown(
            boost::asio::bind_executor(strand_,
                [this, self](const boost::system::error_code& ec) {
                    if (ec) {
                        std::cerr << "[close_session] async_shutdown error: " << ec.message() << std::endl;
                    }

                    // TCP 소켓 안전하게 닫기
                    boost::system::error_code ec2;
                    socket_.lowest_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec2);
                    if (ec2) std::cerr << "[close_session] tcp shutdown error: " << ec2.message() << std::endl;
                    socket_.lowest_layer().close(ec2);
                    if (ec2) std::cerr << "[close_session] tcp close error: " << ec2.message() << std::endl;

                    // ⭐️ remove_session을 마지막에 호출 (release 등 포함)
                    if (auto handler = data_handler_.lock()) {
                        handler->remove_session(session_id_);
                    }
                }
            )
        );
    }
    catch (const std::exception& e) {
        std::cerr << "[close_session] Exception: " << e.what() << std::endl;
    }
}


//void SSLSession::close_session() {
//    if (closed_.exchange(true)) return;
//
//    try {
//        // 타이머 취소
//        login_timer_.cancel();      // 로그인 타이머 취소
//        //ping_timer_.cancel();       // ping 타이머 취소
//        //keepalive_timer_.cancel();  // Keepalive 타이머 취소
//
//        // 1. 세션 삭제 (release도 내부에서 처리)
//        if (auto handler = data_handler_.lock()) {
//            handler->remove_session(session_id_);
//        }
//        else {
//            std::cerr << "[close_session] DataHandler expired!" << std::endl;
//        }
//
//        // 2. 소켓 안전 종료
//        boost::system::error_code ec;
//        socket_.lowest_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
//        if (ec) {
//            std::cerr << "[close_session] shutdown error: " << ec.message() << std::endl;
//        }
//        socket_.lowest_layer().close(ec);
//        if (ec) {
//            std::cerr << "[close_session] close error: " << ec.message() << std::endl;
//        }
//
//        // 3. (더 이상 remove_session, release 등 중복 호출하지 않음!)
//    }
//    catch (const std::exception& e) {
//        std::cerr << "[close_session] Exception: " << e.what() << std::endl;
//    }
//}

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

//void SSLSession::do_write_queue() {
//    auto self = shared_from_this();
//    if (write_queue_.empty()) {
//        write_in_progress_ = false;
//        return;
//    }
//    const std::string& msg = write_queue_.front();
//    boost::asio::async_write(socket_, boost::asio::buffer(msg),
//        boost::asio::bind_executor(strand_,
//            [this, self](const boost::system::error_code& ec, std::size_t /*length*/) {
//                if (!ec) {
//                    write_queue_.pop();
//                    do_write_queue(); // 다음 메시지 있으면 계속 write
//                }
//                else {
//                    write_in_progress_ = false;
//                    close_session(); // 에러시 안전 종료
//                }
//            }
//        )
//    );
//}

void SSLSession::start_login_timeout() {
    login_timer_.expires_after(std::chrono::seconds(90)); // 예: 90초
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
    // (5) 타이머, 버퍼, 큐 등 모두 초기화 (필요시)
    // ...
}


//void SSLSession::reset(boost::asio::ip::tcp::socket&& socket, int session_id) {
//    //std::cout << "[reset] this = " << this
//    //    << ", session_id = " << session_id
//    //    << ", socket.is_open() = " << socket.is_open()
//    //    << std::endl;
//
//    socket_ = boost::asio::ssl::stream<boost::asio::ip::tcp::socket>(
//        std::move(socket),
//        ssl_context_
//    );
//
//    // socket_ 할당 직후 내부 상태
//    //std::cout << "[reset] socket_ next_layer().is_open() = "
//    //    << socket_.next_layer().is_open() << std::endl;
//
//    // executor 상태 체크
//    auto exec = socket_.get_executor();
//    //std::cout << "[reset] socket_.get_executor() addr = " << &exec << std::endl;
//
//    // strand_ 할당 전후
//    //std::cout << "[reset] strand_ 할당 전" << std::endl;
//    strand_ = boost::asio::make_strand(socket_.get_executor());
//    //std::cout << "[reset] strand_ 할당 후, &strand_ = " << &strand_ << std::endl;
//
//    // 이하 동일
//    session_id_ = session_id;
//    nickname_.clear();
//    line_buffer_.clear();
//    message_.clear();
//    memset(data_, 0, sizeof(data_));
//
//    std::queue<std::function<void()>> empty;
//    std::swap(task_queue_, empty);
//    task_running_ = false;
//    read_pending_ = false;
//
//    //std::cout << "[SSLSession reset] strand addr: " << &strand_ << std::endl;
//}

//void SSLSession::reset(boost::asio::ip::tcp::socket&& socket, int session_id) {
//    socket_ = boost::asio::ssl::stream<boost::asio::ip::tcp::socket>(
//        std::move(socket),
//        ssl_context_
//    );
//    // 이하 동일!
//    session_id_ = session_id;
//    nickname_.clear();
//    line_buffer_.clear();
//    message_.clear();
//    memset(data_, 0, sizeof(data_));
//
//    std::queue<std::function<void()>> empty;
//    std::swap(task_queue_, empty);
//    task_running_ = false;
//    read_pending_ = false;
//    // 기타 필요 상태 초기화
//}
