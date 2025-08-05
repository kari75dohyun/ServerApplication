#pragma once
#include "Session.h"   
#include <boost/asio.hpp>
#include <string>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <functional>
#include "SessionManager.h"
#include "MessageDispatcher.h"   // 추가!
#include "SessionPool.h"         // 세션 풀 헤더 추가
#include <boost/asio/strand.hpp>
#include "Zone.h"
#include "ZoneManager.h"
#include <nlohmann/json.hpp>
#include "Utility.h"
#include <thread>
#include <boost/asio/steady_timer.hpp>

using json = nlohmann::json;

class Session;  // 전방 선언, Session 클래스가 정의되기 전에 사용
class SessionPool; // 전방 선언, SessionPool 클래스가 정의되기 전에 사용
class Zone;
class SessionManager;

namespace std {
    template<>
    struct hash<boost::asio::ip::udp::endpoint> {
        std::size_t operator()(const boost::asio::ip::udp::endpoint& ep) const {
            return hash<std::string>()(ep.address().to_string()) ^ hash<unsigned short>()(ep.port());
        }
    };
}

class DataHandler {
private:
    //static constexpr int udp_expire_timeout_seconds = 300;  // UDP 만료 시간 제한(초)
    //static constexpr size_t user_limit_per_sec = 10;        // 1초에 유저당 10패킷 제한
    //static constexpr size_t total_limit_per_sec = 1000;     // 1초에 전체 1000패킷 제한
	//static constexpr size_t max_zone_count = 10;            // 최대 존 개수
    //static constexpr size_t max_udp_queue_size = 10000;       // 필요에 따라 값 조정
	//static constexpr size_t max_zone_session_count = 500;   // 최대 ZONE 세션 개수

    unsigned int shard_count = 0;

    // 함수포인터(람다) 기반 Dispatcher
    MessageDispatcher dispatcher_;

    std::shared_ptr<SessionManager> session_manager_; // SessionManager 멤버 추가

    std::shared_ptr<SessionPool> session_pool_;  // 세션 풀 멤버 추가  

    std::unordered_map<std::string, std::weak_ptr<Session>> nickname_to_session_;  // 닉네임→세션
    std::mutex nickname_mutex_; // 닉네임 맵 보호용

    // 글로벌 keepalive 관련
    boost::asio::steady_timer keepalive_timer_;
    //std::chrono::seconds ping_interval_ = std::chrono::seconds(20);     // ping 보낼 주기
    std::chrono::seconds keepalive_timeout_ = std::chrono::seconds(60); // pong 없을때 세션 끊는 시간
	// UDP Flood 방지 관련
    std::atomic<size_t> udp_total_packet_count_{ 0 };
    std::chrono::steady_clock::time_point udp_total_packet_window_{};

    // UDP 송신 큐와 직렬화용 strand
    //std::queue<std::tuple<std::shared_ptr<std::string>, boost::asio::ip::udp::endpoint>> udp_send_queue_;
    //bool udp_send_in_progress_ = false;
    std::unique_ptr<boost::asio::strand<boost::asio::any_io_executor>> udp_send_strand_;
    //void try_send_next_udp(boost::asio::ip::udp::socket& udp_socket);

	boost::asio::io_context& io_context_; // ZoneManager 생성에 필요
	ZoneManager zone_manager_;            // Zoneanager 멤버 추가

    boost::asio::steady_timer monitor_timer_; // 모니터링 타이머

    struct BadUdpInfo {
        int fail_count = 0;
        std::chrono::steady_clock::time_point first_fail_time;
    };

    std::unordered_map<std::string, BadUdpInfo> bad_udp_map_;
    std::mutex bad_udp_mutex_;

    // 설정값: 제한 횟수, 차단 기간 (초)
    int bad_udp_limit_ = 10;          // 실패 최대 허용 횟수
    std::chrono::seconds block_time_ = std::chrono::seconds(60);  // 차단 유지 시간

