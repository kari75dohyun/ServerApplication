#include <iostream>
#include "MessageDispatcher.h"
#include "Session.h"
#include "DataHandler.h"
#include "Logger.h"
#include <memory>
#include "Utility.h"
#include "MessageHandlers/ChatHandler.h"
#include "MessageHandlers/ChatZoneHandler.h"
#include "MessageHandlers/CertifyHandler.h"
#include "MessageHandlers/KeepaliveHandler.h"
#include "UDPMessageHandlers/UdpHandler.h"
#include "UDPMessageHandlers/UdpBroadcastHandler.h"
#include "UDPMessageHandlers/UdpRegisterHandler.h"
#include "UDPMessageHandlers/UdpBroadcastZoneHandler.h"
#include "AppContext.h"


MessageDispatcher::MessageDispatcher(DataHandler* handler, SessionManager* sessionmanager) : handler_(handler), session_manager_(sessionmanager){
	///////////// TCP 메시지 핸들러 등록 /////////////
    //////////////////////////////////////////////////
    // "login" 핸들러 등록
    register_handler("login", [handler, sessionmanager](std::shared_ptr<Session> session, const nlohmann::json& msg) {login_handler(session, msg, handler, sessionmanager); });
	// "logout" 핸들러 등록
    register_handler("logout", [this](std::shared_ptr<Session> session, const nlohmann::json& msg) { logout_handler(session, msg, handler_); });
    // "chat" 핸들러 등록
    register_handler("chat", [this](std::shared_ptr<Session> session, const nlohmann::json& msg) { chat_handler(session, msg, handler_); });
	// "chat_zone" 핸들러 등록
    register_handler("chat_zone", [this](std::shared_ptr<Session> session, const nlohmann::json& msg) { chatzone_handler(session, msg, handler_); });
    // keepalive 핸들러 등록
    register_handler("keepalive", [](std::shared_ptr<Session> session, const nlohmann::json& msg) { keepalive_handler(session, msg); });

    //register_handler("ping", [this](std::shared_ptr<Session> session, const nlohmann::json& msg) {
    //    session->on_ping_received(); // 아래에서 구현!
    //    // 원하면 "pong" 응답도 가능
    //    session->post_write(R"({"type":"pong"})" "\n");
    //    });
    //////////////////////////////////////////////////


    ///////////// UDP 메시지 핸들러 등록 /////////////
	// "udp" 메시지 핸들러 등록
    register_udp_handler("udp", [this](std::shared_ptr<Session> session, const nlohmann::json& msg,
        const boost::asio::ip::udp::endpoint& from, boost::asio::ip::udp::socket& udp_socket) { Udp_handler(session, msg, from, udp_socket); });
	// "broadcast_udp" 메시지 핸들러 등록
    register_udp_handler("broadcast_udp", [this](std::shared_ptr<Session> session, const nlohmann::json& msg,
        const boost::asio::ip::udp::endpoint& from, boost::asio::ip::udp::socket& udp_socket) { Udpbroadcast_handler(session, msg, from, udp_socket, handler_); });
	// "udp_register" 메시지 핸들러 등록
    register_udp_handler("udp_register", [this](std::shared_ptr<Session> session, const nlohmann::json& msg,
        const boost::asio::ip::udp::endpoint& from, boost::asio::ip::udp::socket& udp_socket) {Udpregister_handler(session, msg, from, udp_socket); });
	// "broadcast_udp_zone" 메시지 핸들러 등록
    register_udp_handler("broadcast_udp_zone", [this](std::shared_ptr<Session> session, const nlohmann::json& msg,
        const boost::asio::ip::udp::endpoint& from, boost::asio::ip::udp::socket& udp_socket) {Udpbroadcastzone_handler(session, msg, from, udp_socket, handler_); });
}

