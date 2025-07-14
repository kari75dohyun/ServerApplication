#include "ZoneManager.h"
#include "Zone.h"
#include "SSLSession.h"
#include "Logger.h"

ZoneManager::ZoneManager(boost::asio::io_context& io, int zone_count)
    : io_(io)  
{
    for (int i = 1; i <= zone_count; ++i) {
        zones_[i] = std::make_shared<Zone>(io_, i, 300);
    }
}

std::shared_ptr<Zone> ZoneManager::get_or_create_zone(int zone_id, size_t max_sessions) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = zones_.find(zone_id);
    if (it != zones_.end()) return it->second;
    auto zone = std::make_shared<Zone>(io_, zone_id, max_sessions);
    zones_[zone_id] = zone;
    return zone;
}

std::shared_ptr<Zone> ZoneManager::get_zone(int zone_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = zones_.find(zone_id);
    if (it != zones_.end()) return it->second;
    return nullptr;
}

// 현재 사용하지 않음.
void ZoneManager::remove_zone(int zone_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    zones_.erase(zone_id);
}

