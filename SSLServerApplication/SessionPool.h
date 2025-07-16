#pragma once
#include <vector>
#include <queue>
#include <memory>
#include <mutex>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include "SSLSession.h"

class SessionPool {
public:
    SessionPool(size_t pool_size, size_t max_size, boost::asio::io_context& io, boost::asio::ssl::context& context, std::weak_ptr<DataHandler> handler);
    // 사용 가능한 세션 획득 (없으면 nullptr 반환)
    std::shared_ptr<SSLSession> acquire(boost::asio::ip::tcp::socket&& socket, int session_id);
    // 세션 반환(재사용)
    void release(std::shared_ptr<SSLSession> session);
    size_t pool_size() const { return pool_.size(); }  // 현재 풀 크기 반환
	// 현재 사용 가능한 세션 수 반환
    void for_each_active(const std::function<void(const std::shared_ptr<SSLSession>&)>& fn);
    size_t count_active();

private:
    std::vector<std::shared_ptr<SSLSession>> pool_;
    std::queue<size_t> available_indices_;
    std::mutex mutex_;
    boost::asio::ssl::context& context_;
    std::weak_ptr<DataHandler> handler_;

    boost::asio::io_context& io_;
    size_t max_size_;   // 최대 확장치!
};

