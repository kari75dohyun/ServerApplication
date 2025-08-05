#pragma once
#include <queue>
#include <tuple>
#include <boost/asio.hpp>
#include <boost/asio/strand.hpp>
#include <mutex>
#include <unordered_set>
#include <memory>

class Session;

class Zone : public std::enable_shared_from_this<Zone> {
private:
    size_t max_sessions_;  // zoen 입장 인원 최대 500명
    int zone_id_ = -1;
    std::unordered_map<int, std::weak_ptr<Session>> sessions_;

    // udp 송신 큐
    //std::queue<std::tuple<std::shared_ptr<std::string>, boost::asio::ip::udp::endpoint>> udp_send_queue_;
    //size_t max_parallel_send_ = 100;
    //size_t current_parallel_send_ = 0;

    boost::asio::strand<boost::asio::io_context::executor_type> strand_;

public:
	//static constexpr size_t max_udp_queue_size = 10000;  // 최대 UDP 송신 큐 크기

    Zone(boost::asio::io_context& io, int zone_id, size_t max_sessions);

    // zone에 소속된 유저 세션
    std::mutex session_mutex_;

    bool add_session(const std::shared_ptr<Session>& sess);
    void remove_session(const std::shared_ptr<Session>& sess);
    int get_zone_id() const { return zone_id_; }

    void broadcast(const std::string& msg, boost::asio::ip::udp::socket& udp_socket, const std::string& sender_nickname);
	// 현재 존에 속한 세션들
    //const auto& sessions() const { return sessions_; }
    void for_each_session(const std::function<void(const std::shared_ptr<Session>&)>& fn);

//private:
    //void try_send_next(boost::asio::ip::udp::socket& udp_socket);
};

