#include <iostream>
#include "MessageDispatcher.h"
#include "SSLSession.h"
#include "DataHandler.h"
#include "Logger.h"
#include <memory>
#include "Utility.h"
#include "MessageHandlers/ChatHandler.h"


MessageDispatcher::MessageDispatcher(DataHandler* handler, SessionManager* sessionmanager) : handler_(handler), session_manager_(sessionmanager){
    // "login" 핸들러 등록
    register_handler("login", [this](std::shared_ptr<SSLSession> session, const nlohmann::json& msg) {
        std::string nickname = msg.value("nickname", "anonymity");

        if (nickname == "quit") {
            session->post_write(R"({"type":"logout","msg":"You are not logged in."})" "\n");
            g_logger->warn("[Connection reset] Client disconnected without a nickname. session_id : {} ", session->get_session_id());
            std::cerr << "Client disconnected without a nickname. " << " session_id: " << session->get_session_id() << std::endl;
            session->close_session();
			return;
        }

        // [중복 검사/처리]
        auto prev = session_manager_->find_session_by_nickname(nickname); //handler_->find_session_by_nickname(nickname);
        if (prev && prev != session) {
            prev->post_write(R"({"type":"error","msg":"Duplicate login ban"})" "\n");
            prev->close_session();
        }
        //handler_->register_nickname(nickname, session);
        g_logger->info("[on_login] {} 세션 등록 시도", nickname);
        session_manager_->register_nickname(nickname, session);
        g_logger->info("[on_login] 세션 등록 완료 후 login_success 전송: {}", nickname);

        session->set_nickname(nickname);
        session->on_nickname_registered(); // 닉네임 등록시 타이머 중지

        // zone 배정
        int default_zone = 1; // 정책에 따라 1번 존에 자동 배정
        handler_->assign_session_to_zone(session, default_zone);

        nlohmann::json notice;
        notice["type"] = "notice";
        notice["msg"] = nickname + " has entered.";
        handler_->broadcast(notice.dump() + "\n", session->get_session_id(), session);

        nlohmann::json login_msg;
        login_msg["type"] = "login_success";
        login_msg["nickname"] = nickname;
        session->post_write(login_msg.dump() + "\n");
        });

    register_handler("logout", [this](std::shared_ptr<SSLSession> session, const nlohmann::json& msg) {
        std::string nickname = session->get_nickname();
        if (nickname.empty()) {
            session->post_write(R"({"type":"logout","msg":"You are not logged in."})" "\n");
            g_logger->warn("[Connection reset] Client disconnected without a nickname. session_id : {} ", session->get_session_id());
            std::cerr << "Client disconnected without a nickname. " << " session_id: " << session->get_session_id() << std::endl;
            session->close_session();
            return;
        }
        nlohmann::json notice;
        notice["type"] = "notice";
        notice["msg"] = nickname + " has left.";
        handler_->broadcast(notice.dump() + "\n", session->get_session_id(), session);
        // 세션 종료
        session->post_write(R"({"type":"logout","msg":"You quit normally."})" "\n");
        g_logger->warn("[Connection reset] Client disconnected. nickname: {} session_id : {}", nickname, session->get_session_id());
        std::cerr << "Client disconnected. " << "  nickname: " << nickname << "  session_id: " << session->get_session_id() << std::endl;
        session->close_session();
		});

    // "chat" 핸들러 등록
    register_handler("chat",
    [this](std::shared_ptr<SSLSession> session, const nlohmann::json& msg) {
        chat_handler(session, msg, handler_);
        });
    //register_handler("chat", [this](std::shared_ptr<SSLSession> session, const nlohmann::json& msg) {
    //    std::string chat_msg = msg.value("msg", "");
    //    std::string nickname = session->get_nickname();

    //    nlohmann::json send_msg;
    //    send_msg["type"] = "chat";
    //    send_msg["from"] = nickname;
    //    send_msg["msg"] = chat_msg;

    //    handler_->broadcast(send_msg.dump() + "\n", session->get_session_id(), session);

    //    session->post_write(
    //        nlohmann::json{
    //            {"type", "notice"},
    //            {"msg", chat_msg + " 'Message sent completed.' "}
    //        }.dump() + "\n"
    //    );
    //    });

    //register_handler("ping", [this](std::shared_ptr<SSLSession> session, const nlohmann::json& msg) {
    //    session->on_ping_received(); // 아래에서 구현!
    //    // 원하면 "pong" 응답도 가능
    //    session->post_write(R"({"type":"pong"})" "\n");
    //    });

    register_handler("keepalive", [this](std::shared_ptr<SSLSession> session, const nlohmann::json& msg) {
        //std::cout << "[keepalive] session_id=" << session->get_session_id() << " ← 클라이언트로부터 keepalive 수신" << std::endl;
        g_logger->info("[keepalive] session_id= {}", session->get_session_id(), " ← 클라이언트로부터 keepalive 수신");
        session->update_alive_time();     // 글로벌 keepalive 타이머 갱신
        //session->on_pong_received();   // keepalive session에서 관리 할때 
        // (원하면) 응답 필요 없으면 생략 가능
        // session->post_write(R"({"type":"pong"})" "\n");
        });
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

    // 타입별 분기
    if (type == "udp") {
        // Echo 응답 (닉네임/메시지 포함)
        nlohmann::json response;
        response["type"] = "udp_reply";
        response["msg"] = "Echo(UDP): " + jmsg.value("msg", "");
        response["nickname"] = jmsg.value("nickname", "anonymity");

        auto data = std::make_shared<std::string>(response.dump());
        std::string nickname = jmsg.value("nickname", "anonymity");
        udp_socket.async_send_to(
            boost::asio::buffer(*data), from,
            [data, nickname](const boost::system::error_code& ec, std::size_t bytes) {
                if (ec) {
                    g_logger->warn("[UDP][send_to callback] Error 4: {} {}", nickname, ec.message());
                }
            }
        );
    }
    else if (type == "broadcast_udp") {
        if (handler_) {
            std::string sender_nickname = jmsg.value("nickname", "");
            handler_->udp_broadcast(raw_msg, udp_socket, sender_nickname);
        }
    }
    else if (type == "udp_register") {
        // 별도 응답 필요시 이 곳에서 UDP로 전송
        // 랜덤 토큰 생성 및 저장
        std::string udp_token = generate_random_token();
        session->set_udp_token(udp_token);

        nlohmann::json response;
        response["type"] = "udp_register_ack";
        response["msg"] = "UDP 인증 토큰 발급 및 endpoint registered";
        response["token"] = udp_token;
        response["nickname"] = jmsg.value("nickname", "anonymity");

        g_logger->info("[DEBUG][UDP] 응답 전송 - endpoint: {}:{}", from.address().to_string(), from.port());
        auto data = std::make_shared<std::string>(response.dump());
        udp_socket.async_send_to(
            boost::asio::buffer(*data), from,
            [data](const boost::system::error_code& ec, std::size_t bytes) {
                if (ec) g_logger->error("[UDP][send_to callback] Error 5: {}", ec.message());
            }
        );
    }
    else if (type == "broadcast_udp_zone") {
        if (!session) return;
        int zone_id = session->get_zone_id();
        if (auto handler = handler_) { // handler_는 DataHandler*
            auto zone = handler->get_zone(zone_id);
            if (zone) {
                // 브로드캐스트 내용 구성 (token 등은 제외!)
                nlohmann::json out;
                out["type"] = "broadcast_udp_zone";
                out["nickname"] = session->get_nickname();
                out["msg"] = jmsg.value("msg", "");
                std::string out_str = out.dump();

                // 수정: map<int, weak_ptr> 순회
                const auto& sessions = zone->sessions();
                for (auto it = sessions.begin(); it != sessions.end(); ++it) {
                    std::shared_ptr<SSLSession> s = it->second.lock();
                    if (!s) continue; // 세션 만료

                    if (s->get_nickname() == session->get_nickname())
                        continue; // 자기자신 제외

                    if (auto ep = s->get_udp_endpoint()) {
                        auto data = std::make_shared<std::string>(out_str);
                        udp_socket.async_send_to(
                            boost::asio::buffer(*data), *ep,
                            [data](const boost::system::error_code&, std::size_t) {});
                    }
                }
                return;
            }
            else {
                g_logger->warn("[UDP] Zone {} not found for broadcast_udp_zone", zone_id);
            }
        }
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