void MessageDispatcher::dispatch(std::shared_ptr<Session> session, const nlohmann::json& msg) {
    try {
        std::string type = msg.value("type", "");
            if (type.empty()) {
            AppContext::instance().logger->warn(
                "[DISPATCH][Invalid message] type 필드 없음. session_id={} msg={}",
                session->get_session_id(), msg.dump()
                 );
            session->post_write(R"({"type":"error","msg":"Missing message type field"})" "\n");
            return;
            
        }
        auto it = handlers_.find(type);
        if (it != handlers_.end()) {
            try {
                it->second(session, msg); // 실제 핸들러 호출
            }
            catch (const nlohmann::json::type_error& e) {
                AppContext::instance().logger->warn(
                    "[DISPATCH][{}] JSON 필드 타입 오류: {} / msg={}",
                    type, e.what(), msg.dump()
                    );
                session->post_write(R"({"type":"error","msg":"Invalid JSON field type"})" "\n");
            }
            catch (const std::exception& e) {
                AppContext::instance().logger->error(
                    "[DISPATCH][{}] 핸들러 내부 예외 발생: {} / session_id={}",
                    type, e.what(), session->get_session_id()
                    );
            // 핸들러 오류는 세션 종료
                    session->close_session();
            }
        }
        else {
            // Unknown type
                session->post_write(R"({"type":"error","msg":"Unknown message type"})" "\n");
            AppContext::instance().logger->warn(
                "[DISPATCH][Unknown type][session_id={}] type='{}' original msg={}",
                session->get_session_id(), type, msg.dump()
                );
            session->close_session();
        }
    }
    catch (const std::exception& e) {
        AppContext::instance().logger->critical(
            "[DISPATCH][FATAL] dispatch 예외: {} / session_id={}",
            e.what(), session->get_session_id()
            );
        session->close_session();
    }
  //  std::string type = msg.value("type", "");
  //  auto it = handlers_.find(type);
  //  if (it != handlers_.end()) {
  //      it->second(session, msg);
  //  }
  //  else {
  //      // Unknown type
  //      session->post_write(R"({"type":"error","msg":"Unknown message type."})" "\n");
  //      // 없는 type  로그 남기고 세션 강제 종료
  //      AppContext::instance().logger->warn(
  //          "[DISPATCH][Unknown type][session_id={}] type='{}' original msg: {}",
  //          session->get_session_id(), type, msg.dump()
  //      );

		//session->close_session(); // 세션 종료
  //  }
}

void MessageDispatcher::dispatch_udp(std::shared_ptr<Session> session, const std::string& raw_msg,
    const boost::asio::ip::udp::endpoint& from, boost::asio::ip::udp::socket& udp_socket)
{
    nlohmann::json jmsg;
    try {
        jmsg = nlohmann::json::parse(raw_msg);
    }
    catch (const std::exception& e) {
        // 파싱 에러 응답(UDP)
        //std::string response = R"({"type":"error","msg":"Invalid JSON"})";
        std::string response = R"({"type":"udp_error","code":"INVALID_JSON","msg":"Invalid JSON"})";
        AppContext::instance().logger->info("[UDP] Exception Invalid JSON: {}", e.what());
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
                if (ec) AppContext::instance().logger->warn("[UDP][send_to callback] Error 6: {}", ec.message());
            });
    }
}

// 이미 파싱된 JSON을 직접 전달받는 UDP용
void MessageDispatcher::dispatch_udp_parsed(
    std::shared_ptr<Session> session,
    const nlohmann::json & parsed_msg,
    const boost::asio::ip::udp::endpoint & from,
    boost::asio::ip::udp::socket & udp_socket)
    {
    try {
        std::string type = parsed_msg.value("type", "");
        auto it = udp_handlers_.find(type);
        if (it != udp_handlers_.end()) {
            it->second(session, parsed_msg, from, udp_socket);
        }
        else {
            static const std::string kUnknownUdp = R"({"type":"error","msg":"Unknown UDP type"})";
            auto data = std::make_shared<std::string>(kUnknownUdp);
            udp_socket.async_send_to(boost::asio::buffer(*data), from, [data](auto, auto) {});
        }
    }
    catch (const std::exception& e) {
        AppContext::instance().logger->error("[MessageDispatcher][UDP_PARSED] dispatch error: {}", e.what());
    }
}

void MessageDispatcher::register_handler(const std::string& type, HandlerFunc handler) {
    handlers_[type] = handler;
}

void MessageDispatcher::register_udp_handler(const std::string& type, UdpHandlerFunc handler) {
    udp_handlers_[type] = handler;
}
