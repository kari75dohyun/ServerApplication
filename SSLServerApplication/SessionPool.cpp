#include "SessionPool.h"  
#include <boost/asio.hpp>  
#include <cassert>
#include <iostream>
#include "Logger.h"

// SessionPool.cpp
SessionPool::SessionPool(size_t pool_size, size_t max_size, boost::asio::io_context& io, boost::asio::ssl::context& context, std::weak_ptr<DataHandler> handler)
    : io_(io), context_(context), handler_(handler), max_size_(max_size)
{
    pool_.reserve(pool_size);
    for (size_t i = 0; i < pool_size; ++i) {
        // **�ݵ�� io_�� ����Ͽ� strand�� socket�� �����ؾ� ��!**
        pool_.emplace_back(std::make_shared<SSLSession>(
            boost::asio::ip::tcp::socket(boost::asio::make_strand(io_)),
            context_, -1, handler_
        ));
        available_indices_.push(i);
    }
}

std::shared_ptr<SSLSession> SessionPool::acquire(boost::asio::ip::tcp::socket&& socket, int session_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    size_t idx;
    if (available_indices_.empty()) {
        // �ִ� Ȯ��ġ �ʰ� �� nullptr ��ȯ!
        if (pool_.size() >= max_size_) {
            //std::cerr << "[SessionPool][ERROR] Max session pool size reached (" << max_size_ << "). New session rejected!" << std::endl;
            g_logger->error("[SessionPool][ERROR] Max session pool size reached ( {}", max_size_, "). New session rejected!");
            //std::cerr << "[SessionPool] ���� Ǯ ũ��: " << pool_.size() << std::endl;
            g_logger->error("[SessionPool] ���� Ǯ ũ��: {}", pool_.size());
            return nullptr;
        }
        idx = pool_.size();
        auto new_session = std::make_shared<SSLSession>(
            boost::asio::ip::tcp::socket(boost::asio::make_strand(io_)),
            context_, -1, handler_
        );
        pool_.push_back(new_session);
        std::cout << "[SessionPool] Pool auto-expanded: size=" << pool_.size() << " / �ִ�ġ: " << max_size_ << std::endl;
    }
    else {
        idx = available_indices_.front();
        available_indices_.pop();
    }

    //std::cout << "[SessionPool][acquire] idx=" << idx << " (Ǯ �� ���� ��: " << pool_.size() << ", ��� ��: " << (pool_.size() - available_indices_.size()) << ")" << std::endl;
    g_logger->info("[SessionPool][acquire] idx= {}", idx, " (Ǯ �� ���� ��: {}", pool_.size(), ", ��� ��: {}", (pool_.size() - available_indices_.size()), ")");

    auto& sess = pool_[idx];
    sess->reset(std::move(socket), session_id);
    return sess;
}

// ���� �ڵ� Ȯ�� �߰�
void SessionPool::release(std::shared_ptr<SSLSession> session) {
    std::cout << "[release] session=" << session.get() << std::endl;
    // �̹� available_indices_�� ����ִ� idx���� üũ!
    auto it = std::find(pool_.begin(), pool_.end(), session);
    if (it != pool_.end()) {
        size_t idx = std::distance(pool_.begin(), it);
        std::cout << "[release] pool_ idx=" << idx << std::endl;

        // idx�� �̹� available_indices_�� ������ ������ ��ȯ�� �ǽ�!
        std::queue<size_t> tmp = available_indices_;
        bool found = false;
        while (!tmp.empty()) {
            if (tmp.front() == idx) found = true;
            tmp.pop();
        }
        if (found) std::cout << "[release] WARNING: idx already available! (�ߺ� ��ȯ)" << std::endl;
    }
}
