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
      strand_(boost::asio::make_strand(socket_.get_executor())) { // Ensure make_strand is used correctly  
    memset(data_, 0, sizeof(data_));
}

SSLSession::~SSLSession() {  
    cerr << "[세션 소멸] id=" << session_id_ << endl;  
}

void SSLSession::start() {  
    auto self(shared_from_this());  
    if (auto handler = data_handler_.lock()) {  
        handler->do_handshake(self);  
    } else {  
        cerr << "DataHandler expired!" << endl;  
    }  
}  

int SSLSession::get_session_id() const {  
    return session_id_;  
}  

void SSLSession::post_task(std::function<void()> fn) {
    auto self = shared_from_this();
    boost::asio::dispatch(strand_, [this, self, fn = std::move(fn)]() mutable {
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
    if (task_queue_.empty()) {
        task_running_ = false;
        return;
    }
    auto fn = std::move(task_queue_.front());
    task_queue_.pop();
    fn();  // 비동기 작업 진입, 콜백 마지막에 run_next_task() 호출!
}

void SSLSession::close_session() {
    try {
        // 1. 소켓 종료
        boost::system::error_code ec;
        socket_.lowest_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        socket_.lowest_layer().close(ec);

        // 2. 세션 삭제 요청 (단, DataHandler가 살아 있을 때만)
        if (auto handler = data_handler_.lock()) {
            handler->remove_session(session_id_);
        }
    }
    catch (const std::exception& e) {
        std::cerr << "[close_session] Exception: " << e.what() << std::endl;
    }
}

void SSLSession::post_write(const std::string& msg) {
    auto self = shared_from_this();
    boost::asio::dispatch(strand_, [this, self, msg]() {
        if (write_queue_.size() >= MAX_WRITE_QUEUE) {
            std::cerr << "[WARN] write_queue_ overflow! (session_id=" << session_id_ << ")\n";
            // 에러 메시지 유저에게도 발송 후 종료
            post_write(R"({"type":"error","msg":"Too many pending messages, closing."})" "\n");
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


void SSLSession::do_write_queue() {
    auto self = shared_from_this();
    if (write_queue_.empty()) {
        write_in_progress_ = false;
        return;
    }
    const std::string& msg = write_queue_.front();
    boost::asio::async_write(socket_, boost::asio::buffer(msg),
        boost::asio::bind_executor(strand_,
            [this, self](const boost::system::error_code& ec, std::size_t /*length*/) {
                if (!ec) {
                    write_queue_.pop();
                    do_write_queue(); // 다음 메시지 있으면 계속 write
                }
                else {
                    write_in_progress_ = false;
                    close_session(); // 에러시 안전 종료
                }
            }
        )
    );
}
