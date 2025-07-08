#pragma once
#include <boost/asio.hpp>
#include <string>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <functional>
#include "MessageDispatcher.h"   // 추가!
#include "SessionPool.h"         // 세션 풀 헤더 추가


class SSLSession;  // 전방 선언, SSLSession 클래스가 정의되기 전에 사용
class SessionPool; // 전방 선언, SessionPool 클래스가 정의되기 전에 사용

class DataHandler {
private:
    unsigned int shard_count = 0;
    std::vector<std::unordered_map<int, std::shared_ptr<SSLSession>>> session_buckets;
    std::vector<std::mutex> session_mutexes;

    int get_shard(int session_id) const {
        return session_id % shard_count;
    }

    // 함수포인터(람다) 기반 Dispatcher
    MessageDispatcher dispatcher_;

    std::shared_ptr<SessionPool> session_pool_;  // 세션 풀 멤버 추가  

    std::unordered_map<std::string, std::weak_ptr<SSLSession>> nickname_to_session_;  // 닉네임→세션
    std::mutex nickname_mutex_; // 닉네임 맵 보호용

	// 글로벌 keepalive 관련
    boost::asio::steady_timer keepalive_timer_;
    //std::chrono::seconds ping_interval_ = std::chrono::seconds(20);     // ping 보낼 주기
    std::chrono::seconds keepalive_timeout_ = std::chrono::seconds(60); // pong 없을때 세션 끊는 시간

public:
    DataHandler(boost::asio::io_context& io); // 생성자 선언 필요!
    // *** 여기! 복사 금지 선언 추가 ***
    DataHandler(const DataHandler&) = delete;
    DataHandler& operator=(const DataHandler&) = delete;

    // TCP/SSL 세션 관리 
    // 세션 추가
    void add_session(int session_id, std::shared_ptr<SSLSession> session);

    // 세션 제거
    void remove_session(int session_id);

    // 세션 찾기
    // std::shared_ptr<SSLSession> get_session(int session_id) const;
    std::shared_ptr<SSLSession> get_session(int session_id);

    // SSL 핸드쉐이크를 비동기적으로 수행하는 함수
    void do_handshake(std::shared_ptr<SSLSession> session);

    // 데이터 읽기
    void do_read(std::shared_ptr<SSLSession> session);

    // 데이터 쓰기
    //void do_write(std::shared_ptr<SSLSession> session);

	// 브로드캐스트 메시지 전송
    void broadcast(const std::string& msg, int sender_session_id, std::shared_ptr<SSLSession> session);

	// UDP 메시지 수신 처리
    void on_udp_receive(const std::string& msg, const boost::asio::ip::udp::endpoint& from,
        boost::asio::ip::udp::socket& udp_socket);

    // 전체 세션을 정확하게 순회하는 함수(콜백 전달 방식)
    void for_each_session(std::function<void(const std::shared_ptr<SSLSession>&)> fn);
    size_t get_total_session_count();

    void broadcast_strict(const std::string& msg);

    //(샤드 전체 락 + 일관된 순회)
    template<typename Func>
    void for_each_session(Func&& func) {
        // 1. 모든 샤드 lock (deadlock 방지: 항상 0~N순)
        std::vector<std::unique_lock<std::mutex>> locks;
        locks.reserve(shard_count);
        for (unsigned int shard = 0; shard < shard_count; ++shard) {
            locks.emplace_back(session_mutexes[shard]);
        }

        // 2. 전체 세션 순회
        for (unsigned int shard = 0; shard < shard_count; ++shard) {
            for (const auto& [id, sess] : session_buckets[shard]) {
                func(sess);
            }
        }
        // 3. lock 자동 해제(스코프 종료)
    }

    // 세션 풀에 대한 getter 추가  
    std::shared_ptr<SessionPool> get_session_pool() const {
        return session_pool_;
    }

    // 세션 풀 설정 함수 추가  
    void set_session_pool(std::shared_ptr<SessionPool> pool) {
        session_pool_ = std::move(pool);
    }

    void register_nickname(const std::string& nickname, std::shared_ptr<SSLSession> session);
    void unregister_nickname(const std::string& nickname, std::shared_ptr<SSLSession> session);
    std::shared_ptr<SSLSession> find_session_by_nickname(const std::string& nickname);

	// 글로벌 keepalive 관련
    void start_keepalive_loop();
    void do_keepalive_check();
};