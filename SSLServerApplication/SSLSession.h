#pragma once
#include "DataHandler.h"
#include "MessageBufferManager.h"
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/steady_timer.hpp>
#include <memory>
#include <string>
#include <queue>
#include <optional>

class DataHandler;  // 전방 선언: DataHandler 클래스

enum class SessionState { Handshaking, Handshaked, LoginWait, Ready, Closed };

const int kMaxCloseRetries = 3;   // 재시도 횟수
const int kRetryDelayMs = 100;  // 재시도 간격(ms)

// SSL 세션을 관리하는 클래스
class SSLSession : public std::enable_shared_from_this<SSLSession> {
private:
    static constexpr size_t MAX_WRITE_QUEUE = 1000;
    static constexpr size_t MAX_TASK_QUEUE = 1000;
    static constexpr int LOGIN_TIMEOUT_SECONDS = 90;  // 로그인 제한(초)

    size_t write_queue_overflow_count_ = 0;          // 연속 overflow count

    static constexpr size_t kMaxWriteQueueSize = 100;            // 임계치2: queue 최대 길이
    static constexpr size_t kWriteQueueWarnThreshold = 80;       // 임계치1: 경고 임계(80%)
    static constexpr size_t kWriteQueueOverflowLimit = 10;       // 임계치3: 연속 FULL 세션 종료 한계

    boost::asio::ssl::stream<boost::asio::ip::tcp::socket> socket_;  // SSL 스트림을 사용한 소켓
    boost::asio::strand<boost::asio::any_io_executor> strand_;       // 수정된 strand_ 타입
    char data_[2048];                                                // 데이터를 읽을 버퍼
    std::string message_;                                            // 서버에서 보낼 메시지를 저장하는 변수 
    int session_id_;                                                 // 세션을 식별하기 위한 세션 ID
    std::string nickname_;
    std::string line_buffer_;                                        // 세션 멤버에 추가
    std::weak_ptr<DataHandler> data_handler_;                        // DataHandler 포인터
    std::atomic<bool> read_pending_{ false };
    std::queue<std::function<void()>> task_queue_;                   // 직렬화 큐 관련 추가
    bool task_running_ = false;

    MessageBufferManager msg_buf_mgr_;                               // 누적 버퍼
    //std::queue<std::string> write_queue_;                          // write 메시지 큐
    std::queue<std::shared_ptr<std::string>> write_queue_;
    bool write_in_progress_ = false;                                 // 현재 write 중인지
    std::atomic<bool> closed_{ false };                              // 중복 종료 방지 플래그 추가
    boost::asio::ssl::context& ssl_context_;

    boost::asio::steady_timer login_timer_;                          // 닉네임 입력 타이머
    std::atomic<bool> nickname_registered_{ false };                 // 닉네임 입력 상태 플래그

    // keepalive 관련
    //boost::asio::steady_timer ping_timer_;
    //boost::asio::steady_timer keepalive_timer_;
    //std::chrono::seconds ping_interval_{ 20 };
    //std::chrono::seconds keepalive_timeout_{ 60 };

    //void start_ping_sender();
    //void start_keepalive_timer();

    // 글로벌 keepalive 관련 => 클라 heartbeat 구조로 변경
    std::atomic<std::chrono::steady_clock::time_point> last_alive_time_{};
    SessionState state_ = SessionState::Handshaking;                 // 초기값 Handshaking
	// UDP 엔드포인트
    std::optional<boost::asio::ip::udp::endpoint> udp_endpoint_;
    std::chrono::steady_clock::time_point udp_ep_update_time_{};
    //UDP 엔드포인트 갱신 및 만료
    std::atomic<std::chrono::steady_clock::time_point> last_udp_alive_time_{};

    std::string udp_token_; // UDP 임시 토큰
	// UDP Flood 방지 관련
    std::atomic<size_t> udp_packet_count_{ 0 };
    std::chrono::steady_clock::time_point udp_packet_window_{};

    int zone_id_ = 0; // 기본 0 = 미배정

    std::atomic<uint64_t> generation_{ 0 };   // generation: reset시 증가
    std::atomic<bool> active_{ false };   // 활성 세션 여부

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
    void set_message(std::string&& message) { message_ = std::move(message); }

    // Getter for socket_  
    boost::asio::ssl::stream<boost::asio::ip::tcp::socket>& get_socket() { return socket_; }

