#include "SessionPool.h"  
#include <boost/asio.hpp>  
#include <cassert>
#include <iostream>
#include "Logger.h"
#include "AppContext.h"

// SessionPool.cpp
SessionPool::SessionPool(size_t pool_size, size_t max_size, boost::asio::io_context& io, std::weak_ptr<DataHandler> handler)
    : io_(io), handler_(handler), max_size_(max_size)
{
    AppContext::instance().logger->info("[DEBUG] SessionPool 생성됨! this={}", (void*)this);

    pool_.reserve(pool_size);
    for (size_t i = 0; i < pool_size; ++i) {
        // **반드시 io_를 사용하여 strand와 socket을 생성해야 함!**
        pool_.emplace_back(std::make_shared<Session>(
            boost::asio::ip::tcp::socket(boost::asio::make_strand(io_)),
            -1, handler_
        ));
        reusable_indices_.push(i);
    }
}

std::shared_ptr<Session> SessionPool::acquire(boost::asio::ip::tcp::socket&& socket, int session_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    size_t idx;
    //std::lock_guard<std::mutex> idx_lock(index_mutex_);  // 추가됨

    if (reusable_indices_.empty()) {
        // 풀 확장
        if (pool_.size() >= max_size_) {
            AppContext::instance().logger->error("[SessionPool][acquire] Max session pool size reached ({}). New session rejected!", max_size_);
#ifndef NDEBUG
            assert(false && "SessionPool::acquire: 풀 최대치 초과");
#endif
            return nullptr;
        }
        idx = pool_.size();
        auto new_session = std::make_shared<Session>(
            boost::asio::ip::tcp::socket(boost::asio::make_strand(io_)),
            -1, handler_
        );
        pool_.push_back(new_session);
        AppContext::instance().logger->info("[SessionPool] Pool auto-expanded: size={} / 최대치: {}", pool_.size(), max_size_);
    }
    else {
        idx = reusable_indices_.front();   // reusable_indices_ 사용
        reusable_indices_.pop();           // 토큰 소비
        available_index_set_.erase(idx);   // set에서 삭제 (중복 방지)

//        idx = available_indices_.front();
//        available_indices_.pop();
//
//        // 중복 할당 방지: 이미 사용중인 세션이 아닌지 확인 (상태 플래그 등으로 더 엄격하게 할 수도 있음)
//        auto& sess = pool_[idx];
//        //if (sess.use_count() != 1) { // 참조 카운트가 1이 아니라면 이미 어딘가에서 사용중
//		if (sess->is_active()) { // 세션이 활성화 상태라면
//            AppContext::instance().logger->critical("[SessionPool][acquire] idx={} 세션이 이미 사용중! 강제 reset 시도", idx);
//            // 운영 장애 알람 발송
//            send_admin_alert("[ALERT] SessionPool 중복 acquire 감지! idx=" + std::to_string(idx));
//#ifndef NDEBUG
//            assert(false && "SessionPool::acquire: 중복 acquire 감지");
//#endif
//            sess->close_session();
//        }
    }

    auto& sess = pool_[idx];
    sess->reset(std::move(socket), session_id);
    sess->set_active(true); // 세션 활성화!
	// 세션 풀에서 acquire 시점에 활성화 상태로 설정
    sess->set_release_callback([this](std::shared_ptr<Session> s) {
        this->release(s);
        });

    AppContext::instance().logger->info(
        "[SessionPool][acquire] idx={} | 풀 세션 수: {}, 가용 세션 수: {}, 사용 중 세션(풀 기준): {}", idx, pool_.size(),  reusable_indices_.size(), pool_.size() - reusable_indices_.size());
    return sess;
}

void SessionPool::release(std::shared_ptr<Session> session) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 1. pool_에서 인덱스 찾기
    auto it = std::find(pool_.begin(), pool_.end(), session);
    if (it == pool_.end()) {
        AppContext::instance().logger->error("[SessionPool][release] 풀에 존재하지 않는 세션 release 시도! (무시됨)");
#ifndef NDEBUG
        assert(false && "SessionPool::release: 풀에 없는 세션 release!");
#endif
        // 강제로 참조 해제
        session.reset();  // shared_ptr 카운트 감소

        return;
    }
    size_t idx = std::distance(pool_.begin(), it);

    // 2. 중복 release 방지 (unordered_set으로 검사)
    {
        //std::lock_guard<std::mutex> idx_lock(index_mutex_);  // 인덱스 동기화
        if (available_index_set_.find(idx) != available_index_set_.end()) {
            AppContext::instance().logger->critical("[SessionPool][release] 중복 release 감지! idx={}", idx);
            send_admin_alert("[ALERT] SessionPool 중복 acquire 감지! idx=" + std::to_string(idx));
#ifndef NDEBUG
            assert(false && "SessionPool::release: 중복 release 발생!");
#endif
            return;
        }
        // released_ = true 처리!
        session->mark_released();

        // 중복이 아니면 set과 큐에 삽입
        reusable_indices_.push(idx);
        available_index_set_.insert(idx);
    }

    // 3. 정상 반환
    session->set_active(false);
    //reusable_indices_.push(idx);
    AppContext::instance().logger->info("[SessionPool][release] idx={} 반환 (풀 총 세션 수: {}, 사용 중: {})",
        idx, pool_.size(), pool_.size() - reusable_indices_.size());
}

void SessionPool::for_each_active(const std::function<void(const std::shared_ptr<Session>&)>& fn) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& sess : pool_) {
        if (sess->is_active())
            fn(sess);
    }
}

size_t SessionPool::count_active() {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t cnt = 0;
    for (auto& sess : pool_) {
        if (sess->is_active())
            ++cnt;
    }
    return cnt;
}
