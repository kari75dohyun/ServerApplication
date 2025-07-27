#include "SessionManager.h"
#include "Logger.h"
#include <chrono>
#include "AppContext.h"

SessionManager::SessionManager(size_t shard_count)
    : shard_count_(shard_count),
    session_buckets_(shard_count),
    session_mutexes_(shard_count)
{
}

void SessionManager::add_session(std::shared_ptr<Session> session) {
    int session_id = session->get_session_id();
    int shard = get_shard(session_id);

    std::shared_ptr<Session> replaced_session;
    {
        std::lock_guard<std::mutex> lock(session_mutexes_[shard]);
        auto& bucket = session_buckets_[shard];
        auto it = bucket.find(session_id);
        if (it != bucket.end()) {
            replaced_session = it->second;  // 기존 세션 저장
            bucket.erase(it);
        }
        bucket[session_id] = session; // 새 세션 등록
    }

    // 락을 벗어난 뒤에 안전하게 자원 정리
    if (replaced_session && !replaced_session->is_closed()) {
        replaced_session->close_session();
        // 필요하면 세션 풀에도 반환 (DataHandler가 호출한 쪽에서 처리하는 게 깔끔)
    }
}

std::shared_ptr<Session> SessionManager::remove_session(int session_id) {
    int shard = get_shard(session_id);
    std::shared_ptr<Session> removed_session;
    {
        std::lock_guard<std::mutex> lock(session_mutexes_[shard]);
        auto it = session_buckets_[shard].find(session_id);
        if (it != session_buckets_[shard].end()) {
            removed_session = it->second;
            session_buckets_[shard].erase(it);
        }
    }
    return removed_session;
}

std::shared_ptr<Session> SessionManager::find_session(int session_id) {
    int shard = get_shard(session_id);
    std::lock_guard<std::mutex> lock(session_mutexes_[shard]);
    auto it = session_buckets_[shard].find(session_id);
    if (it != session_buckets_[shard].end())
        return it->second;
    return nullptr;
}

void SessionManager::for_each_session(const std::function<void(const std::shared_ptr<Session>&)>& fn) {
    // 내부 샤드별 락 + 전체 세션 복사(락 보장)
    std::vector<std::shared_ptr<Session>> sessions;
    for (unsigned int shard = 0; shard < shard_count_; ++shard) {
        std::lock_guard<std::mutex> lock(session_mutexes_[shard]);
        for (const auto& [id, sess] : session_buckets_[shard]) {
            if (sess) sessions.push_back(sess);
        }
    }
    for (const auto& sess : sessions) {
        fn(sess);
    }
}


size_t SessionManager::session_count() {
    size_t total = 0;
    for (size_t shard = 0; shard < shard_count_; ++shard) {
        std::lock_guard<std::mutex> lock(session_mutexes_[shard]);
        total += session_buckets_[shard].size();
    }
    return total;
}

// 닉네임 관리
void SessionManager::register_nickname(const std::string& nickname, std::shared_ptr<Session> session) {
    AppContext::instance().logger->info("[DEBUG][TCP] SessionManager address: {}", (void*)this);
    std::lock_guard<std::mutex> lock(nickname_mutex_);
    auto it = nickname_index_.find(nickname);
    if (it != nickname_index_.end()) {
        auto prev = it->second.lock();
        if (prev && prev != session) {
            // [1] 이전 세션 강제 종료
            prev->post_write(R"({"type":"error","msg":"다른 곳에서 로그인되어 기존 연결이 종료됩니다."})" "\n");
            prev->close_session();
            // **여기서 바로 nickname_index_를 overwrite하면, prev의 unregister_nickname이 꼬일 수 있으니**
            // (optionally) prev가 unregister_nickname을 호출할 때만 nickname_index_에서 지우도록 함
        }
    }

    nickname_index_[nickname] = session; // 무조건 overwrite (단, unregister시 “소유자”만 삭제)
}

void SessionManager::unregister_nickname(const std::string& nickname, std::shared_ptr<Session> session) {
    std::lock_guard<std::mutex> lock(nickname_mutex_);
    auto it = nickname_index_.find(nickname);
    if (it != nickname_index_.end()) {
        // 본인이 “마지막으로 등록된 세션”일 때만 삭제!
        if (!it->second.expired() && it->second.lock() == session) {
            nickname_index_.erase(it);
        }
    }
	cleanup_expired_nicknames();  // 만료된 닉네임 정리
}

std::shared_ptr<Session> SessionManager::find_session_by_nickname(const std::string& nickname) {
    std::lock_guard<std::mutex> lock(nickname_mutex_);
    auto it = nickname_index_.find(nickname);
    if (it != nickname_index_.end()) {
        return it->second.lock();
    }
    return nullptr;
}

size_t SessionManager::get_total_session_count() {
    size_t total = 0;
    for (unsigned int i = 0; i < shard_count_; ++i) {
        std::lock_guard<std::mutex> lock(session_mutexes_[i]);
        total += session_buckets_[i].size();
    }
    return total;
}

void SessionManager::cleanup_expired_nicknames() {
    std::lock_guard<std::mutex> lock(nickname_mutex_);
    for (auto it = nickname_index_.begin(); it != nickname_index_.end(); ) {
        if (it->second.expired()) {
            AppContext::instance().logger->info("[NICKNAME SWEEP] expired nickname entry removed: {}", it->first);
            it = nickname_index_.erase(it);
        }
        else {
            ++it;
        }
    }
}

void SessionManager::cleanup_inactive_sessions(std::chrono::seconds max_idle_time) {
    auto now = std::chrono::steady_clock::now();
    for (size_t shard = 0; shard < shard_count_; ++shard) {
        std::lock_guard<std::mutex> lock(session_mutexes_[shard]);
        for (auto it = session_buckets_[shard].begin(); it != session_buckets_[shard].end(); ) {
            auto& session = it->second;
            if (session) {
                auto last_alive = session->get_last_alive_time();
                if (now - last_alive > max_idle_time) {
                    AppContext::instance().logger->info("[SessionManager] 세션 {} 비활성 시간 초과, 종료 처리", session->get_session_id());
                    session->close_session();
                    it = session_buckets_[shard].erase(it);
                    continue;
                }
            }
            ++it;
        }
    }
}