#include "SessionPool.h"  
#include <boost/asio.hpp>  
#include <cassert>
#include <iostream>

// SessionPool.cpp
SessionPool::SessionPool(size_t pool_size, boost::asio::io_context& io, boost::asio::ssl::context& context, std::weak_ptr<DataHandler> handler)
    : io_(io), context_(context), handler_(handler)
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
    std::cout << "[acquire] session_id=" << session_id<< ", socket.is_open()=" << socket.is_open() << std::endl;
    std::lock_guard<std::mutex> lock(mutex_);  
    if (available_indices_.empty()) return nullptr;  

    size_t idx = available_indices_.front();  
    available_indices_.pop();  
    auto& sess = pool_[idx];  

    // ����, ����ID �� ����ε�  
    sess->reset(std::move(socket), session_id);  

    return sess;  
}  

void SessionPool::release(std::shared_ptr<SSLSession> session) {
    // �ߺ� release�� remove_session���� üũ�ϹǷ�, ���⼭�� �����ϰ�!
    auto it = std::find(pool_.begin(), pool_.end(), session);
    if (it != pool_.end()) {
        size_t idx = std::distance(pool_.begin(), it);
        available_indices_.push(idx);
    }
}

//void SessionPool::release(std::shared_ptr<SSLSession> session) {
//    std::cout << "[release] session=" << session.get() << std::endl;
//    // �̹� available_indices_�� ����ִ� idx���� üũ!
//    auto it = std::find(pool_.begin(), pool_.end(), session);
//    if (it != pool_.end()) {
//        size_t idx = std::distance(pool_.begin(), it);
//        std::cout << "[release] pool_ idx=" << idx << std::endl;
//
//        // idx�� �̹� available_indices_�� ������ ������ ��ȯ�� �ǽ�!
//        std::queue<size_t> tmp = available_indices_;
//        bool found = false;
//        while (!tmp.empty()) {
//            if (tmp.front() == idx) found = true;
//            tmp.pop();
//        }
//        if (found) std::cout << "[release] WARNING: idx already available! (�ߺ� ��ȯ)" << std::endl;
//    }
//}
