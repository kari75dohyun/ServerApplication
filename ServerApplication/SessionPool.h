#pragma once
#include <vector>
#include <queue>
#include <memory>
#include <mutex>
#include <boost/asio.hpp>
#include "Session.h"
#include <unordered_set>

class SessionPool {
public:
    SessionPool(size_t pool_size, size_t max_size, boost::asio::io_context& io, std::weak_ptr<DataHandler> handler);
    // 사용 가능한 세션 획득 (없으면 nullptr 반환)
    std::shared_ptr<Session> acquire(boost::asio::ip::tcp::socket&& socket, int session_id);
    // 세션 반환(재사용)
    void release(std::shared_ptr<Session> session);
    size_t pool_size() const { return pool_.size(); }  // 현재 풀 크기 반환
	// 현재 사용 가능한 세션 수 반환
    void for_each_active(const std::function<void(const std::shared_ptr<Session>&)>& fn);
    size_t count_active();

private:
    std::vector<std::shared_ptr<Session>> pool_;
    //std::queue<size_t> available_indices_;
    std::mutex mutex_;
    std::weak_ptr<DataHandler> handler_;

    boost::asio::io_context& io_;
    size_t max_size_;   // 최대 확장치!

    // 인덱스 중복 검사
    std::queue<size_t> reusable_indices_;               // 인덱스 큐
    std::unordered_set<size_t> available_index_set_;    // 중복 검사용
    std::mutex index_mutex_;                            // 인덱스 큐 + set 동기화
};

