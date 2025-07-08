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
        // **반드시 io_를 사용하여 strand와 socket을 생성해야 함!**
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

    // 소켓, 세션ID 등 재바인딩  
    sess->reset(std::move(socket), session_id);  

    return sess;  
}  

void SessionPool::release(std::shared_ptr<SSLSession> session) {
    // 중복 release는 remove_session에서 체크하므로, 여기서는 간결하게!
    auto it = std::find(pool_.begin(), pool_.end(), session);
    if (it != pool_.end()) {
        size_t idx = std::distance(pool_.begin(), it);
        available_indices_.push(idx);
    }
}

//void SessionPool::release(std::shared_ptr<SSLSession> session) {
//    std::cout << "[release] session=" << session.get() << std::endl;
//    // 이미 available_indices_에 들어있는 idx인지 체크!
//    auto it = std::find(pool_.begin(), pool_.end(), session);
//    if (it != pool_.end()) {
//        size_t idx = std::distance(pool_.begin(), it);
//        std::cout << "[release] pool_ idx=" << idx << std::endl;
//
//        // idx가 이미 available_indices_에 있으면 “이중 반환” 의심!
//        std::queue<size_t> tmp = available_indices_;
//        bool found = false;
//        while (!tmp.empty()) {
//            if (tmp.front() == idx) found = true;
//            tmp.pop();
//        }
//        if (found) std::cout << "[release] WARNING: idx already available! (중복 반환)" << std::endl;
//    }
//}
