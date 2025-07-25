#include <iostream>
#include "MessageDispatcher.h"
#include "SSLSession.h"
#include "DataHandler.h"
#include "Logger.h"
#include <memory>
#include "Utility.h"
#include "MessageHandlers/ChatHandler.h"
#include "MessageHandlers/CertifyHandler.h"
#include "MessageHandlers/KeepaliveHandler.h"
#include "UDPMessageHandlers/UdpHandler.h"
#include "UDPMessageHandlers/UdpBroadcastHandler.h"
#include "UDPMessageHandlers/UdpRegisterHandler.h"
#include "UDPMessageHandlers/UdpBroadcastZoneHandler.h"


MessageDispatcher::MessageDispatcher(DataHandler* handler, SessionManager* sessionmanager) : handler_(handler), session_manager_(sessionmanager){
	///////////// TCP 메시지 핸들러 등록 /////////////
    //////////////////////////////////////////////////
    // "login" 핸들러 등록
    register_handler("login", [handler, sessionmanager](std::shared_ptr<SSLSession> session, const nlohmann::json& msg) {login_handler(session, msg, handler, sessionmanager); });
	// "logout" 핸들러 등록
    register_handler("logout", [this](std::shared_ptr<SSLSession> session, const nlohmann::json& msg) { logout_handler(session, msg, handler_); });
    // "chat" 핸들러 등록
    register_handler("chat", [this](std::shared_ptr<SSLSession> session, const nlohmann::json& msg) { chat_handler(session, msg, handler_); });
	// keepalive 핸들러 등록
    register_handler("keepalive", [](std::shared_ptr<SSLSession> session, const nlohmann::json& msg) { keepalive_handler(session, msg); });

    //register_handler("ping", [this](std::shared_ptr<SSLSession> session, const nlohmann::json& msg) {
    //    session->on_ping_received(); // 아래에서 구현!
    //    // 원하면 "pong" 응답도 가능
    //    session->post_write(R"({"type":"pong"})" "\n");
    //    });
    //////////////////////////////////////////////////


    ///////////// UDP 메시지 핸들러 등록 /////////////
	// "udp" 메시지 핸들러 등록
    register_udp_handler("udp", [this](std::shared_ptr<SSLSession> session, const nlohmann::json& msg,
        const boost::asio::ip::udp::endpoint& from, boost::asio::ip::udp::socket& udp_socket) { Udp_handler(session, msg, from, udp_socket); });
	// "broadcast_udp" 메시지 핸들러 등록
    register_udp_handler("broadcast_udp", [this](std::shared_ptr<SSLSession> session, const nlohmann::json& msg,
        const boost::asio::ip::udp::endpoint& from, boost::asio::ip::udp::socket& udp_socket) { Udpbroadcast_handler(session, msg, from, udp_socket, handler_); });
	// "udp_register" 메시지 핸들러 등록
    register_udp_handler("udp_register", [this](std::shared_ptr<SSLSession> session, const nlohmann::json& msg,
        const boost::asio::ip::udp::endpoint& from, boost::asio::ip::udp::socket& udp_socket) {Udpregister_handler(session, msg, from, udp_socket); });
	// "broadcast_udp_zone" 메시지 핸들러 등록
    register_udp_handler("broadcast_udp_zone", [this](std::shared_ptr<SSLSession> session, const nlohmann::json& msg,
        const boost::asio::ip::udp::endpoint& from, boost::asio::ip::udp::socket& udp_socket) {Udpbroadcastzone_handler(session, msg, from, udp_socket, handler_); });
}

void MessageDispatcher::dispatch(std::shared_ptr<SSLSession> session, const nlohmann::json& msg) {
    std::string type = msg.value("type", "");
    auto it = handlers_.find(type);
    if (it != handlers_.end()) {
        it->second(session, msg);
    }
    else {
        // Unknown type
        session->post_write(R"({"type":"error","msg":"Unknown message type."})" "\n");
    }
}

void MessageDispatcher::dispatch_udp(std::shared_ptr<SSLSession> session, const std::string& raw_msg,
    const boost::asio::ip::udp::endpoint& from, boost::asio::ip::udp::socket& udp_socket)
{
    nlohmann::json jmsg;
    try {
        jmsg = nlohmann::json::parse(raw_msg);
    }
    catch (const std::exception& e) {
        // 파싱 에러 응답(UDP)
        std::string response = R"({"type":"error","msg":"Invalid JSON"})";
        g_logger->info("[UDP] Exception Invalid JSON: {}", e.what());
        auto data = std::make_shared<std::string>(response);
        udp_socket.async_send_to(boost::asio::buffer(*data), from,
            [data](const boost::system::error_code&, std::size_t) {});
        return;
    }
    std::string type = jmsg.value("type", "");
    auto it = udp_handlers_.find(type);
    if (it != udp_handlers_.end()) {
        it->second(session, jmsg, from, udp_socket);
    }
    else {
        // 기타 타입 기본 에코
        std::string response = "Echo(UDP): " + raw_msg;
        auto data = std::make_shared<std::string>(response);
        udp_socket.async_send_to(
            boost::asio::buffer(*data), from,
            [data](const boost::system::error_code& ec, std::size_t) {
                if (ec) g_logger->warn("[UDP][send_to callback] Error 6: {}", ec.message());
            });
    }
}

void MessageDispatcher::register_handler(const std::string& type, HandlerFunc handler) {
    handlers_[type] = handler;
}

void MessageDispatcher::register_udp_handler(const std::string& type, UdpHandlerFunc handler) {
    udp_handlers_[type] = handler;
}
