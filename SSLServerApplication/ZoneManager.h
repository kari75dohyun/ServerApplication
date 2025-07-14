#pragma once
#include <unordered_map>
#include <memory>
#include <mutex>
#include <boost/asio.hpp>
#include "Zone.h"

class ZoneManager {
public:
    ZoneManager(boost::asio::io_context& io, int zone_count = 10);

    std::shared_ptr<Zone> get_or_create_zone(int zone_id, size_t max_sessions = 300);
    std::shared_ptr<Zone> get_zone(int zone_id);
    void remove_zone(int zone_id);

    template<typename Func>
    void for_each_zone(Func&& fn) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [id, z] : zones_) fn(id, z);
    }
private:
    boost::asio::io_context& io_;
    std::unordered_map<int, std::shared_ptr<Zone>> zones_;
    std::mutex mutex_;
};
