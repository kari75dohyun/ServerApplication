#pragma once
#include <functional>
#include <unordered_map>
#include <memory>
#include <nlohmann/json.hpp>
#include <boost/asio.hpp>

class SSLSession;
class DataHandler;
class SessionManager; // 전방 선언

class MessageDispatcher {
public:
    using HandlerFunc = std::function<void(std::shared_ptr<SSLSession>, const nlohmann::json&)>;
    using UdpHandlerFunc = std::function<void(std::shared_ptr<SSLSession>, const nlohmann::json&, const boost::asio::ip::udp::endpoint&, boost::asio::ip::udp::socket&)>;

    MessageDispatcher(DataHandler* handler, SessionManager* sessionmanager); // DataHandler 포인터 주입

    void dispatch(std::shared_ptr<SSLSession> session, const nlohmann::json& msg);

    void dispatch_udp(std::shared_ptr<SSLSession> session, const std::string& raw_msg,
        const boost::asio::ip::udp::endpoint& from, boost::asio::ip::udp::socket& udp_socket);

    void register_handler(const std::string& type, HandlerFunc handler);
    void register_udp_handler(const std::string& type, UdpHandlerFunc handler);

private:
    std::unordered_map<std::string, HandlerFunc> handlers_;
    std::unordered_map<std::string, UdpHandlerFunc> udp_handlers_;
    DataHandler* handler_;
    SessionManager* session_manager_;
    //std::weak_ptr<SessionManager> session_manager_;
};
