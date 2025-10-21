#pragma once
#include <nlohmann/json.hpp>
#include <boost/asio.hpp>
#include <unordered_map>
#include <functional>
#include <memory>
#include <string>
#include <atomic>
#include <future>
#include <functional>
#include "generated/wire.pb.h"

class SessionManager;
class DataHandler;

class DBmwRouter : public std::enable_shared_from_this<DBmwRouter> {
public:
    using json = nlohmann::json;
    using Handler = std::function<void(const json&)>;
    using ResponseCb = std::function<void(const json&)>;

    // io는 타이머/strand용, 세션/브로드캐스트 용도로 SM/DH 주입
    DBmwRouter(boost::asio::io_context& io,
        std::shared_ptr<SessionManager> session_manager,
        std::shared_ptr<DataHandler>    data_handler);

    // DBMiddlewareClient의 송신 함수를 주입
    // - 일반 JSON(4바이트 프리픽스) 전송용
    // - secret+JSON(4바이트 프리픽스) 전송용
    void set_senders(std::function<void(const json&)> send_json_fn,
        std::function<void(const json&)> send_secure_json_fn);

    // DB 미들웨어 수신 메시지 엔트리 포인트
    void handle(const json& j);
	// Protobuf 수신 메시지 엔트리 포인트
    void handle(const wire::Envelope& env);

    // 커스텀 타입 핸들러 등록 (옵션)
    void register_handler(const std::string& type, Handler h);

    // ===== 요청/응답 매칭 API =====
    // api: "get_user_profile" 등
    // payload: {"nickname":"..."} 등
    // secure: secret prefix 필요 여부 (login/keepalive는 true 권장)
    // timeout_ms: 타임아웃(ms)
    // max_retries: 타임아웃 시 재전송 횟수
    // session_id: 응답 매칭 후 세션 포워딩 등에 사용할 수 있는 메타(옵션)
    std::shared_future<json> send_request(const std::string& api,
        json payload,
        bool secure = false,
        int timeout_ms = 2000,
        int max_retries = 2,
        int session_id = -1);

    std::string send_request_cb(const std::string& api,
        json payload,
        bool secure,
        int timeout_ms,
        int max_retries,
        int session_id,
        ResponseCb on_ok,
        ResponseCb on_error);

    // 메트릭 스냅샷/로그
    struct Metrics {
        uint64_t sent = 0;
        uint64_t ok = 0;
        uint64_t timed_out = 0;
        uint64_t retried = 0;
        uint64_t failed = 0;
    };
    Metrics metrics_snapshot() const;
    void    log_metrics() const;
    // 타이머 시작
    void start_metrics_timer(std::chrono::seconds interval = std::chrono::seconds(60));

private:
    // 내부 핸들러들
    //void handle_login_ack(const json& j);
    //void handle_login_ok(const json& j);
    //void handle_keepalive_ack(const json& j);
    //void handle_db_query_result(const json& j);
    //void handle_push_notice(const json& j);
    //void handle_error_with_req_id(const json& j); // {"type":"error", "req_id":"...", ...}

    // 유틸
    static int  extract_session_id_from_req(const std::string& req_id);
    std::string make_req_id(int session_id);

    // 요청 테이블 엔트리
    struct Pending {
        std::promise<json>                           promise;
        std::shared_future<json>                     future;
        std::unique_ptr<boost::asio::steady_timer>   timer;
        json                                         request_json; // 재전송용
        bool                                         secure = false;
        int                                          retries = 0;
        int                                          max_retries = 0;
        int                                          timeout_ms = 0;
        int                                          session_id = -1;
        // 콜백 저장소 (선택적)
        ResponseCb                                   on_ok;
        ResponseCb                                   on_error;
        bool                                         use_callbacks = false;
    };

    // 전송/타임아웃 관리
    void send_now_locked(const std::string& req_id);
    void arm_timeout_locked(const std::string& req_id);
    void fulfill_and_erase_locked(const std::string& req_id, const json& result, bool success);

private:
    // 실행 컨텍스트
    boost::asio::io_context& io_;
    boost::asio::strand<boost::asio::any_io_executor> strand_;

    std::shared_ptr<SessionManager> session_manager_;
    std::shared_ptr<DataHandler>    data_handler_;

    boost::asio::steady_timer metrics_timer_;
    // 상태 기록 (예: 최근 타임아웃 수)
    std::atomic<int> last_timeout_count_{ 0 };

    void schedule_next_metrics(std::chrono::seconds interval);

    // 송신 함수(주입)
    std::function<void(const json&)> send_json_fn_;
    std::function<void(const json&)> send_secure_json_fn_;

    // 라우팅 테이블
    std::unordered_map<std::string, Handler> handlers_;

    // 요청 대기 테이블
    std::unordered_map<std::string, std::unique_ptr<Pending>> pending_;

    // req_id 생성기
    std::atomic<uint64_t> req_seq_{ 0 };

    // 메트릭
    std::atomic<uint64_t> m_sent_{ 0 };
    std::atomic<uint64_t> m_ok_{ 0 };
    std::atomic<uint64_t> m_to_{ 0 };
    std::atomic<uint64_t> m_retry_{ 0 };
    std::atomic<uint64_t> m_fail_{ 0 };
};
