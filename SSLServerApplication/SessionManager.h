#pragma once
#include <unordered_map>
#include <memory>
#include <mutex>
#include <vector>
#include <functional>
#include <string>
#include "SSLSession.h"

class SSLSession; // 전방 선언

class SessionManager {
public:
    SessionManager(size_t shard_count = 16);

    // 세션 추가/삭제
    void add_session(std::shared_ptr<SSLSession> session);
    std::shared_ptr<SSLSession> remove_session(int session_id);

    // 세션 검색
    std::shared_ptr<SSLSession> find_session(int session_id);
    std::shared_ptr<SSLSession> find_session_by_nickname(const std::string& nickname);

    // 전체 세션에 대해 함수 적용
    void for_each_session(const std::function<void(const std::shared_ptr<SSLSession>&)>& fn);

    // 전체 세션 수
    size_t session_count();

    // 닉네임 등록/해제
    void register_nickname(const std::string& nickname, std::shared_ptr<SSLSession> session);
    void unregister_nickname(const std::string& nickname, std::shared_ptr<SSLSession> session);

    size_t get_total_session_count();

    void cleanup_expired_nicknames();

private:
    size_t shard_count_ = 0;
    std::vector<std::unordered_map<int, std::shared_ptr<SSLSession>>> session_buckets_;
    std::vector<std::mutex> session_mutexes_;

    std::unordered_map<std::string, std::weak_ptr<SSLSession>> nickname_index_;
    std::mutex nickname_mutex_;

    int get_shard(int session_id) const { return session_id % shard_count_; }
};

