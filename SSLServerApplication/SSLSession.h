#pragma once

#include "DataHandler.h"
#include "MessageBufferManager.h"
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/strand.hpp>
#include <memory>
#include <string>
#include <queue>

class DataHandler;  // 전방 선언: DataHandler 클래스

// SSL 세션을 관리하는 클래스
class SSLSession : public std::enable_shared_from_this<SSLSession> {
private:
    boost::asio::ssl::stream<boost::asio::ip::tcp::socket> socket_;  // SSL 스트림을 사용한 소켓
    boost::asio::strand<boost::asio::any_io_executor> strand_;  // 수정된 strand_ 타입
    char data_[2048];  // 데이터를 읽을 버퍼
    std::string message_;  // 서버에서 보낼 메시지를 저장하는 변수 
    int session_id_;  // 세션을 식별하기 위한 세션 ID
    std::string nickname_;
    std::string line_buffer_; // 세션 멤버에 추가
    std::weak_ptr<DataHandler> data_handler_;  // DataHandler 포인터

    std::atomic<bool> read_pending_{ false };   // 추가

    // ---- 직렬화 큐 관련 추가 ----
    std::queue<std::function<void()>> task_queue_;
    bool task_running_ = false;

    MessageBufferManager msg_buf_mgr_; // 누적 버퍼

    std::queue<std::string> write_queue_;    // write 메시지 큐
    bool write_in_progress_ = false;         // 현재 write 중인지

    static constexpr size_t MAX_WRITE_QUEUE = 1000;
    static constexpr size_t MAX_TASK_QUEUE = 1000;

public:
    // 생성자: 클라이언트 소켓과 SSL 컨텍스트를 받아 SSL 스트림을 초기화
    SSLSession(boost::asio::ip::tcp::socket socket, boost::asio::ssl::context& context, int session_id, std::weak_ptr<DataHandler> data_handler);
    ~SSLSession();

    // 세션 시작: 핸드쉐이크 수행
    void start();

    // 세션 ID 가져오기
    int get_session_id() const;

    // 메시지 큐 직렬화 관련
    void post_task(std::function<void()> fn);
    void run_next_task();

    // Getter for message_  
    const std::string& get_message() const { return message_; }

    // Setter for message_  
    void set_message(const std::string& message) { message_ = message; }

    // Getter for socket_  
    boost::asio::ssl::stream<boost::asio::ip::tcp::socket>& get_socket() { return socket_; }

    boost::asio::strand<boost::asio::any_io_executor>& get_strand() { return strand_; }  // 수정된 반환 타입

	char* get_data() { return data_; }              // 읽기 전용 버퍼를 반환하는 함수
	const char* get_data() const { return data_; }  // 읽기 전용 버퍼를 반환하는 함수 (const 버전)

	// nickname 설정 및 가져오기
    void set_nickname(const std::string& n) { nickname_ = n; }
    std::string get_nickname() const { return nickname_; }
	// 라인 버퍼를 가져오는 함수
    std::string& get_line_buffer() { return line_buffer_; }
    // 클라이언트 연결 종료 시 세션 종료
    void close_session();

    // 중복 do_read 체크 함수
    bool try_acquire_read() {
        return !read_pending_.exchange(true);
    }
    // do_read 콜백에서 호출 (pending 해제)
    void release_read() {
        read_pending_ = false;
    }
    // 필요하다면 getter도 추가 가능
    bool is_read_pending() const { return read_pending_.load(); }

    // Getter for recv_buffer_  
    MessageBufferManager& get_msg_buffer() { return msg_buf_mgr_; }
	// write 메시지 큐 관련 함수 (직렬화)
    void post_write(const std::string& msg);

private:
    void do_write_queue();

};