    boost::asio::steady_timer cleanup_timer_;  // 비활성 세션 클린업용 타이머

    std::unordered_map<boost::asio::ip::udp::endpoint, std::weak_ptr<Session>> udp_endpoint_to_session_;
    std::mutex udp_endpoint_mutex_;

private:
    UdpRateLimiterShard udp_global_limiter_;

public:
    DataHandler(boost::asio::io_context& io, std::shared_ptr<SessionManager> session_manager, int zone_count, size_t udp_shard_count); // 생성자 선언 필요!
    // *** 여기! 복사 금지 선언 추가 ***
    DataHandler(const DataHandler&) = delete;
    DataHandler& operator=(const DataHandler&) = delete;

    void dispatch(const std::shared_ptr<Session>& session, const json& msg);
    // TCP 세션 관리 
    // 세션 추가
    void add_session(int session_id, std::shared_ptr<Session> session);

    // 세션 제거
    void remove_session(int session_id);

    // 브로드캐스트 메시지 전송
    void broadcast(const std::string& msg, int sender_session_id, std::shared_ptr<Session> session);

    // UDP 메시지 수신 처리
    void on_udp_receive(const std::string& msg, const boost::asio::ip::udp::endpoint& from,
        boost::asio::ip::udp::socket& udp_socket);

    // 전체 세션을 정확하게 순회하는 함수(콜백 전달 방식)
    void for_each_session(const std::function<void(const std::shared_ptr<Session>&)> fn);

    void broadcast_strict(const std::string& msg);

    // 세션 풀에 대한 getter 추가  
    std::shared_ptr<SessionPool> get_session_pool() const {
        return session_pool_;
    }

    // 세션 풀 설정 함수 추가  
    void set_session_pool(std::shared_ptr<SessionPool> pool) {
        session_pool_ = std::move(pool);
    }

    std::shared_ptr<Session> find_session_by_nickname(const std::string& nickname);

    // 글로벌 keepalive 관련
    void start_keepalive_loop();
    void do_keepalive_check();

	//UDP 송신큐 Srand 기반 직렬화로 수정.
    void udp_broadcast(const std::string& msg, boost::asio::ip::udp::socket& udp_socket, const std::string& sender_nickname);

	// 로그인 하지 않고 DDos 공격하는 세션 정리
    void cleanup_unauth_sessions(size_t max_unauth); // 미인증 세션 정리

	// UDP 엔드포인트 만료 처리
    void expire_stale_udp_endpoints(std::chrono::seconds timeout);

	// UDP Flood 방지 관련 Set, Get 함수 추가
    void set_udp_total_packet_count(size_t count) {
        udp_total_packet_count_ = count;
	}
    const size_t get_udp_total_packet_count() const {
        return udp_total_packet_count_;
    }
    void inc_udp_total_packet_count() { ++udp_total_packet_count_; }
    void set_udp_total_packet_window(const std::chrono::steady_clock::time_point& window) {
        udp_total_packet_window_ = window;
    }
    const std::chrono::steady_clock::time_point get_udp_total_packet_window() const {
        return udp_total_packet_window_;
	}

    // zone, channel, room별로 나눈다.
    void assign_session_to_zone(std::shared_ptr<Session> session, int zone_id);  // zone 배정 함수
    void udp_broadcast_zone(int zone_id, const std::string& msg, boost::asio::ip::udp::socket& udp_socket, const std::string& sender_nickname);

    std::shared_ptr<Zone> get_zone(int zone_id);

	void start_monitor_loop(); // 모니터링 루프 시작 함수

    void register_bad_udp_packet(const boost::asio::ip::udp::endpoint& endpoint);
    bool is_blocked_udp(const boost::asio::ip::udp::endpoint& endpoint);

    void start_cleanup_loop();  // 주기적 클린업 시작

    std::shared_ptr<Session> getSessionByEndpoint(const boost::asio::ip::udp::endpoint& endpoint);
};