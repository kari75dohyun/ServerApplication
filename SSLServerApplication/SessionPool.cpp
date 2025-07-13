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
        // **반드시 io_를 사용하여 strand와 socket을 생성해야 함!**
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
        // 풀 확장
        if (pool_.size() >= max_size_) {
            g_logger->error("[SessionPool][acquire] Max session pool size reached ({}). New session rejected!", max_size_);
#ifndef NDEBUG
            assert(false && "SessionPool::acquire: 풀 최대치 초과");
#endif
            return nullptr;
        }
        idx = pool_.size();
        auto new_session = std::make_shared<SSLSession>(
            boost::asio::ip::tcp::socket(boost::asio::make_strand(io_)),
            context_, -1, handler_
        );
        pool_.push_back(new_session);
        g_logger->info("[SessionPool] Pool auto-expanded: size={} / 최대치: {}", pool_.size(), max_size_);
    }
    else {
        idx = available_indices_.front();
        available_indices_.pop();

        // 중복 할당 방지: 이미 사용중인 세션이 아닌지 확인 (상태 플래그 등으로 더 엄격하게 할 수도 있음)
        auto& sess = pool_[idx];
        if (sess.use_count() != 1) { // 참조 카운트가 1이 아니라면 이미 어딘가에서 사용중
            g_logger->warn("[SessionPool][acquire] idx={} 세션이 이미 사용중! 강제 reset 시도", idx);
#ifndef NDEBUG
            assert(false && "SessionPool::acquire: 중복 acquire 감지");
#endif
        }
    }

    auto& sess = pool_[idx];
    sess->reset(std::move(socket), session_id);

    g_logger->info("[SessionPool][acquire] idx={} (풀 총 세션 수: {}, 사용 중: {})", idx, pool_.size(), pool_.size() - available_indices_.size());
    return sess;
}

//std::shared_ptr<SSLSession> SessionPool::acquire(boost::asio::ip::tcp::socket&& socket, int session_id) {
//    std::lock_guard<std::mutex> lock(mutex_);
//
//    size_t idx;
//    if (available_indices_.empty()) {
//        // 최대 확장치 초과 시 nullptr 반환!
//        if (pool_.size() >= max_size_) {
//            //std::cerr << "[SessionPool][ERROR] Max session pool size reached (" << max_size_ << "). New session rejected!" << std::endl;
//            g_logger->error("[SessionPool][ERROR] Max session pool size reached ( {}", max_size_, "). New session rejected!");
//            //std::cerr << "[SessionPool] 현재 풀 크기: " << pool_.size() << std::endl;
//            g_logger->error("[SessionPool] 현재 풀 크기: {}", pool_.size());
//            return nullptr;
//        }
//        idx = pool_.size();
//        auto new_session = std::make_shared<SSLSession>(
//            boost::asio::ip::tcp::socket(boost::asio::make_strand(io_)),
//            context_, -1, handler_
//        );
//        pool_.push_back(new_session);
//        std::cout << "[SessionPool] Pool auto-expanded: size=" << pool_.size() << " / 최대치: " << max_size_ << std::endl;
//    }
//    else {
//        idx = available_indices_.front();
//        available_indices_.pop();
//    }
//
//    //std::cout << "[SessionPool][acquire] idx=" << idx << " (풀 총 세션 수: " << pool_.size() << ", 사용 중: " << (pool_.size() - available_indices_.size()) << ")" << std::endl;
//    g_logger->info("[SessionPool][acquire] idx= {}", idx, " (풀 총 세션 수: {}", pool_.size(), ", 사용 중: {}", (pool_.size() - available_indices_.size()), ")");
//
//    auto& sess = pool_[idx];
//    sess->reset(std::move(socket), session_id);
//    return sess;
//}

void SessionPool::release(std::shared_ptr<SSLSession> session) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 1. pool_에서 인덱스 찾기
    auto it = std::find(pool_.begin(), pool_.end(), session);
    if (it == pool_.end()) {
        g_logger->error("[SessionPool][release] 풀에 존재하지 않는 세션 release 시도! (무시됨)");
#ifndef NDEBUG
        assert(false && "SessionPool::release: 풀에 없는 세션 release!");
#endif
        return;
    }
    size_t idx = std::distance(pool_.begin(), it);

    // 2. 이미 available_indices_에 있는지 검사 (중복 release 방지)
    // (성능상 set으로 바꾸는게 이상적이지만, 기존 queue면 그대로!)
    {
        bool already_released = false;
        std::queue<size_t> tmp = available_indices_;
        while (!tmp.empty()) {
            if (tmp.front() == idx) {
                already_released = true;
                break;
            }
            tmp.pop();
        }
        if (already_released) {
            g_logger->warn("[SessionPool][release] 중복 release 감지! idx={}", idx);
#ifndef NDEBUG
            assert(false && "SessionPool::release: 중복 release 발생!");
#endif
            return;
        }
    }

    // 3. 정상 반환
    available_indices_.push(idx);
    g_logger->info("[SessionPool][release] idx={} 반환 (풀 총 세션 수: {}, 사용 중: {})",
        idx, pool_.size(), pool_.size() - available_indices_.size());
}



//// 세션 자동 확장 추가
//void SessionPool::release(std::shared_ptr<SSLSession> session) {
//    auto it = std::find(pool_.begin(), pool_.end(), session);
//    if (it != pool_.end()) {
//        size_t idx = std::distance(pool_.begin(), it);
//        std::lock_guard<std::mutex> lock(mutex_);
//        available_indices_.push(idx);
//        //std::cout << "[SessionPool][release] idx=" << idx << " 반환 (풀 총 세션 수: " << pool_.size() << ", 사용 중: " << (pool_.size() - available_indices_.size()) << ")" << std::endl;
//        g_logger->info("[SessionPool][release] idx= {}", idx, " 반환 (풀 총 세션 수: {}", pool_.size(), ", 사용 중: {}", pool_.size() - available_indices_.size(), ")");
//    }
//}
