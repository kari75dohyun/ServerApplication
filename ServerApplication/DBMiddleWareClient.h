#pragma once
#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>
#include <nlohmann/json.hpp>
#include <queue>
#include <memory>
#include <atomic>
#include <functional>
#include <string>
#include <array>
#include "MessageBufferManager.h"
#include "generated/wire.pb.h"

class DBMiddlewareClient : public std::enable_shared_from_this<DBMiddlewareClient> {
public:
    using OnMessageFn = std::function<void(const nlohmann::json&)>;
    // Protobuf 콜백
    using OnProtoFn = std::function<void(const wire::Envelope&)>;

    DBMiddlewareClient(boost::asio::io_context& io,
        std::string host,
        uint16_t port,
        OnMessageFn on_message,
        std::string login_nickname,
        std::string secret);

    void start();
    void stop();
    bool is_connected() const { return connected_.load(); }

    // 프로토콜 콜백 세터
    void set_on_message_pb(OnProtoFn fn) { on_message_pb_ = std::move(fn); }
    // Protobuf 전송 
    void send_proto(const google::protobuf::MessageLite& msg);
    void send_secure_proto(const google::protobuf::MessageLite& msg);

    // JSON만 body로 보내고 싶을 때 (자동 4바이트 프리픽스)
    void send_json(const nlohmann::json& j);

    // 이미 만들어 둔 바디(예: "secret + json")를 4바이트 프리픽스로 감싸 전송
    void send_framed(const std::string& body);

    // secret 프리픽스 + JSON을 함께 보내야 하는 패킷용 (login/keepalive)
    void send_secure_json(const nlohmann::json& j);

    void set_heartbeat_interval(int seconds) { heartbeat_sec_ = seconds; }

    // keepalive ACK 수신 시 호출 (미수신 카운터 리셋)
    void note_heartbeat_ack();

	// 인증 완료 표시 -> authed_ , Setter 함수
    // DB 미들웨어 인증 상태를 갱신하는 함수
    // server_login_ack OK 를 받으면 true 로 설정 -> 하트비트 전송 시작
    // 실패 시 false 로 설정 -> 인증되지 않은 상태로 간주
    // authed_ 는 atomic<bool> 이므로 멀티스레드 환경에서도 안전하게 접근 가능
    void mark_authed(bool v) { authed_.store(v); }

private:
    void do_connect();
    void schedule_reconnect();
    void do_read_some();
    void flush_write_queue();
    void close_socket();

    void server_send_login();          // 접속 직후 로그인 (secret + json → framed)
    void arm_heartbeat_timer(); // 주기 keepalive (secret + json → framed)

private:
    boost::asio::ip::tcp::socket socket_;
    boost::asio::strand<boost::asio::any_io_executor> strand_;
    boost::asio::ip::tcp::resolver resolver_;
    boost::asio::steady_timer reconnect_timer_;
    boost::asio::steady_timer heartbeat_timer_;

    std::string host_;
    uint16_t port_;
    std::string server_login_nickname_;
    std::string secret_;     // secret 저장

    std::atomic<bool> connected_{ false };
    std::atomic<bool> stopping_{ false };
    std::atomic<bool> server_login_sent_{ false };
    std::atomic<bool> authed_{ false };

    // 하트비트 미수신 카운터 & 임계치
    std::atomic<int> missed_heartbeat_{ 0 };
    int heartbeat_miss_limit_ = 3;  // 연속 3회 미수신 시 재접속 같은 정책에 사용

    std::array<char, 4096> read_buf_{};  // 제로 초기화
    MessageBufferManager msg_buf_mgr_;

    std::queue<std::shared_ptr<std::string>> write_queue_;
    bool write_in_progress_ = false;

    int reconnect_sec_ = 2;
    int reconnect_sec_max_ = 30;
    int heartbeat_sec_ = 20;

    OnMessageFn on_message_;
    OnProtoFn on_message_pb_;
};