    boost::asio::strand<boost::asio::any_io_executor>& get_strand() { return strand_; }  // 수정된 반환 타입

    char* get_data() { return data_; }              // 읽기 전용 버퍼를 반환하는 함수
    const char* get_data() const { return data_; }  // 읽기 전용 버퍼를 반환하는 함수 (const 버전)

    // nickname 설정 및 가져오기
    void set_nickname(const std::string& n) { nickname_ = n; }
    void set_nickname(std::string&& n) { nickname_ = std::move(n); }

    std::string get_nickname() const { return nickname_; }
    // 라인 버퍼를 가져오는 함수
    std::string& get_line_buffer() { return line_buffer_; }
    // 클라이언트 연결 종료 시 세션 종료
    //void close_session();

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
    void post_write(std::shared_ptr<std::string> msg); // 새 버전

    // Session 재설정 함수
    void reset(boost::asio::ip::tcp::socket&& socket, int session_id);

    void start_login_timeout();            // 닉네임 타이머 시작
    void on_nickname_registered();         // 닉네임 등록 완료 콜백
    bool is_nickname_registered() const { return nickname_registered_.load(); }

    // keepalive 관련 추가
    //void on_pong_received();

    // 글로벌 keepalive 관련
    void update_alive_time() {
        last_alive_time_ = std::chrono::steady_clock::now();
    }
    std::chrono::steady_clock::time_point get_last_alive_time() const {
        return last_alive_time_.load();
    }

    bool is_closed() const { return closed_.load(); }

    // 상태 가져오기
    SessionState get_state() const { return state_; }
    // 상태 설정 (필요하면 public, 아니라면 protected/private로)
    void set_state(SessionState s) { state_ = s; }

    void set_udp_endpoint(const boost::asio::ip::udp::endpoint& ep) { 
        udp_endpoint_ = ep;
        udp_ep_update_time_ = std::chrono::steady_clock::now();
    }
    std::optional<boost::asio::ip::udp::endpoint> get_udp_endpoint() const { return udp_endpoint_; }
    std::chrono::steady_clock::time_point get_udp_ep_update_time() const { return udp_ep_update_time_; }
    void clear_udp_endpoint() { udp_endpoint_.reset(); }

	std::string get_client_ip() const;      // 클라이언트 IP 주소를 가져오는 함수
	unsigned short get_client_port() const; // 클라이언트 포트를 가져오는 함수

	// UDP endpoint 만료시간 갱신 및 조회
    void update_udp_alive_time();
    std::chrono::steady_clock::time_point get_last_udp_alive_time() const;

    // UDP 임시 토큰 
	void set_udp_token(const std::string& token) { udp_token_ = token; }
    const std::string get_udp_token() const { return udp_token_; }

    // 세션 상태를 문자열로 변환 (디버깅용)
    std::string state_to_string() const {
        switch (state_) {
            case SessionState::Handshaking: return "Handshaking";
            case SessionState::LoginWait: return "LoginWait";
            case SessionState::Ready: return "Ready";
            case SessionState::Closed: return "Closed";
            default: return "Unknown";
        }
	}

    // UDP Flood 방지 관련
    void set_udp_packet_count(size_t count) { udp_packet_count_ = count; }
    const size_t get_udp_packet_count() const { return udp_packet_count_.load(); }
    void inc_udp_packet_count() { ++udp_packet_count_; }
    void set_udp_packet_window(const std::chrono::steady_clock::time_point& window) {
        udp_packet_window_ = window;
    }
    const std::chrono::steady_clock::time_point get_udp_packet_window() const {
        return udp_packet_window_;
	}

    void set_zone_id(int zone_id) { zone_id_ = zone_id; }
    int get_zone_id() const { return zone_id_; }

    void enqueue_write(std::shared_ptr<std::string> msg);

    uint64_t get_generation() const { return generation_.load(); }
    void increment_generation() { ++generation_; }
	// 활성 세션 여부 설정 및 조회
    void set_active(bool v) { active_ = v; }
    bool is_active() const { return active_.load(); }

    boost::asio::steady_timer retry_timer_;  // 재시도 타이머
    void close_session();
    void do_handshake(int retry_count = 0);
    void do_read();

private:
    void do_write_queue();

};
