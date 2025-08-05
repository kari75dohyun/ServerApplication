#include "ZoneManager.h"
#include "Zone.h"
#include "Session.h"
#include "Logger.h"
#include "AppContext.h"

ZoneManager::ZoneManager(boost::asio::io_context& io, int zone_count)
    : io_(io)  
{
    for (int i = 1; i <= zone_count; ++i) {
        zones_[i] = std::make_shared<Zone>(io_, i, static_cast<size_t>(AppContext::instance().config.value("zone_max_sessions_count", 500)));
    }
}

std::shared_ptr<Zone> ZoneManager::get_zone(int zone_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = zones_.find(zone_id);
    if (it != zones_.end()) return it->second;
    // zones_에 zone_id가 없으면 새로 생성
    //auto zone = std::make_shared<Zone>(io_, zone_id, max_sessions);
    //zones_[zone_id] = zone;
    //return zone;
    // 
    //새로운 Zone은 생성하지 않음
    AppContext::instance().logger->warn("[ZoneManager] 요청된 zone_id={}가 존재하지 않음", zone_id);
    return nullptr;
}

// 현재 사용하지 않음. 사용하게 될때 헤더에 public 로 바꾼다.
void ZoneManager::remove_zone(int zone_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    zones_.erase(zone_id);
}

void ZoneManager::remove_session(const std::shared_ptr<Session>& sess) {
    if (!sess) return;
    int zone_id = sess->get_zone_id();
    if (zone_id > -1) {
        auto zone = get_zone(zone_id);
        if (zone) {
            zone->remove_session(sess);
			sess->set_zone_id(-1);  // 세션의 zone_id 초기화
			sess->clear_zone(); // 세션의 zone 정보 초기화
            AppContext::instance().logger->info("[ZONE] 세션 {} → ZONE {}에서 제거됨", sess->get_session_id(), zone_id);
        }
        else {
            AppContext::instance().logger->warn("[ZONE] 세션 {} → ZONE {} 제거 실패 (존 없음)", sess->get_session_id(), zone_id);
        }
	}
}
